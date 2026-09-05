#include "Misc/AutomationTest.h"
#include "MonolithNiagaraActions.h"
#include "MonolithJsonUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "NiagaraScript.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithNiagaraSuggestionsTest,
	"Monolith.Niagara.ErrorSuggestions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithNiagaraSuggestionsTest::RunTest(const FString& Parameters)
{
	const FString Folder = TEXT("/Game/Tests/Monolith/Niagara/Suggestions_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Path = Folder / TEXT("HealthModule");
	const FString Filename = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
	UPackage* Package = CreatePackage(*Path);
	UNiagaraScript* Script = NewObject<UNiagaraScript>(Package, TEXT("HealthModule"), RF_Public | RF_Standalone);
	ON_SCOPE_EXIT
	{
		FAssetRegistryModule::AssetDeleted(Script);
		Package->SetDirtyFlag(false);
		Script->ClearFlags(RF_Public | RF_Standalone);
		Script->MarkAsGarbage();
		TestFalse(TEXT("Diagnostic fixture never creates a file"), IFileManager::Get().FileExists(*Filename));
	};
	FAssetRegistryModule::AssetCreated(Script);
	Package->SetDirtyFlag(false);
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("script_path"), Folder / TEXT("HelathModule"));
	const FMonolithActionResult Result = FMonolithNiagaraActions::HandleGetModuleGraph(Params);
	TestEqual(TEXT("Actual graph action typo is not_found"), Result.ErrorCode, FMonolithJsonUtils::ErrNotFound);
	if (!TestTrue(TEXT("Action carries structured error data"), Result.ErrorData.IsValid())) return false;
	const TSharedPtr<FJsonObject> Data = Result.ErrorData->AsObject();
	TestEqual(TEXT("Action error class"), Data->GetStringField(TEXT("class")), FString(TEXT("not_found")));
	TestFalse(TEXT("Rejected typo did not execute a mutation"), Data->GetBoolField(TEXT("executed")));
	const TArray<TSharedPtr<FJsonValue>>& Suggestions = Data->GetArrayField(TEXT("suggestions"));
	if (TestTrue(TEXT("Actual action returns nonempty ranked suggestions"), !Suggestions.IsEmpty()))
	{
		TestEqual(TEXT("Suggestion identifies real script"), Suggestions[0]->AsObject()->GetStringField(TEXT("script")), Path);
	}
	TestFalse(TEXT("Lookup leaves package clean"), Package->IsDirty());
	return true;
}

#endif
