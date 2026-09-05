#if WITH_DEV_AUTOMATION_TESTS

#include "Tests/UIHonestyTestUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUIMenuUnsupportedKeysTest,
    "Monolith.UI.Honesty.MenuUnsupportedKeys", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMenuUnsupportedKeysTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::HonestyTests;
    for (const TCHAR* Key : { TEXT("layers"), TEXT("focus_table"), TEXT("nav_overrides") })
    {
        FScopedWidget Widget(*this);
        const TSharedPtr<FJsonObject> Params = Menu(Widget, true, true);
        Params->SetArrayField(Key, { MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()) });
        const auto Data = CheckNotImplemented(*this,
            FMonolithToolRegistry::Get().ExecuteAction(TEXT("ui"), TEXT("build_menu_from_spec"), Params));
        if (!Data.IsValid()) continue;
        TestTrue(TEXT("Error identifies the unsupported key"), HasString(Data, TEXT("unimplemented_parts"), Key));
        TestFalse(TEXT("Dry run has no applied mutations"), Data->GetBoolField(TEXT("partial")));
        TestEqual(TEXT("Dry run applied_keys is empty"), Data->GetArrayField(TEXT("applied_keys")).Num(), 0);
        TestFalse(TEXT("Dry run did not save the screen"), IFileManager::Get().FileExists(*Widget.Filename()));
    }
    // Empty aggregation arrays request nothing and must not trip the capability error.
    {
        FScopedWidget Widget(*this);
        const TSharedPtr<FJsonObject> Params = Menu(Widget, true, true);
        for (const TCHAR* Key : { TEXT("layers"), TEXT("focus_table"), TEXT("nav_overrides") })
        {
            Params->SetArrayField(Key, TArray<TSharedPtr<FJsonValue>>());
        }
        const auto Result = FMonolithToolRegistry::Get().ExecuteAction(TEXT("ui"), TEXT("build_menu_from_spec"), Params);
        TestTrue(TEXT("Empty aggregation arrays are a supported dry run"), Result.bSuccess);
        if (Result.Result.IsValid())
        {
            TestEqual(TEXT("Empty arrays report ok status"), Result.Result->GetStringField(TEXT("status")), FString(TEXT("ok")));
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUIMenuMissingSpecTest,
    "Monolith.UI.Honesty.MenuMissingSpec", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMenuMissingSpecTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::HonestyTests;
    FScopedWidget Widget(*this);
    const auto Data = CheckNotImplemented(*this, FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("build_menu_from_spec"), Menu(Widget, false, false)));
    if (!Data.IsValid()) return false;
    TestTrue(TEXT("Kind scaffolding is named"), HasString(Data, TEXT("unimplemented_parts"), TEXT("screens[0].kind_scaffolding")));
    TestFalse(TEXT("No screen was applied"), Data->GetBoolField(TEXT("partial")));
    TestFalse(TEXT("Menu cannot claim semantic success"), Data->GetBoolField(TEXT("bSuccess")));
    TestEqual(TEXT("No keys applied"), Data->GetArrayField(TEXT("applied_keys")).Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUIMenuPartialBuildTest,
    "Monolith.UI.Honesty.MenuPartialBuild", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMenuPartialBuildTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::HonestyTests;
    FScopedWidget Built(*this);
    FScopedWidget Unsupported(*this);
    auto Params = Menu(Built, true, false);
    Params->SetArrayField(TEXT("screens"), {
        MakeShared<FJsonValueObject>(Screen(Built.Path, true)),
        MakeShared<FJsonValueObject>(Screen(Unsupported.Path, false)) });
    Params->SetArrayField(TEXT("layers"), { MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()) });
    const auto Data = CheckNotImplemented(*this, FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("build_menu_from_spec"), Params));
    if (!Data.IsValid()) return false;
    TestTrue(TEXT("Successful screen is explicitly partial"), Data->GetBoolField(TEXT("partial")));
    TestTrue(TEXT("Applied screen spec is named"), HasString(Data, TEXT("applied_keys"), TEXT("screens[0].spec")));
    TestFalse(TEXT("Unsupported screen is never applied"), HasString(Data, TEXT("applied_keys"), TEXT("screens[1].spec")));
    TestTrue(TEXT("Unsupported layers are named"), HasString(Data, TEXT("unimplemented_parts"), TEXT("layers")));
    TestTrue(TEXT("Unsupported screen is named"), HasString(Data, TEXT("unimplemented_parts"), TEXT("screens[1].kind_scaffolding")));
    const auto& Screens = Data->GetArrayField(TEXT("screens"));
    TestEqual(TEXT("Both screen outcomes retained"), Screens.Num(), 2);
    if (Screens.Num() == 2)
    {
        TestTrue(TEXT("Completed screen result survives the error"),
            Screens[0]->AsObject()->GetObjectField(TEXT("build_result"))->GetBoolField(TEXT("bSuccess")));
    }
    UWidgetBlueprint* WBP = Built.Load();
    TestNotNull(TEXT("Partial result corresponds to a real WBP"), WBP);
    if (WBP && WBP->WidgetTree)
        TestTrue(TEXT("Actual widget root was built"), WBP->WidgetTree->RootWidget && WBP->WidgetTree->RootWidget->IsA<UVerticalBox>());
    TestTrue(TEXT("Completed screen was saved"), IFileManager::Get().FileExists(*Built.Filename()));
    TestFalse(TEXT("Unsupported screen was not saved"), IFileManager::Get().FileExists(*Unsupported.Filename()));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUIMenuSupportedBuildTest,
    "Monolith.UI.Honesty.MenuSupportedBuild", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIMenuSupportedBuildTest::RunTest(const FString& Parameters)
{
    using namespace MonolithUI::HonestyTests;
    FScopedWidget Widget(*this);
    const auto Result = FMonolithToolRegistry::Get().ExecuteAction(
        TEXT("ui"), TEXT("build_menu_from_spec"), Menu(Widget, true, false));
    TestTrue(TEXT("Supported-only request still succeeds"), Result.bSuccess);
    if (!Result.Result.IsValid()) return false;
    TestTrue(TEXT("Screen builder reports success"), Result.Result->GetBoolField(TEXT("bSuccess")));
    TestNotNull(TEXT("Supported screen exists"), Widget.Load());
    TestTrue(TEXT("Supported screen was saved"), IFileManager::Get().FileExists(*Widget.Filename()));
    return true;
}

#endif
