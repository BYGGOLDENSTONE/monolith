#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "MonolithUISettingsActions.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithUISettingsSourcePairTest,
    "Monolith.UI.Settings.SourcePairSafety", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithUISettingsSourcePairTest::RunTest(const FString& Parameters)
{
    // This fixture owns one GUID directory and only deletes its two known files.
    const FString Module = TEXT("MonolithSettingsFixture") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Directory = FPaths::Combine(FPaths::ProjectDir(), TEXT("Source"), Module);
    const FString Header = FPaths::Combine(Directory, TEXT("FixtureSettings.h"));
    const FString Cpp = FPaths::Combine(Directory, TEXT("FixtureSettings.cpp"));
    if (!IFileManager::Get().MakeDirectory(*Directory, true)) return false;
    auto Params = MakeShared<FJsonObject>();
    Params->SetStringField(TEXT("class_name"), TEXT("UFixtureSettings"));
    Params->SetStringField(TEXT("module_name"), Module);
    Params->SetArrayField(TEXT("features"), {MakeShared<FJsonValueString>(TEXT("keybinding_support"))});
    FFileHelper::SaveStringToFile(TEXT("Existing source sentinel"), *Cpp);
    const auto Collision = FMonolithUISettingsActions::HandleScaffoldGameUserSettings(Params);
    TestFalse(TEXT("Existing cpp blocks the pair before header creation"), Collision.bSuccess);
    TestFalse(TEXT("Collision leaves no partial header"), FPaths::FileExists(Header));
    FString Existing;
    FFileHelper::LoadFileToString(Existing, *Cpp);
    TestEqual(TEXT("Existing cpp preserved byte content"), Existing, FString(TEXT("Existing source sentinel")));
    IFileManager::Get().Delete(*Cpp);
    Params->SetStringField(TEXT("class_name"), TEXT("../EscapedSettings"));
    TestFalse(TEXT("Class path traversal rejected"), FMonolithUISettingsActions::HandleScaffoldGameUserSettings(Params).bSuccess);
    Params->SetStringField(TEXT("class_name"), TEXT("FixtureSettings"));
    const auto Generated = FMonolithUISettingsActions::HandleScaffoldGameUserSettings(Params);
    TestTrue(TEXT("Class prefix normalized and source pair generated"), Generated.bSuccess);
    TestTrue(TEXT("Header generated"), FPaths::FileExists(Header));
    TestTrue(TEXT("Cpp generated"), FPaths::FileExists(Cpp));
    if (Generated.Result)
    {
        TestEqual(TEXT("Unreal reflected class path omits U prefix"), Generated.Result->GetStringField(TEXT("default_engine_ini")),
            FString::Printf(TEXT("[/Script/Engine.Engine]\nGameUserSettingsClassName=/Script/%s.FixtureSettings"), *Module));
    }
    IFileManager::Get().Delete(*Header);
    IFileManager::Get().Delete(*Cpp);
    TestTrue(TEXT("Fixture directory removed"), IFileManager::Get().DeleteDirectory(*Directory));
    return true;
}
#endif
