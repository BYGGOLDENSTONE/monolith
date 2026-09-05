#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithFuzzyMatch.h"

namespace MonolithActionResultDetail
{
	static const TCHAR* ClassForCode(int32 Code)
	{
		switch (Code)
		{
		case FMonolithJsonUtils::ErrParseError:
		case FMonolithJsonUtils::ErrInvalidRequest:
		case FMonolithJsonUtils::ErrInvalidParams:
			return TEXT("invalid_param");
		case FMonolithJsonUtils::ErrMethodNotFound:
		case FMonolithJsonUtils::ErrNotFound:
			return TEXT("not_found");
		case FMonolithJsonUtils::ErrPreconditionFailed:
			return TEXT("precondition_failed");
		case FMonolithJsonUtils::ErrNotImplemented:
			return TEXT("not_implemented");
		case FMonolithJsonUtils::ErrOptionalDepUnavailable:
			return TEXT("optional_dep_unavailable");
		case FMonolithJsonUtils::ErrCoordinationBusy:
			return TEXT("lease_busy");
		case FMonolithJsonUtils::ErrInvalidLease:
			return TEXT("invalid_lease");
		default:
			return TEXT("engine_error");
		}
	}

	static FMonolithActionResult TypedError(const FString& Message, int32 Code)
	{
		TSharedPtr<FJsonObject> Data = FMonolithActionResult::NormalizeErrorData(Code, nullptr);
		Data->SetBoolField(TEXT("executed"), false);
		Data->SetBoolField(TEXT("retryable"), false);
		return FMonolithActionResult::Error(Message, Code).WithErrorData(Data);
	}
}

TSharedPtr<FJsonObject> FMonolithActionResult::NormalizeErrorData(int32 Code, const TSharedPtr<FJsonValue>& Data)
{
	TSharedPtr<FJsonObject> Normalized = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject>* Existing = nullptr;
	if (Data.IsValid() && Data->TryGetObject(Existing) && Existing && Existing->IsValid())
	{
		Normalized->Values = (*Existing)->Values;
	}
	else if (Data.IsValid())
	{
		// Preserve the Phase 0 convention used when attaching warnings to scalar data.
		Normalized->SetField(TEXT("value"), Data);
	}
	if (!Normalized->HasField(TEXT("class")))
	{
		Normalized->SetStringField(TEXT("class"), MonolithActionResultDetail::ClassForCode(Code));
	}
	return Normalized;
}

FMonolithActionResult FMonolithActionResult::NotFound(const FString& Kind, const FString& Needle, const TArray<FString>& Candidates)
{
	FMonolithActionResult Result = MonolithActionResultDetail::TypedError(
		FString::Printf(TEXT("%s '%s' not found"), *Kind, *Needle), FMonolithJsonUtils::ErrNotFound);
	TSharedPtr<FJsonObject> Data = Result.ErrorData->AsObject();
	Data->SetStringField(TEXT("kind"), Kind);
	Data->SetStringField(TEXT("needle"), Needle);
	TArray<TSharedPtr<FJsonValue>> Suggestions;
	for (const MonolithFuzzyMatchDetail::FFuzzyCandidate& Candidate :
		MonolithFuzzyMatchDetail::ScoreFuzzyMatches(Needle, Candidates, 3))
	{
		TSharedPtr<FJsonObject> Suggestion = MakeShared<FJsonObject>();
		Suggestion->SetStringField(Kind, Candidate.Key);
		Suggestion->SetNumberField(TEXT("score"), Candidate.Score);
		Suggestions.Add(MakeShared<FJsonValueObject>(Suggestion));
	}
	Data->SetArrayField(TEXT("suggestions"), Suggestions);
	return Result;
}

FMonolithActionResult FMonolithActionResult::InvalidParam(const FString& Name, const FString& Why)
{
	FMonolithActionResult Result = MonolithActionResultDetail::TypedError(
		FString::Printf(TEXT("Invalid parameter '%s': %s"), *Name, *Why), FMonolithJsonUtils::ErrInvalidParams);
	TSharedPtr<FJsonObject> Data = Result.ErrorData->AsObject();
	Data->SetStringField(TEXT("param"), Name);
	Data->SetStringField(TEXT("why"), Why);
	return Result;
}

FMonolithActionResult FMonolithActionResult::PreconditionFailed(const FString& What, const FString& NextAction)
{
	FMonolithActionResult Result = MonolithActionResultDetail::TypedError(What, FMonolithJsonUtils::ErrPreconditionFailed);
	TSharedPtr<FJsonObject> Data = Result.ErrorData->AsObject();
	Data->SetStringField(TEXT("precondition"), What);
	Data->SetStringField(TEXT("next_action"), NextAction);
	return Result;
}

FMonolithActionResult FMonolithActionResult::NotImplemented(const FString& Part)
{
	FMonolithActionResult Result = MonolithActionResultDetail::TypedError(
		FString::Printf(TEXT("Not implemented: %s"), *Part), FMonolithJsonUtils::ErrNotImplemented);
	TSharedPtr<FJsonObject> Data = Result.ErrorData->AsObject();
	Data->SetStringField(TEXT("reason"), TEXT("not_implemented"));
	Data->SetBoolField(TEXT("implemented"), false);
	Data->SetStringField(TEXT("part"), Part);
	return Result;
}

FMonolithActionResult FMonolithActionResult::EngineError(const FString& Message)
{
	return MonolithActionResultDetail::TypedError(Message, FMonolithJsonUtils::ErrInternalError);
}

FMonolithActionResult FMonolithActionResult::OptionalDepUnavailable(const FString& Dep)
{
	FMonolithActionResult Result = MonolithActionResultDetail::TypedError(
		FString::Printf(TEXT("Optional dependency '%s' is unavailable"), *Dep), FMonolithJsonUtils::ErrOptionalDepUnavailable);
	Result.ErrorData->AsObject()->SetStringField(TEXT("dep_name"), Dep);
	return Result;
}
