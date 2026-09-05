// SPDX-License-Identifier: MIT
#include "MonolithCoordination.h"
#include "MonolithJsonUtils.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"

FMonolithCoordination& FMonolithCoordination::Get()
{
	static FMonolithCoordination Instance;
	return Instance;
}

FMonolithCoordination::FMonolithCoordination(TFunction<double()> InClock)
	: Clock(InClock ? MoveTemp(InClock) : TFunction<double()>([] { return FPlatformTime::Seconds(); }))
{
}

void FMonolithCoordination::RegisterTool()
{
	auto Schema = MakeShared<FJsonObject>();
	const auto AddParam = [&Schema](const TCHAR* Name, const TCHAR* Type, const TCHAR* Description)
	{
		auto Param = MakeShared<FJsonObject>();
		Param->SetStringField(TEXT("type"), Type);
		Param->SetStringField(TEXT("description"), Description);
		Schema->SetObjectField(Name, Param);
	};
	AddParam(TEXT("operation"), TEXT("string"), TEXT("status (default), acquire, renew, or release."));
	AddParam(TEXT("owner"), TEXT("string"), TEXT("Required for acquire: human-readable agent/workflow label, max 128 characters. A label is not proof of ownership."));
	AddParam(TEXT("ttl_seconds"), TEXT("number"), TEXT("Lease duration, 10..600 seconds; acquire defaults to 120, renew retains the current duration when omitted. Renew before expiry."));
	AddParam(TEXT("_lease_token"), TEXT("string"), TEXT("Opaque token returned by acquire; required for renew/release. Also pass in every protected tool call's params."));
	TArray<TSharedPtr<FJsonValue>> Operations;
	for (const TCHAR* Operation : { TEXT("status"), TEXT("acquire"), TEXT("renew"), TEXT("release") })
	{
		Operations.Add(MakeShared<FJsonValueString>(Operation));
	}
	Schema->GetObjectField(TEXT("operation"))->SetArrayField(TEXT("enum"), Operations);
	Schema->GetObjectField(TEXT("operation"))->SetStringField(TEXT("default"), TEXT("status"));
	Schema->GetObjectField(TEXT("ttl_seconds"))->SetNumberField(TEXT("minimum"), 10);
	Schema->GetObjectField(TEXT("ttl_seconds"))->SetNumberField(TEXT("maximum"), 600);
	FMonolithToolRegistry::Get().RegisterAction(TEXT("monolith"), TEXT("coordination"),
		TEXT("Coordinate multiple agents sharing one Unreal Editor. Acquire an exclusive workflow lease, pass _lease_token in domain tool params, renew before expiry, release in finally. Status is public and never reveals tokens. Busy/stale requests fail before execution. This does not run UObjects concurrently or undo completed work."),
		FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>& Params) { return Get().Handle(Params); }), Schema);
}

void FMonolithCoordination::ExpireLocked()
{
	if (!LeaseToken.IsEmpty() && ActiveExecutions == 0 && Clock() >= ExpiresAt)
	{
		Owner.Reset();
		LeaseToken.Reset();
		ExpiresAt = 0;
	}
}

TSharedPtr<FJsonObject> FMonolithCoordination::StatusLocked() const
{
	auto Status = MakeShared<FJsonObject>();
	Status->SetBoolField(TEXT("active"), !LeaseToken.IsEmpty());
	Status->SetStringField(TEXT("owner"), Owner);
	Status->SetNumberField(TEXT("remaining_seconds"), LeaseToken.IsEmpty() ? 0.0 : FMath::Max(0.0, ExpiresAt - Clock()));
	Status->SetBoolField(TEXT("executing"), ActiveExecutions > 0);
	Status->SetStringField(TEXT("scope"), TEXT("editor_process"));
	Status->SetBoolField(TEXT("parallel_uobject_execution"), false);
	return Status;
}

FMonolithActionResult FMonolithCoordination::ErrorLocked(const TCHAR* Reason, const TCHAR* Message, int32 Code) const
{
	auto Data = StatusLocked();
	Data->SetStringField(TEXT("reason"), Reason);
	Data->SetBoolField(TEXT("executed"), false);
	Data->SetBoolField(TEXT("retryable"), Code == FMonolithJsonUtils::ErrCoordinationBusy);
	if (Code == FMonolithJsonUtils::ErrCoordinationBusy)
	{
		Data->SetNumberField(TEXT("retry_after_seconds"), FMath::Max(1.0, ExpiresAt - Clock()));
	}
	return FMonolithActionResult::Error(Message, Code).WithErrorData(Data);
}

FMonolithActionResult FMonolithCoordination::Handle(const TSharedPtr<FJsonObject>& Params)
{
	FScopeLock Lock(&Mutex);
	ExpireLocked();
	FString Operation = TEXT("status");
	if (Params.IsValid() && Params->HasField(TEXT("operation")) && !Params->TryGetStringField(TEXT("operation"), Operation))
	{
		return ErrorLocked(TEXT("invalid_operation"), TEXT("operation must be status, acquire, renew, or release."), -32602);
	}
	if (Operation != TEXT("status") && Operation != TEXT("acquire") && Operation != TEXT("renew") && Operation != TEXT("release"))
	{
		return ErrorLocked(TEXT("invalid_operation"), TEXT("operation must be status, acquire, renew, or release."), -32602);
	}

	FString Token;
	const bool bTokenProvided = Params.IsValid() && Params->HasField(TEXT("_lease_token"));
	if (bTokenProvided && (!Params->TryGetStringField(TEXT("_lease_token"), Token) || Token.IsEmpty()))
	{
		return ErrorLocked(TEXT("invalid_lease"), TEXT("_lease_token must be a nonempty token returned by acquire."), FMonolithJsonUtils::ErrInvalidLease);
	}
	if ((Operation != TEXT("acquire") && Operation != TEXT("status")) || bTokenProvided)
	{
		if (LeaseToken.IsEmpty() || Token != LeaseToken)
		{
			return ErrorLocked(TEXT("invalid_lease"), TEXT("Lease token is missing, stale, or belongs to another workflow. Acquire a new lease before continuing."), FMonolithJsonUtils::ErrInvalidLease);
		}
	}
	if (Operation == TEXT("status"))
	{
		return FMonolithActionResult::Success(StatusLocked());
	}
	if (Operation == TEXT("release"))
	{
		if (ActiveExecutions > 0)
		{
			return ErrorLocked(TEXT("lease_executing"), TEXT("Cannot release a lease inside a running leased action. Release after that action returns."), FMonolithJsonUtils::ErrCoordinationBusy);
		}
		Owner.Reset();
		LeaseToken.Reset();
		ExpiresAt = 0;
		return FMonolithActionResult::Success(StatusLocked());
	}

	double TTL = Operation == TEXT("renew") ? LeaseTTLSeconds : 120.0;
	if (Params.IsValid() && Params->HasField(TEXT("ttl_seconds")) &&
		(!Params->TryGetNumberField(TEXT("ttl_seconds"), TTL) || !FMath::IsFinite(TTL) || TTL < 10.0 || TTL > 600.0))
	{
		return ErrorLocked(TEXT("invalid_ttl"), TEXT("ttl_seconds must be a finite number between 10 and 600."), -32602);
	}
	if (Operation == TEXT("acquire"))
	{
		FString RequestedOwner;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("owner"), RequestedOwner) ||
			RequestedOwner.TrimStartAndEnd().IsEmpty() || RequestedOwner.Len() > 128)
		{
			return ErrorLocked(TEXT("invalid_owner"), TEXT("acquire requires a nonempty owner label of at most 128 characters."), -32602);
		}
		if (!LeaseToken.IsEmpty())
		{
			return ErrorLocked(TEXT("lease_busy"), TEXT("Another workflow holds the editor lease. Wait for release/expiry; an owner label cannot reclaim it. Use renew with its token if you own it."), FMonolithJsonUtils::ErrCoordinationBusy);
		}
		Owner = RequestedOwner.TrimStartAndEnd();
		LeaseToken = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}
	LeaseTTLSeconds = TTL;
	ExpiresAt = Clock() + TTL;
	bGraceGranted = false;
	auto Result = StatusLocked();
	Result->SetStringField(TEXT("_lease_token"), LeaseToken);
	return FMonolithActionResult::Success(Result);
}

bool FMonolithCoordination::IsExempt(const FString& Namespace, const FString& Action)
{
	return (Namespace == TEXT("monolith") && (Action == TEXT("discover") || Action == TEXT("status") || Action == TEXT("guide"))) ||
		(Namespace == TEXT("describe") && Action == TEXT("action_schema"));
}

FMonolithActionResult FMonolithCoordination::CheckAccess(const FString& Namespace, const FString& Action,
	const TSharedPtr<FJsonObject>& Params, FString& OutExecutionToken, bool bInheritLeaseContext)
{
	FScopeLock Lock(&Mutex);
	ExpireLocked();
	OutExecutionToken.Reset();
	// A modal editor handler may pump the task graph. A new HTTP request is
	// never a trusted nested pipeline call, even on the same game thread.
	if (!bInheritLeaseContext && DispatchDepth > 0)
	{
		return ErrorLocked(TEXT("editor_executing"), TEXT("An editor action is still executing. This reentrant request did not execute; retry after it completes."), FMonolithJsonUtils::ErrCoordinationBusy);
	}
	// Coordination validates its own token. In particular acquire must remain
	// callable by a competing client so it receives a useful busy response.
	if (Namespace == TEXT("monolith") && Action == TEXT("coordination"))
	{
		return FMonolithActionResult::Success(nullptr);
	}
	const bool bExplicitToken = Params.IsValid() && Params->HasField(TEXT("_lease_token"));
	FString Token = bInheritLeaseContext ? ExecutionToken : FString();
	if (bExplicitToken && (!Params->TryGetStringField(TEXT("_lease_token"), Token) || Token.IsEmpty()))
	{
		return ErrorLocked(TEXT("invalid_lease"), TEXT("_lease_token must be a nonempty token returned by acquire."), FMonolithJsonUtils::ErrInvalidLease);
	}
	if (!Token.IsEmpty())
	{
		if (LeaseToken.IsEmpty() || Token != LeaseToken)
		{
			return ErrorLocked(TEXT("invalid_lease"), TEXT("Lease token is stale or invalid; this call did not execute. Acquire a new lease and inspect state before continuing."), FMonolithJsonUtils::ErrInvalidLease);
		}
		OutExecutionToken = Token;
	}
	else if (!LeaseToken.IsEmpty() && !IsExempt(Namespace, Action))
	{
		return ErrorLocked(TEXT("lease_busy"), TEXT("Editor is reserved by another workflow. This call did not execute. Pass the owner's _lease_token in params or wait for release/expiry."), FMonolithJsonUtils::ErrCoordinationBusy);
	}
	return FMonolithActionResult::Success(nullptr);
}

void FMonolithCoordination::EndLeasedExecutionLocked()
{
	--ActiveExecutions;
	const double Now = Clock();
	if (ActiveExecutions == 0 && !LeaseToken.IsEmpty() && Now >= ExpiresAt && !bGraceGranted)
	{
		ExpiresAt = Now + FMath::Min(30.0, LeaseTTLSeconds / 4.0);
		bGraceGranted = true;
	}
}

FMonolithCoordination::FBatchScope::FBatchScope(FMonolithCoordination& InCoordinator)
	: Coordinator(InCoordinator)
{
	check(IsInGameThread());
	FScopeLock Lock(&Coordinator.Mutex);
	PreviousBatch = Coordinator.CurrentBatch;
	Coordinator.CurrentBatch = this;
}

FMonolithCoordination::FBatchScope::~FBatchScope()
{
	FScopeLock Lock(&Coordinator.Mutex);
	check(Coordinator.CurrentBatch == this);
	Coordinator.CurrentBatch = PreviousBatch;
	if (bPinned) { Coordinator.EndLeasedExecutionLocked(); }
}

FMonolithCoordination::FExecutionScope::FExecutionScope(FMonolithCoordination& InCoordinator, const FString& Token)
	: Coordinator(InCoordinator), bLeased(!Token.IsEmpty())
{
	check(IsInGameThread());
	FScopeLock Lock(&Coordinator.Mutex);
	PreviousToken = Coordinator.ExecutionToken;
	Coordinator.ExecutionToken = Token;
	++Coordinator.DispatchDepth;
	if (bLeased)
	{
		++Coordinator.ActiveExecutions;
		if (Coordinator.CurrentBatch && !Coordinator.CurrentBatch->bPinned)
		{
			Coordinator.CurrentBatch->bPinned = true;
			++Coordinator.ActiveExecutions;
		}
	}
}

FMonolithCoordination::FExecutionScope::~FExecutionScope()
{
	FScopeLock Lock(&Coordinator.Mutex);
	Coordinator.ExecutionToken = MoveTemp(PreviousToken);
	--Coordinator.DispatchDepth;
	if (bLeased) { Coordinator.EndLeasedExecutionLocked(); }
}
