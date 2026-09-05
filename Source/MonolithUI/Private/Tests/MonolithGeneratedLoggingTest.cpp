#include "Misc/AutomationTest.h"
#include "MonolithUIAccessibilityActions.h"
#include "MonolithUISettingsActions.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS
// Compile two same-named categories in one translation unit, as a unity build
// does for generated classes. Qualified UE_LOG calls must compile as well.
namespace MonolithGeneratedLoggingFixtureA
{
DEFINE_LOG_CATEGORY_STATIC(LogMonolith, Log, All);
}
namespace MonolithGeneratedLoggingFixtureB
{
DEFINE_LOG_CATEGORY_STATIC(LogMonolith, Log, All);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithGeneratedLoggingTest, "Monolith.UI.GeneratedLogging",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithGeneratedLoggingTest::RunTest(const FString& Parameters)
{
    UE_LOG(MonolithGeneratedLoggingFixtureA::LogMonolith, VeryVerbose, TEXT("First generated category"));
    UE_LOG(MonolithGeneratedLoggingFixtureB::LogMonolith, VeryVerbose, TEXT("Second generated category"));
    const FString ModuleName = TEXT("MonolithLoggingAutomation") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Directory = FPaths::Combine(FPaths::ProjectDir(), TEXT("Source"), ModuleName);
    if (!TestTrue(TEXT("Create isolated scaffold source directory"), IFileManager::Get().MakeDirectory(*Directory, true))) return false;
    ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Directory, false, true); };
    using FHandler = FMonolithActionResult (*)(const TSharedPtr<FJsonObject>&);
    struct FCase { const TCHAR* ClassName; FHandler Handler; };
    const FCase Cases[] = {
        {TEXT("ULoggingAccessibility"), &FMonolithUIAccessibilityActions::HandleScaffoldAccessibilitySubsystem},
        {TEXT("ULoggingSaveA"), &FMonolithUISettingsActions::HandleScaffoldSaveGame},
        {TEXT("ULoggingSaveB"), &FMonolithUISettingsActions::HandleScaffoldSaveGame},
        {TEXT("ULoggingSaveSubsystem"), &FMonolithUISettingsActions::HandleScaffoldSaveSubsystem},
    };
    TSet<FString> Namespaces;
    for (const auto& Case : Cases)
    {
        auto Params = MakeShared<FJsonObject>();
        Params->SetStringField(TEXT("class_name"), Case.ClassName);
        Params->SetStringField(TEXT("module_name"), ModuleName);
        Params->SetStringField(TEXT("save_game_class"), TEXT("ULoggingSaveA"));
        const auto Result = Case.Handler(Params);
        if (!TestTrue(TEXT("Scaffold generates source"), Result.bSuccess)) continue;
        const FString CleanName = FString(Case.ClassName).RightChop(1);
        FString Cpp;
        if (!TestTrue(TEXT("Generated CPP readable"), FFileHelper::LoadFileToString(Cpp, *FPaths::Combine(Directory, CleanName + TEXT(".cpp"))))) continue;
        const FString Namespace = TEXT("MonolithGenerated_") + CleanName;
        TestFalse(TEXT("Each generated class has a distinct logging namespace"), Namespaces.Contains(Namespace));
        Namespaces.Add(Namespace);
        TestTrue(TEXT("Category declared inside class-specific namespace"), Cpp.Contains(TEXT("namespace ") + Namespace + TEXT("\n{\nDEFINE_LOG_CATEGORY_STATIC(LogMonolith, Log, All);\n}")));
        const FString Definition = TEXT("DEFINE_LOG_CATEGORY_STATIC(LogMonolith, Log, All);");
        const int32 FirstDefinition = Cpp.Find(Definition);
        TestTrue(TEXT("Generated category is defined"), FirstDefinition != INDEX_NONE);
        TestEqual(TEXT("Exactly one category definition per generated CPP"), Cpp.Find(Definition, ESearchCase::CaseSensitive, ESearchDir::FromStart, FirstDefinition + Definition.Len()), INDEX_NONE);
        TestTrue(TEXT("Generated log calls use qualified category"), Cpp.Contains(TEXT("UE_LOG(") + Namespace + TEXT("::LogMonolith,")));
        TestFalse(TEXT("No global category lookup remains"), Cpp.Contains(TEXT("UE_LOG(LogMonolith,")));
        TestFalse(TEXT("Generated runtime CPP does not depend on MonolithCore header"), Cpp.Contains(TEXT("MonolithJsonUtils.h")));
    }
    TestEqual(TEXT("All four generated classes checked"), Namespaces.Num(), 4);
    return true;
}
#endif
