// SPDX-License-Identifier: MIT
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithToolRegistry.h"

namespace MonolithOptionalAvailabilityTest
{
TSharedPtr<FJsonObject> FindNamespace(const FMonolithActionResult& Inventory, const FString& Namespace)
{
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (!Inventory.bSuccess || !Inventory.Result.IsValid() ||
		!Inventory.Result->TryGetArrayField(TEXT("namespaces"), Rows)) return nullptr;
	for (const TSharedPtr<FJsonValue>& Value : *Rows)
	{
		const TSharedPtr<FJsonObject>* Row = nullptr;
		if (Value.IsValid() && Value->TryGetObject(Row) && (*Row)->GetStringField(TEXT("namespace")) == Namespace)
			return *Row;
	}
	return nullptr;
}

TSharedPtr<FJsonObject> FindDependency(const TSharedPtr<FJsonObject>& Row, const FString& Plugin)
{
	const TArray<TSharedPtr<FJsonValue>>* Dependencies = nullptr;
	if (!Row.IsValid() || !Row->TryGetArrayField(TEXT("optional_dependencies"), Dependencies)) return nullptr;
	for (const TSharedPtr<FJsonValue>& Value : *Dependencies)
	{
		const TSharedPtr<FJsonObject>* Dependency = nullptr;
		if (Value.IsValid() && Value->TryGetObject(Dependency) &&
			(*Dependency)->GetStringField(TEXT("required_plugin")) == Plugin) return *Dependency;
	}
	return nullptr;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithOptionalAvailabilityTest,
	"Monolith.Discover.OptionalDependencyAvailability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithOptionalAvailabilityTest::RunTest(const FString&)
{
	using namespace MonolithOptionalAvailabilityTest;
	auto& Registry = FMonolithToolRegistry::Get();
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString MissingNS = TEXT("__optional_missing_") + Suffix;
	const FString PresentNS = TEXT("__optional_present_") + Suffix;
	const FString MixedNS = TEXT("__optional_mixed_") + Suffix;
	const TArray<FString> OwnedNamespaces = {MissingNS, PresentNS, MixedNS};
	ON_SCOPE_EXIT
	{
		for (const FString& Namespace : OwnedNamespaces) Registry.UnregisterNamespace(Namespace);
	};
	const FString MissingPlugin = TEXT("MonolithFixtureMissingPlugin");
	const FString PresentPlugin = TEXT("MonolithFixturePresentPlugin");
	const FString MissingReason = TEXT("Fixture plugin is not enabled for this target");
	const auto AvailableHandler = FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>&)
	{
		auto Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("fixture_executed"), true);
		return FMonolithActionResult::Success(Result);
	});
	const auto MissingHandler = FMonolithActionHandler::CreateLambda([MissingPlugin](const TSharedPtr<FJsonObject>&)
	{
		return FMonolithActionResult::OptionalDepUnavailable(MissingPlugin);
	});
	const auto Schema = FParamSchemaBuilder().Build();
	Registry.RegisterAction(MissingNS, TEXT("optional_action"), TEXT("Missing fixture dependency"), MissingHandler, Schema);
	Registry.RegisterAction(PresentNS, TEXT("optional_action"), TEXT("Present fixture dependency"), AvailableHandler, Schema);
	Registry.RegisterAction(MixedNS, TEXT("base_action"), TEXT("Available base feature"), AvailableHandler, Schema);
	Registry.RegisterAction(MixedNS, TEXT("optional_action"), TEXT("Missing optional feature"), MissingHandler, Schema);
	Registry.SetOptionalDependencyAvailability(MissingNS, MissingPlugin, false, MissingReason);
	Registry.SetOptionalDependencyAvailability(PresentNS, PresentPlugin, true, FString());
	Registry.SetOptionalDependencyAvailability(MixedNS, MissingPlugin, false, MissingReason, false);
	Registry.SetOptionalDependencyAvailability(MixedNS, PresentPlugin, true, FString(), false);

	auto Inventory = Registry.ExecuteAction(TEXT("monolith"), TEXT("discover"), MakeShared<FJsonObject>());
	if (!TestTrue(TEXT("Inventory succeeds with missing optional dependencies"), Inventory.bSuccess && Inventory.Result.IsValid())) return false;
	for (const FString& Namespace : OwnedNamespaces)
	{
		const auto Row = FindNamespace(Inventory, Namespace);
		if (!TestTrue(TEXT("Optional fixture namespace remains discoverable"), Row.IsValid())) continue;
		const TSharedPtr<FJsonObject>* Availability = nullptr;
		if (!TestTrue(TEXT("Every namespace exposes availability"), Row->TryGetObjectField(TEXT("availability"), Availability))) continue;
		TestTrue(TEXT("Availability includes a reason string"), (*Availability)->HasTypedField<EJson::String>(TEXT("reason")));
		TestTrue(TEXT("Availability includes required plugin"), (*Availability)->HasField(TEXT("required_plugin")));
		TestEqual(TEXT("Only whole missing dependency makes namespace unavailable"),
			(*Availability)->GetBoolField(TEXT("available")), Namespace != MissingNS);
		if (Namespace == MissingNS)
		{
			TestEqual(TEXT("Whole namespace identifies missing plugin"), (*Availability)->GetStringField(TEXT("required_plugin")), MissingPlugin);
			TestEqual(TEXT("Whole namespace preserves dependency reason"), (*Availability)->GetStringField(TEXT("reason")), MissingReason);
			TestEqual(TEXT("Missing plugin still advertises registered action count"), Row->GetIntegerField(TEXT("action_count")), 1);
		}
		else if (Namespace == PresentNS)
		{
			TestEqual(TEXT("Available optional namespace still identifies plugin"), (*Availability)->GetStringField(TEXT("required_plugin")), PresentPlugin);
		}
		else
		{
			TestEqual(TEXT("Mixed namespace retains base and unavailable optional actions"), Row->GetIntegerField(TEXT("action_count")), 2);
			for (const FString& Plugin : {MissingPlugin, PresentPlugin})
			{
				const auto Dependency = FindDependency(Row, Plugin);
				if (!TestTrue(TEXT("Mixed namespace exposes each optional feature dependency"), Dependency.IsValid())) continue;
				TestEqual(TEXT("Mixed dependency reports its own state"), Dependency->GetBoolField(TEXT("available")), Plugin == PresentPlugin);
				if (Plugin == MissingPlugin)
					TestEqual(TEXT("Missing feature reason survives discovery"), Dependency->GetStringField(TEXT("reason")), MissingReason);
			}
		}
	}

	for (const FString& Namespace : {MissingNS, MixedNS})
	{
		const auto Result = Registry.ExecuteAction(Namespace, TEXT("optional_action"), MakeShared<FJsonObject>());
		TestFalse(TEXT("Missing optional action fails explicitly"), Result.bSuccess);
		TestEqual(TEXT("Missing optional action uses dependency code"), Result.ErrorCode, FMonolithJsonUtils::ErrOptionalDepUnavailable);
		if (TestTrue(TEXT("Missing dependency has structured data"), Result.ErrorData.IsValid()))
		{
			const auto Data = Result.ErrorData->AsObject();
			if (!TestTrue(TEXT("Dependency error data is an object"), Data.IsValid())) continue;
			TestEqual(TEXT("Dependency error names required plugin"), Data->GetStringField(TEXT("dep_name")), MissingPlugin);
			TestEqual(TEXT("Dependency error has optional class"), Data->GetStringField(TEXT("class")), FString(TEXT("optional_dep_unavailable")));
			TestFalse(TEXT("Missing dependency executes no mutation"), Data->GetBoolField(TEXT("executed")));
		}
	}
	const auto BaseResult = Registry.ExecuteAction(MixedNS, TEXT("base_action"), MakeShared<FJsonObject>());
	TestTrue(TEXT("Missing optional feature does not disable mixed namespace base action"), BaseResult.bSuccess &&
		BaseResult.Result.IsValid() && BaseResult.Result->GetBoolField(TEXT("fixture_executed")));
	const auto PresentResult = Registry.ExecuteAction(PresentNS, TEXT("optional_action"), MakeShared<FJsonObject>());
	TestTrue(TEXT("Available optional action still executes"), PresentResult.bSuccess &&
		PresentResult.Result.IsValid() && PresentResult.Result->GetBoolField(TEXT("fixture_executed")));

	// Updating a dependency replaces its metadata and changes the next discovery.
	Registry.SetOptionalDependencyAvailability(MissingNS, MissingPlugin, true, FString());
	const auto Updated = Registry.GetOptionalDependencyAvailability(MissingNS);
	if (TestEqual(TEXT("Updating dependency never duplicates metadata"), Updated.Num(), 1))
	{
		TestTrue(TEXT("Updated dependency is available"), Updated[0].bAvailable);
		TestTrue(TEXT("Whole-namespace scope retained"), Updated[0].bWholeNamespace);
		TestEqual(TEXT("Dependency identity retained"), Updated[0].RequiredPlugin, MissingPlugin);
		TestTrue(TEXT("Old unavailable reason cleared"), Updated[0].Reason.IsEmpty());
	}
	Inventory = Registry.ExecuteAction(TEXT("monolith"), TEXT("discover"), MakeShared<FJsonObject>());
	const auto UpdatedRow = FindNamespace(Inventory, MissingNS);
	const TSharedPtr<FJsonObject>* UpdatedAvailability = nullptr;
	if (TestTrue(TEXT("Updated metadata remains discoverable"), UpdatedRow.IsValid() &&
		UpdatedRow->TryGetObjectField(TEXT("availability"), UpdatedAvailability)))
		TestTrue(TEXT("Discovery immediately reflects dependency update"), (*UpdatedAvailability)->GetBoolField(TEXT("available")));

	for (const FString& Namespace : OwnedNamespaces)
	{
		Registry.UnregisterNamespace(Namespace);
		TestEqual(TEXT("Unregistration removes dependency metadata"), Registry.GetOptionalDependencyAvailability(Namespace).Num(), 0);
		TestEqual(TEXT("Unregistration removes fixture actions"), Registry.GetActions(Namespace).Num(), 0);
	}
	return true;
}

#endif
