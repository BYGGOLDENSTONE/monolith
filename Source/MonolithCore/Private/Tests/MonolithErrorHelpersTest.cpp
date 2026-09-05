#include "Misc/AutomationTest.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithErrorHelperPayloadsTest,
	"Monolith.Core.ErrorHelpers.Payloads", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithErrorHelperPayloadsTest::RunTest(const FString& Parameters)
{
	struct FCase
	{
		FMonolithActionResult Result;
		int32 Code;
		const TCHAR* Class;
	};
	const FCase Cases[] = {
		{ FMonolithActionResult::NotFound(TEXT("asset"), TEXT("Missing")), FMonolithJsonUtils::ErrNotFound, TEXT("not_found") },
		{ FMonolithActionResult::InvalidParam(TEXT("count"), TEXT("must be positive")), FMonolithJsonUtils::ErrInvalidParams, TEXT("invalid_param") },
		{ FMonolithActionResult::PreconditionFailed(TEXT("index not mined"), TEXT("risk.mine")), FMonolithJsonUtils::ErrPreconditionFailed, TEXT("precondition_failed") },
		{ FMonolithActionResult::NotImplemented(TEXT("scoring")), FMonolithJsonUtils::ErrNotImplemented, TEXT("not_implemented") },
		{ FMonolithActionResult::EngineError(TEXT("Compilation failed")), FMonolithJsonUtils::ErrInternalError, TEXT("engine_error") },
		{ FMonolithActionResult::OptionalDepUnavailable(TEXT("CommonUI")), FMonolithJsonUtils::ErrOptionalDepUnavailable, TEXT("optional_dep_unavailable") }
	};
	for (const FCase& Case : Cases)
	{
		TestFalse(FString::Printf(TEXT("%s is an error"), Case.Class), Case.Result.bSuccess);
		TestEqual(FString::Printf(TEXT("%s code"), Case.Class), Case.Result.ErrorCode, Case.Code);
		TestFalse(TEXT("Error has a message"), Case.Result.ErrorMessage.IsEmpty());
		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (!TestTrue(TEXT("Helper data is an object"), Case.Result.ErrorData.IsValid()
			&& Case.Result.ErrorData->TryGetObject(Data) && Data && Data->IsValid())) return false;
		TestEqual(TEXT("Helper class"), (*Data)->GetStringField(TEXT("class")), FString(Case.Class));
		TestTrue(TEXT("Executed has boolean type"), (*Data)->HasTypedField<EJson::Boolean>(TEXT("executed")));
		TestFalse(TEXT("Helper defaults to unexecuted"), (*Data)->GetBoolField(TEXT("executed")));
		TestTrue(TEXT("Retryable has boolean type"), (*Data)->HasTypedField<EJson::Boolean>(TEXT("retryable")));
		TestFalse(TEXT("Helper does not promise automatic retry"), (*Data)->GetBoolField(TEXT("retryable")));
	}
	TestEqual(TEXT("NotFound retains kind"), Cases[0].Result.ErrorData->AsObject()->GetStringField(TEXT("kind")), FString(TEXT("asset")));
	TestEqual(TEXT("NotFound retains needle"), Cases[0].Result.ErrorData->AsObject()->GetStringField(TEXT("needle")), FString(TEXT("Missing")));
	TestEqual(TEXT("No candidates gives empty suggestions"), Cases[0].Result.ErrorData->AsObject()->GetArrayField(TEXT("suggestions")).Num(), 0);
	TestEqual(TEXT("InvalidParam names parameter"), Cases[1].Result.ErrorData->AsObject()->GetStringField(TEXT("param")), FString(TEXT("count")));
	TestEqual(TEXT("InvalidParam explains constraint"), Cases[1].Result.ErrorData->AsObject()->GetStringField(TEXT("why")), FString(TEXT("must be positive")));
	TestEqual(TEXT("Precondition preserves requirement"), Cases[2].Result.ErrorData->AsObject()->GetStringField(TEXT("precondition")), FString(TEXT("index not mined")));
	TestEqual(TEXT("Precondition suggests next action"), Cases[2].Result.ErrorData->AsObject()->GetStringField(TEXT("next_action")), FString(TEXT("risk.mine")));
	TestEqual(TEXT("NotImplemented preserves Phase 0 reason"), Cases[3].Result.ErrorData->AsObject()->GetStringField(TEXT("reason")), FString(TEXT("not_implemented")));
	TestFalse(TEXT("NotImplemented remains explicit"), Cases[3].Result.ErrorData->AsObject()->GetBoolField(TEXT("implemented")));
	TestEqual(TEXT("NotImplemented names missing part"), Cases[3].Result.ErrorData->AsObject()->GetStringField(TEXT("part")), FString(TEXT("scoring")));
	TestEqual(TEXT("EngineError preserves message"), Cases[4].Result.ErrorMessage, FString(TEXT("Compilation failed")));
	TestEqual(TEXT("OptionalDep names dependency"), Cases[5].Result.ErrorData->AsObject()->GetStringField(TEXT("dep_name")), FString(TEXT("CommonUI")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithErrorHelperSuggestionsTest,
	"Monolith.Core.ErrorHelpers.Suggestions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithErrorHelperSuggestionsTest::RunTest(const FString& Parameters)
{
	const FMonolithActionResult Result = FMonolithActionResult::NotFound(TEXT("graph"), TEXT("Health"),
		{ TEXT("Mana"), TEXT("HealthMaximum"), TEXT("Health"), TEXT("HealthMinimum") });
	const TArray<TSharedPtr<FJsonValue>>& Suggestions = Result.ErrorData->AsObject()->GetArrayField(TEXT("suggestions"));
	if (!TestEqual(TEXT("Suggestions capped at three"), Suggestions.Num(), 3)) return false;
	TestEqual(TEXT("Closest candidate ranks first"), Suggestions[0]->AsObject()->GetStringField(TEXT("graph")), FString(TEXT("Health")));
	TestEqual(TEXT("Exact candidate has score one"), Suggestions[0]->AsObject()->GetNumberField(TEXT("score")), 1.0);
	double PreviousScore = 1.0;
	for (const TSharedPtr<FJsonValue>& Suggestion : Suggestions)
	{
		const double Score = Suggestion->AsObject()->GetNumberField(TEXT("score"));
		TestTrue(TEXT("Scores descend and stay normalized"), Score <= PreviousScore && Score >= 0.0);
		PreviousScore = Score;
	}
	const FMonolithActionResult Single = FMonolithActionResult::NotFound(TEXT("asset"), TEXT("M_Heatlh"), { TEXT("M_Health") });
	TestEqual(TEXT("One candidate yields one suggestion"), Single.ErrorData->AsObject()->GetArrayField(TEXT("suggestions")).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithErrorDataNormalizationTest,
	"Monolith.Core.ErrorHelpers.NormalizeData", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithErrorDataNormalizationTest::RunTest(const FString& Parameters)
{
	auto Original = MakeShared<FJsonObject>();
	Original->SetStringField(TEXT("reason"), TEXT("save_failed"));
	Original->SetBoolField(TEXT("executed"), true);
	Original->SetBoolField(TEXT("retryable"), true);
	Original->SetArrayField(TEXT("warnings"), { MakeShared<FJsonValueString>(TEXT("existing warning")) });
	const auto Normalized = FMonolithActionResult::NormalizeErrorData(FMonolithJsonUtils::ErrInternalError, MakeShared<FJsonValueObject>(Original));
	TestTrue(TEXT("Normalization clones the object"), Normalized.Get() != &Original.Get());
	TestFalse(TEXT("Original object gains no metadata"), Original->HasField(TEXT("class")));
	TestEqual(TEXT("Legacy engine failure classified"), Normalized->GetStringField(TEXT("class")), FString(TEXT("engine_error")));
	TestEqual(TEXT("Existing reason survives"), Normalized->GetStringField(TEXT("reason")), FString(TEXT("save_failed")));
	TestTrue(TEXT("Executed true survives"), Normalized->GetBoolField(TEXT("executed")));
	TestTrue(TEXT("Explicit retryability survives"), Normalized->GetBoolField(TEXT("retryable")));
	TestEqual(TEXT("Warnings survive"), Normalized->GetArrayField(TEXT("warnings"))[0]->AsString(), FString(TEXT("existing warning")));
	Normalized->SetBoolField(TEXT("executed"), false);
	TestTrue(TEXT("Changing normalized top-level field does not mutate input"), Original->GetBoolField(TEXT("executed")));

	Original->SetStringField(TEXT("class"), TEXT("unknown_outcome"));
	Original->SetStringField(TEXT("executed"), TEXT("unknown"));
	const auto Unknown = FMonolithActionResult::NormalizeErrorData(FMonolithJsonUtils::ErrInternalError, MakeShared<FJsonValueObject>(Original));
	TestEqual(TEXT("Explicit class is preserved"), Unknown->GetStringField(TEXT("class")), FString(TEXT("unknown_outcome")));
	TestEqual(TEXT("Unknown execution stays unknown"), Unknown->GetStringField(TEXT("executed")), FString(TEXT("unknown")));

	const TSharedPtr<FJsonValue> Values[] = {
		MakeShared<FJsonValueString>(TEXT("original data")), MakeShared<FJsonValueNumber>(17),
		MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueNull>(),
		MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueString>(TEXT("item")) })
	};
	for (const TSharedPtr<FJsonValue>& Value : Values)
	{
		const auto Wrapped = FMonolithActionResult::NormalizeErrorData(FMonolithJsonUtils::ErrInvalidParams, Value);
		TestTrue(TEXT("Non-object payload retained under value"), Wrapped->TryGetField(TEXT("value")) == Value);
		TestEqual(TEXT("Non-object failure classified"), Wrapped->GetStringField(TEXT("class")), FString(TEXT("invalid_param")));
		TestFalse(TEXT("Legacy data gains no execution claim"), Wrapped->HasField(TEXT("executed")));
		TestFalse(TEXT("Legacy data gains no retry claim"), Wrapped->HasField(TEXT("retryable")));
	}
	const auto Empty = FMonolithActionResult::NormalizeErrorData(FMonolithJsonUtils::ErrInternalError, nullptr);
	TestEqual(TEXT("Absent data adds only class"), Empty->Values.Num(), 1);
	TestEqual(TEXT("Method-not-found code retains not-found class"),
		FMonolithActionResult::NormalizeErrorData(FMonolithJsonUtils::ErrMethodNotFound, nullptr)->GetStringField(TEXT("class")), FString(TEXT("not_found")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithErrorDataMergeTest,
	"Monolith.Core.ErrorHelpers.WithErrorData", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithErrorDataMergeTest::RunTest(const FString& Parameters)
{
	auto Override = MakeShared<FJsonObject>();
	Override->SetBoolField(TEXT("executed"), true);
	Override->SetBoolField(TEXT("partial"), true);
	FMonolithActionResult NotFound = FMonolithActionResult::NotFound(TEXT("asset"), TEXT("M_Heatlh"), { TEXT("M_Health") });
	const auto OriginalNotFound = NotFound.ErrorData->AsObject();
	NotFound.WithErrorData(Override);
	const auto Merged = NotFound.ErrorData->AsObject();
	TestEqual(TEXT("Merge keeps helper class"), Merged->GetStringField(TEXT("class")), FString(TEXT("not_found")));
	TestEqual(TEXT("Merge keeps suggestions"), Merged->GetArrayField(TEXT("suggestions"))[0]->AsObject()->GetStringField(TEXT("asset")), FString(TEXT("M_Health")));
	TestTrue(TEXT("Caller execution overrides helper default"), Merged->GetBoolField(TEXT("executed")));
	TestTrue(TEXT("Caller partial data is included"), Merged->GetBoolField(TEXT("partial")));
	TestFalse(TEXT("Helper input object is unchanged"), OriginalNotFound->GetBoolField(TEXT("executed")));
	TestFalse(TEXT("Caller input gains no helper class"), Override->HasField(TEXT("class")));
	TestFalse(TEXT("Caller input gains no suggestions"), Override->HasField(TEXT("suggestions")));

	FMonolithActionResult Engine = FMonolithActionResult::EngineError(TEXT("Save failed"));
	const auto OriginalEngine = Engine.ErrorData->AsObject();
	Engine.WithErrorData(Override);
	TestEqual(TEXT("Engine class survives merge"), Engine.ErrorData->AsObject()->GetStringField(TEXT("class")), FString(TEXT("engine_error")));
	TestTrue(TEXT("Engine execution override survives"), Engine.ErrorData->AsObject()->GetBoolField(TEXT("executed")));
	TestFalse(TEXT("Original engine defaults unchanged"), OriginalEngine->GetBoolField(TEXT("executed")));
	Override->SetStringField(TEXT("class"), TEXT("unknown_outcome"));
	Override->SetStringField(TEXT("executed"), TEXT("unknown"));
	Engine.WithErrorData(Override);
	TestEqual(TEXT("Explicit caller class wins"), Engine.ErrorData->AsObject()->GetStringField(TEXT("class")), FString(TEXT("unknown_outcome")));
	TestEqual(TEXT("Explicit unknown execution wins"), Engine.ErrorData->AsObject()->GetStringField(TEXT("executed")), FString(TEXT("unknown")));
	TestFalse(TEXT("Untouched retryability retained"), Engine.ErrorData->AsObject()->GetBoolField(TEXT("retryable")));
	Engine.WithErrorData(nullptr);
	TestFalse(TEXT("Null still resets data"), Engine.ErrorData.IsValid());
	return true;
}

#endif
