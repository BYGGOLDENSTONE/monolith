#include "Tests/UIHonestyTestUtils.h"
#include "Blueprint/WidgetNavigation.h"
#include "Components/Button.h"
#include "Components/Overlay.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionResult.h"
#include "Spec/UISpecBuilder.h"
#if WITH_COMMONUI
#include "CommonActivatableWidget.h"
#include "Widgets/CommonActivatableWidgetContainer.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUIMenuAggregationTest,
    "Monolith.UI.Honesty.MenuAggregation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithUIMenuAggregationTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::HonestyTests;
    for (const TCHAR* Key : {TEXT("layers"), TEXT("focus_table"), TEXT("nav_overrides")})
    {
        FScopedWidget Widget(*this);
        auto Params = Menu(Widget, true, false);
        Params->SetArrayField(Key, {MakeShared<FJsonValueObject>(MakeShared<FJsonObject>())});
        const auto Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("ui"), TEXT("build_menu_from_spec"), Params);
        TestFalse(TEXT("Malformed aggregation is rejected"), Result.bSuccess);
        TestEqual(TEXT("Validation error"), Result.ErrorCode, -32602);
        TestFalse(TEXT("Rejected aggregation creates no file"), IFileManager::Get().FileExists(*Widget.Filename()));
        TestNull(TEXT("Rejected aggregation creates no in-memory WBP"), FindObject<UWidgetBlueprint>(nullptr, *Widget.ObjectPath()));
    }
    FScopedWidget Widget(*this);
    auto Params = Menu(Widget, true, true);
    for (const TCHAR* Key : {TEXT("layers"), TEXT("focus_table"), TEXT("nav_overrides")}) Params->SetArrayField(Key, {});
    TestTrue(TEXT("Empty arrays remain supported"), FMonolithToolRegistry::Get().ExecuteAction(TEXT("ui"), TEXT("build_menu_from_spec"), Params).bSuccess);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUIMenuMissingSpecTest,
    "Monolith.UI.Honesty.MenuMissingSpec", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithUIMenuMissingSpecTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::HonestyTests;
    FScopedWidget Widget(*this);
    auto Params = Menu(Widget, false, false);
#if WITH_COMMONUI
    const auto Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("ui"), TEXT("build_menu_from_spec"), Params);
    TestTrue(TEXT("main_menu kind scaffolds without embedded spec"), Result.bSuccess);
    UWidgetBlueprint* WBP = Widget.Load();
    TestNotNull(TEXT("Scaffold saved"), WBP);
    if (WBP && WBP->WidgetTree)
    {
        TestNotNull(TEXT("Start button authored"), Cast<UButton>(WBP->WidgetTree->FindWidget(TEXT("StartButton"))));
        TestNotNull(TEXT("Settings button authored"), WBP->WidgetTree->FindWidget(TEXT("SettingsButton")));
        TestNotNull(TEXT("Quit button authored"), WBP->WidgetTree->FindWidget(TEXT("QuitButton")));
    }
#else
    TestFalse(TEXT("CommonUI kind rejected when unavailable"), FMonolithToolRegistry::Get().ExecuteAction(TEXT("ui"), TEXT("build_menu_from_spec"), Params).bSuccess);
#endif
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUIMenuPartialBuildTest,
    "Monolith.UI.Honesty.MenuPartialBuild", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithUIMenuPartialBuildTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::HonestyTests;
    FScopedWidget First(*this), Second(*this);
    auto Params = Menu(First, true, false);
    auto Invalid = Screen(Second.Path, false);
    Invalid->SetStringField(TEXT("id"), TEXT("second"));
    Invalid->SetStringField(TEXT("kind"), TEXT("unknown_kind"));
    Params->SetArrayField(TEXT("screens"), {MakeShared<FJsonValueObject>(Screen(First.Path, true)), MakeShared<FJsonValueObject>(Invalid)});
    auto Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("ui"), TEXT("build_menu_from_spec"), Params);
    TestFalse(TEXT("Later invalid screen rejects whole request before writes"), Result.bSuccess);
    TestFalse(TEXT("Earlier valid screen not saved"), IFileManager::Get().FileExists(*First.Filename()));
    TestFalse(TEXT("Invalid screen not saved"), IFileManager::Get().FileExists(*Second.Filename()));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUIMenuSupportedBuildTest,
    "Monolith.UI.Honesty.MenuSupportedBuild", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithUIMenuSupportedBuildTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::HonestyTests;
    FScopedWidget Widget(*this);
    const auto Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("ui"), TEXT("build_menu_from_spec"), Menu(Widget, true, false));
    TestTrue(TEXT("Embedded spec succeeds"), Result.bSuccess);
    TestNotNull(TEXT("Screen exists"), Widget.Load());
    TestTrue(TEXT("Screen is saved"), IFileManager::Get().FileExists(*Widget.Filename()));
    return true;
}

#if WITH_COMMONUI
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUIMenuFailedAuthoringCleanupTest,
    "Monolith.UI.Menu.FailedAuthoringCleanup", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithUIMenuFailedAuthoringCleanupTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::HonestyTests;
    FScopedWidget Widget(*this);
    FUISpecDocument Spec;
    Spec.Name = TEXT("FailedAuthoring");
    Spec.ParentClass = TEXT("UserWidget");
    Spec.Root = MakeShared<FUISpecNode>();
    Spec.Root->Type = TEXT("VerticalBox");
    Spec.Root->Id = TEXT("RootBox");
    FUISpecBuilderInputs Inputs;
    Inputs.Document = &Spec;
    Inputs.AssetPath = Widget.Path;
    Inputs.BeforeCompile = [](UWidgetBlueprint*, FString& Error)
    {
        Error = TEXT("Intentional authoring failure fixture");
        return false;
    };
    const auto Result = FUISpecBuilder::Build(Inputs);
    TestFalse(TEXT("Authoring failure cannot report success"), Result.bSuccess);
    TestFalse(TEXT("Failed authoring never saves a package"), IFileManager::Get().FileExists(*Widget.Filename()));
    TestFalse(TEXT("Rolled-back WBP is no longer valid"), IsValid(FindObject<UWidgetBlueprint>(nullptr, *Widget.ObjectPath())));
    // The scoped fixture must tolerate already-completed builder rollback and
    // must not submit garbage objects to ForceDeleteObjects on destruction.
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUIMenuFocusNavigationLayersTest,
    "Monolith.UI.Menu.FocusNavigationLayers", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithUIMenuFocusNavigationLayersTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::HonestyTests;
    FScopedWidget ScreenWidget(*this), Host(*this);
    auto Params = Menu(ScreenWidget, false, true);
    Params->SetStringField(TEXT("menu_asset_path"), Host.Path);
    Params->SetArrayField(TEXT("layers"), {MakeShared<FJsonValueObject>(FMonolithJsonUtils::Parse(TEXT("{\"id\":\"MainLayer\",\"screens\":[\"screen\"]}")))});
    Params->SetArrayField(TEXT("focus_table"), {MakeShared<FJsonValueObject>(FMonolithJsonUtils::Parse(TEXT("{\"screen\":\"screen\",\"target\":\"StartButton\"}")))});
    Params->SetArrayField(TEXT("nav_overrides"), {MakeShared<FJsonValueObject>(FMonolithJsonUtils::Parse(TEXT("{\"screen\":\"screen\",\"widget\":\"StartButton\",\"direction\":\"Down\",\"target\":\"SettingsButton\"}")))});
    auto DryRun = FMonolithToolRegistry::Get().ExecuteAction(TEXT("ui"), TEXT("build_menu_from_spec"), Params);
    TestTrue(TEXT("Aggregation dry-run succeeds"), DryRun.bSuccess);
    TestFalse(TEXT("Dry-run creates no host"), IFileManager::Get().FileExists(*Host.Filename()));
    TestFalse(TEXT("Dry-run creates no screen"), IFileManager::Get().FileExists(*ScreenWidget.Filename()));
    Params->SetBoolField(TEXT("dry_run"), false);
    auto Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("ui"), TEXT("build_menu_from_spec"), Params);
    TestTrue(TEXT("Aggregation committed"), Result.bSuccess);
    if (!Result.bSuccess) { AddError(Result.ErrorMessage + TEXT(" ") + (Result.ErrorData ? FMonolithJsonUtils::Serialize(Result.ErrorData->AsObject()) : FString())); return false; }
    UWidgetBlueprint* ScreenWBP = ScreenWidget.Load();
    UWidgetBlueprint* HostWBP = Host.Load();
    TestNotNull(TEXT("Screen saved"), ScreenWBP);
    TestNotNull(TEXT("Host saved"), HostWBP);
    if (!ScreenWBP || !HostWBP) return false;
    TestTrue(TEXT("Screen compiled without errors"), ScreenWBP->Status != BS_Error);
    TestTrue(TEXT("Host compiled without errors"), HostWBP->Status != BS_Error);
    UWidget* Start = ScreenWBP->WidgetTree->FindWidget(TEXT("StartButton"));
    if (TestNotNull(TEXT("Start navigation exists"), Start ? Start->Navigation.Get() : nullptr))
    {
        TestEqual(TEXT("Explicit navigation authored"), Start->Navigation->Down.Rule, EUINavigationRule::Explicit);
        TestEqual(TEXT("Serialized navigation target"), Start->Navigation->Down.WidgetToFocus, FName(TEXT("SettingsButton")));
    }
    UFunction* Focus = ScreenWBP->GeneratedClass->FindFunctionByName(TEXT("BP_GetDesiredFocusTarget"));
    TestTrue(TEXT("Focus override belongs to generated class"), Focus && Focus->GetOuter() == ScreenWBP->GeneratedClass);
    TestNotNull(TEXT("Host stack authored"), Cast<UCommonActivatableWidgetStack>(HostWBP->WidgetTree->FindWidget(TEXT("MainLayer"))));
    bool bPush = false;
    for (UEdGraph* Graph : HostWBP->UbergraphPages)
        for (UEdGraphNode* Node : Graph->Nodes)
            if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
                if (Call->FunctionReference.GetMemberName() == TEXT("BP_AddWidget"))
                    bPush = Call->FindPin(TEXT("ActivatableWidgetClass"))->DefaultObject == ScreenWBP->GeneratedClass && !Call->GetExecPin()->LinkedTo.IsEmpty();
    TestTrue(TEXT("Stack initialization pushes compiled screen class"), bPush);
    return true;
}
#endif
#endif
