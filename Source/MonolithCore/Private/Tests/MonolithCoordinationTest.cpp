// SPDX-License-Identifier: MIT
#include "MonolithCoordination.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace MonolithCoordinationTest
{
	static TSharedPtr<FJsonObject> Request(const FString& Operation, const FString& Token = FString())
	{
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("operation"), Operation);
		if (!Token.IsEmpty()) { Params->SetStringField(TEXT("_lease_token"), Token); }
		return Params;
	}
	static TSharedPtr<FJsonObject> Acquire(const FString& Owner = TEXT("agent-a"), double TTL = 10.0)
	{
		auto Params = Request(TEXT("acquire"));
		Params->SetStringField(TEXT("owner"), Owner);
		Params->SetNumberField(TEXT("ttl_seconds"), TTL);
		return Params;
	}
	static FString TokenOf(const FMonolithActionResult& Result)
	{
		FString Token;
		if (Result.Result.IsValid()) { Result.Result->TryGetStringField(TEXT("_lease_token"), Token); }
		return Token;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLeaseLifecycleTest, "Monolith.Coordination.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLeaseLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace MonolithCoordinationTest;
	double Now = 100;
	FMonolithCoordination Coordinator([&Now] { return Now; });
	TestFalse(TEXT("Initial state is idle"), Coordinator.Handle(Request(TEXT("status"))).Result->GetBoolField(TEXT("active")));
	TestFalse(TEXT("Empty owner rejected"), Coordinator.Handle(Acquire(TEXT("  "))).bSuccess);
	TestFalse(TEXT("Short TTL rejected"), Coordinator.Handle(Acquire(TEXT("a"), 9)).bSuccess);
	TestFalse(TEXT("Long TTL rejected"), Coordinator.Handle(Acquire(TEXT("a"), 601)).bSuccess);
	auto Acquired = Coordinator.Handle(Acquire());
	TestTrue(TEXT("Lease acquired"), Acquired.bSuccess);
	const FString Token = TokenOf(Acquired);
	TestFalse(TEXT("Token returned"), Token.IsEmpty());
	TestFalse(TEXT("Status never leaks token"), Coordinator.Handle(Request(TEXT("status"))).Result->HasField(TEXT("_lease_token")));
	TestEqual(TEXT("Status rejects explicit wrong token"), Coordinator.Handle(Request(TEXT("status"), TEXT("wrong"))).ErrorCode, -32011);
	TestEqual(TEXT("Competing owner refused"), Coordinator.Handle(Acquire(TEXT("agent-b"))).ErrorCode, -32010);
	TestEqual(TEXT("Same label is not authorization"), Coordinator.Handle(Acquire()).ErrorCode, -32010);
	TestEqual(TEXT("Foreign release refused"), Coordinator.Handle(Request(TEXT("release"), TEXT("wrong"))).ErrorCode, -32011);
	TestEqual(TEXT("Missing renewal token refused"), Coordinator.Handle(Request(TEXT("renew"))).ErrorCode, -32011);
	Now = 109;
	auto Renewal = Request(TEXT("renew"), Token);
	Renewal->SetNumberField(TEXT("ttl_seconds"), 10);
	TestTrue(TEXT("Owner can renew"), Coordinator.Handle(Renewal).bSuccess);
	Now = 110;
	TestTrue(TEXT("Renewal extended expiration"), Coordinator.Handle(Request(TEXT("status"))).Result->GetBoolField(TEXT("active")));
	Now = 119;
	TestFalse(TEXT("Expiry at exact deadline"), Coordinator.Handle(Request(TEXT("status"))).Result->GetBoolField(TEXT("active")));
	TestEqual(TEXT("Expired renewal refused"), Coordinator.Handle(Renewal).ErrorCode, -32011);
	FString ExecutionToken;
	TestEqual(TEXT("Stale token cannot execute when idle"), Coordinator.CheckAccess(TEXT("editor"), TEXT("test"), Request(TEXT("unused"), Token), ExecutionToken).ErrorCode, -32011);
	const FString NewToken = TokenOf(Coordinator.Handle(Acquire(TEXT("agent-b"))));
	TestTrue(TEXT("Reacquire rotates token"), !NewToken.IsEmpty() && NewToken != Token);
	TestTrue(TEXT("Owner can release"), Coordinator.Handle(Request(TEXT("release"), NewToken)).bSuccess);
	TestEqual(TEXT("Released token remains stale"), Coordinator.Handle(Request(TEXT("release"), NewToken)).ErrorCode, -32011);
	TestFalse(TEXT("Released state idle"), Coordinator.Handle(Request(TEXT("status"))).Result->GetBoolField(TEXT("active")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLeaseGuardTest, "Monolith.Coordination.GuardAndNestedDispatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLeaseGuardTest::RunTest(const FString& Parameters)
{
	using namespace MonolithCoordinationTest;
	double Now = 0;
	FMonolithCoordination Coordinator([&Now] { return Now; });
	auto Empty = MakeShared<FJsonObject>();
	FString ExecutionToken;
	TestTrue(TEXT("Legacy unleased call allowed when idle"), Coordinator.CheckAccess(TEXT("editor"), TEXT("mutate"), Empty, ExecutionToken).bSuccess);
	const FString Token = TokenOf(Coordinator.Handle(Acquire()));
	auto Denied = Coordinator.CheckAccess(TEXT("editor"), TEXT("mutate"), Empty, ExecutionToken);
	TestEqual(TEXT("Untagged call denied"), Denied.ErrorCode, -32010);
	TestTrue(TEXT("Structured busy data"), Denied.ErrorData.IsValid());
	TestTrue(TEXT("Discovery available while reserved"), Coordinator.CheckAccess(TEXT("monolith"), TEXT("discover"), Empty, ExecutionToken).bSuccess);
	TestTrue(TEXT("Status available while reserved"), Coordinator.CheckAccess(TEXT("monolith"), TEXT("status"), Empty, ExecutionToken).bSuccess);
	TestTrue(TEXT("Guide available while reserved"), Coordinator.CheckAccess(TEXT("monolith"), TEXT("guide"), Empty, ExecutionToken).bSuccess);
	TestEqual(TEXT("Exempt discovery rejects explicit bad token"), Coordinator.CheckAccess(TEXT("monolith"), TEXT("discover"), Request(TEXT("unused"), TEXT("wrong")), ExecutionToken).ErrorCode, -32011);
	TestTrue(TEXT("Schema metadata available while reserved"), Coordinator.CheckAccess(TEXT("describe"), TEXT("action_schema"), Empty, ExecutionToken).bSuccess);
	TestFalse(TEXT("Other describe actions require lease"), Coordinator.CheckAccess(TEXT("describe"), TEXT("class"), Empty, ExecutionToken).bSuccess);
	TestFalse(TEXT("Core update requires lease"), Coordinator.CheckAccess(TEXT("monolith"), TEXT("update"), Empty, ExecutionToken).bSuccess);
	auto OwnedParams = Request(TEXT("unused"), Token);
	TestTrue(TEXT("Matching token allowed"), Coordinator.CheckAccess(TEXT("editor"), TEXT("mutate"), OwnedParams, ExecutionToken).bSuccess);
	{
		FMonolithCoordination::FExecutionScope Scope(Coordinator, ExecutionToken);
		Now = 15;
		TestTrue(TEXT("Nested calls inherit even while current action crosses deadline"), Coordinator.CheckAccess(TEXT("editor"), TEXT("nested"), Empty, ExecutionToken).bSuccess);
		TestEqual(TEXT("Inherited token"), ExecutionToken, Token);
		TestEqual(TEXT("External request cannot inherit during modal reentry"), Coordinator.CheckAccess(TEXT("editor"), TEXT("nested"), Empty, ExecutionToken, false).ErrorCode, -32010);
		TestEqual(TEXT("Even owner's external request cannot reenter running mutation"), Coordinator.CheckAccess(TEXT("editor"), TEXT("nested"), OwnedParams, ExecutionToken, false).ErrorCode, -32010);
		TestFalse(TEXT("Explicit bad child token cannot inherit"), Coordinator.CheckAccess(TEXT("editor"), TEXT("nested"), Request(TEXT("unused"), TEXT("wrong")), ExecutionToken).bSuccess);
		TestEqual(TEXT("No takeover during running action"), Coordinator.Handle(Acquire(TEXT("agent-b"))).ErrorCode, -32010);
		TestFalse(TEXT("No release during running action"), Coordinator.Handle(Request(TEXT("release"), Token)).bSuccess);
	}
	TestEqual(TEXT("Expired token denied after action returns"), Coordinator.CheckAccess(TEXT("editor"), TEXT("mutate"), OwnedParams, ExecutionToken).ErrorCode, -32011);
	TestTrue(TEXT("No inherited context leaks after scope"), Coordinator.CheckAccess(TEXT("editor"), TEXT("mutate"), Empty, ExecutionToken).bSuccess);
	TestTrue(TEXT("Context cleared"), ExecutionToken.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLeaseCompetingOwnersTest, "Monolith.Coordination.CompetingOwners",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLeaseCompetingOwnersTest::RunTest(const FString& Parameters)
{
	using namespace MonolithCoordinationTest;
	FMonolithCoordination Coordinator([] { return 0.0; });
	TArray<FMonolithActionResult> Results;
	Results.SetNum(8);
	ParallelFor(Results.Num(), [&](int32 Index)
	{
		Results[Index] = Coordinator.Handle(Acquire(FString::Printf(TEXT("agent-%d"), Index)));
	});
	int32 Winners = 0;
	int32 Busy = 0;
	for (const auto& Result : Results)
	{
		Winners += Result.bSuccess ? 1 : 0;
		Busy += Result.ErrorCode == -32010 ? 1 : 0;
	}
	TestEqual(TEXT("One winner across simultaneous acquisitions"), Winners, 1);
	TestEqual(TEXT("All competing clients receive busy"), Busy, 7);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLeaseRegistryTest, "Monolith.Coordination.RegistryIntegration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithLeaseRegistryTest::RunTest(const FString& Parameters)
{
	using namespace MonolithCoordinationTest;
	auto& Registry = FMonolithToolRegistry::Get();
	auto& Coordinator = FMonolithCoordination::Get();
	if (Coordinator.Handle(Request(TEXT("status"))).Result->GetBoolField(TEXT("active")))
	{
		AddWarning(TEXT("RegistryIntegration skipped: a live client holds the editor lease."));
		return true;
	}
	const FString Namespace = TEXT("__coordination_test");
	int32 Executions = 0;
	bool bTokenStripped = true;
	bool bReentrantRejected = false;
	Registry.RegisterAction(Namespace, TEXT("inner"), TEXT("Test action"), FMonolithActionHandler::CreateLambda(
		[&](const TSharedPtr<FJsonObject>& Params)
		{
			++Executions;
			bTokenStripped &= !Params->HasField(TEXT("_lease_token"));
			return FMonolithActionResult::Success(MakeShared<FJsonObject>());
		}), MakeShared<FJsonObject>());
	Registry.RegisterAction(Namespace, TEXT("outer"), TEXT("Nested test action"), FMonolithActionHandler::CreateLambda(
		[&](const TSharedPtr<FJsonObject>& Params)
		{
			bTokenStripped &= !Params->HasField(TEXT("_lease_token"));
			const auto Reentrant = Registry.ExecuteAction(Namespace, TEXT("inner"), MakeShared<FJsonObject>(), false);
			bReentrantRejected = !Reentrant.bSuccess && Reentrant.ErrorCode == -32010;
			return Registry.ExecuteAction(Namespace, TEXT("inner"), MakeShared<FJsonObject>());
		}));
	FString Token;
	ON_SCOPE_EXIT
	{
		if (!Token.IsEmpty()) { Coordinator.Handle(Request(TEXT("release"), Token)); }
		Registry.UnregisterNamespace(Namespace);
	};
	const auto Acquired = Registry.ExecuteAction(TEXT("monolith"), TEXT("coordination"), Acquire(TEXT("automation"), 120));
	Token = TokenOf(Acquired);
	TestTrue(TEXT("Tool registered and acquire executes"), Acquired.bSuccess);
	if (Token.IsEmpty()) { return false; }
	auto Params = MakeShared<FJsonObject>();
	TestFalse(TEXT("Busy call rejected through registry"), Registry.ExecuteAction(Namespace, TEXT("inner"), Params).bSuccess);
	TestEqual(TEXT("Rejected handler never called"), Executions, 0);
	Params->SetStringField(TEXT("_lease_token"), Token);
	TestTrue(TEXT("Nested registry dispatch inherits owner"), Registry.ExecuteAction(Namespace, TEXT("outer"), Params).bSuccess);
	TestEqual(TEXT("Nested action executed once"), Executions, 1);
	TestTrue(TEXT("Metadata removed before handlers"), bTokenStripped);
	TestTrue(TEXT("External modal reentry never executes or inherits"), bReentrantRejected);
	TestTrue(TEXT("Caller params remain unchanged"), Params->HasField(TEXT("_lease_token")));
	TestFalse(TEXT("Later unrelated call cannot inherit context"), Registry.ExecuteAction(Namespace, TEXT("inner"), MakeShared<FJsonObject>()).bSuccess);
	auto WorkerResult = Async(EAsyncExecution::ThreadPool, [&]
	{
		return Registry.ExecuteAction(Namespace, TEXT("inner"), Params);
	}).Get();
	TestFalse(TEXT("Worker thread execution rejected"), WorkerResult.bSuccess);
	TestEqual(TEXT("Worker never touched handler"), Executions, 1);
	TestTrue(TEXT("Release executes through tool"), Registry.ExecuteAction(TEXT("monolith"), TEXT("coordination"), Request(TEXT("release"), Token)).bSuccess);
	TestEqual(TEXT("Stale request fenced after release"), Registry.ExecuteAction(Namespace, TEXT("inner"), Params).ErrorCode, -32011);
	Token.Reset();
	return true;
}
#endif
