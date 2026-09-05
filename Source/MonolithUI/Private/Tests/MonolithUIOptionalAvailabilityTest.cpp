#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUIOptionalAvailabilityTest, "Monolith.UI.OptionalAvailability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIOptionalAvailabilityTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const auto Empty = MakeShared<FJsonObject>();
	const auto Base = Registry.ExecuteAction(TEXT("ui"), TEXT("list_widget_types"), Empty);
	if (!TestTrue(TEXT("Base UMG discovery remains usable"), Base.bSuccess && Base.Result.IsValid())) return false;
	TestTrue(TEXT("Base UMG reports widget types"), Base.Result->GetNumberField(TEXT("count")) > 0);
	int32 CommonActions = 0;
	for (const auto& Action : Registry.GetActions(TEXT("ui")))
	{
		if (Action.Category == TEXT("CommonUI")) ++CommonActions;
	}
	TestEqual(TEXT("All CommonUI category schemas remain registered"), CommonActions, 61);
	const auto Stats = Registry.ExecuteAction(TEXT("ui"), TEXT("dump_style_cache_stats"), Empty);
#if WITH_COMMONUI
	TestTrue(TEXT("Compiled CommonUI style diagnostics run"), Stats.bSuccess);
	const auto ReportParams = MakeShared<FJsonObject>();
	ReportParams->SetStringField(TEXT("folder_path"), TEXT("/Game/Tests/Monolith/UI/OptionalAvailability"));
	const auto Report = Registry.ExecuteAction(TEXT("ui"), TEXT("export_commonui_report"), ReportParams);
	TestTrue(TEXT("Compiled CommonUI category handler runs"), Report.bSuccess && Report.Result.IsValid());
#else
	auto CheckUnavailable = [this](const FMonolithActionResult& Result)
	{
		TestFalse(TEXT("Absent CommonUI rejects execution"), Result.bSuccess);
		TestEqual(TEXT("CommonUI dependency code"), Result.ErrorCode, FMonolithJsonUtils::ErrOptionalDepUnavailable);
		if (!TestTrue(TEXT("CommonUI error has data"), Result.ErrorData.IsValid())) return;
		const auto Data = Result.ErrorData->AsObject();
		if (!TestTrue(TEXT("CommonUI error data is an object"), Data.IsValid())) return;
		TestEqual(TEXT("CommonUI error class"), Data->GetStringField(TEXT("class")), FString(TEXT("optional_dep_unavailable")));
		TestEqual(TEXT("CommonUI dependency named"), Data->GetStringField(TEXT("dep_name")), FString(TEXT("CommonUI")));
		TestFalse(TEXT("CommonUI absence causes no mutation"), Data->GetBoolField(TEXT("executed")));
	};
	CheckUnavailable(Stats);
	const auto CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("save_path"), TEXT("/Game/Tests/Monolith/UI/OptionalAvailability/WBP_Unavailable"));
	CheckUnavailable(Registry.ExecuteAction(TEXT("ui"), TEXT("create_activatable_widget"), CreateParams));
#endif
	const auto Discover = Registry.ExecuteAction(TEXT("monolith"), TEXT("discover"), Empty);
	if (!TestTrue(TEXT("Top-level discovery succeeds"), Discover.bSuccess && Discover.Result.IsValid())) return false;
	for (const auto& Value : Discover.Result->GetArrayField(TEXT("namespaces")))
	{
		const auto Ns = Value->AsObject();
		if (Ns->GetStringField(TEXT("namespace")) != TEXT("ui")) continue;
		TestTrue(TEXT("Mixed UI namespace remains available"), Ns->GetObjectField(TEXT("availability"))->GetBoolField(TEXT("available")));
		for (const auto& DepValue : Ns->GetArrayField(TEXT("optional_dependencies")))
		{
			const auto Dep = DepValue->AsObject();
			if (Dep->GetStringField(TEXT("required_plugin")) != TEXT("CommonUI")) continue;
			TestEqual(TEXT("CommonUI discovery matches compiled implementation"), Dep->GetBoolField(TEXT("available")), WITH_COMMONUI != 0);
			TestEqual(TEXT("CommonUI availability reason"), Dep->GetStringField(TEXT("reason")), FString(WITH_COMMONUI ? TEXT("") : TEXT("not_compiled")));
			return true;
		}
		AddError(TEXT("UI discovery omitted the CommonUI optional dependency"));
		return false;
	}
	AddError(TEXT("UI namespace missing from top-level discovery"));
	return false;
}
#endif
