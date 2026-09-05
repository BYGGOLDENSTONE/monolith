#include "Misc/AutomationTest.h"
#include "MonolithMaterialActions.h"
#include "MonolithJsonUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialSuggestionsTest,
	"Monolith.Material.ErrorSuggestions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialSuggestionsTest::RunTest(const FString& Parameters)
{
	const FString Folder = TEXT("/Game/Tests/Monolith/Material/Suggestions_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Path = Folder / TEXT("M_Health");
	const FString Filename = FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
	UPackage* Package = CreatePackage(*Path);
	UMaterial* Material = NewObject<UMaterial>(Package, TEXT("M_Health"), RF_Public | RF_Standalone);
	ON_SCOPE_EXIT
	{
		FAssetRegistryModule::AssetDeleted(Material);
		Package->SetDirtyFlag(false);
		Material->ClearFlags(RF_Public | RF_Standalone);
		Material->MarkAsGarbage();
		TestFalse(TEXT("Diagnostic fixture never creates a file"), IFileManager::Get().FileExists(*Filename));
	};
	FAssetRegistryModule::AssetCreated(Material);
	UMaterialExpressionConstant* Expression = NewObject<UMaterialExpressionConstant>(Material, TEXT("HealthConstant"));
	Material->GetExpressionCollection().AddExpression(Expression);
	Package->SetDirtyFlag(false);

	auto CheckMiss = [&](const FMonolithActionResult& Result, const FString& Candidate)
	{
		TestEqual(TEXT("Action typo is not_found"), Result.ErrorCode, FMonolithJsonUtils::ErrNotFound);
		if (!TestTrue(TEXT("Action carries structured error data"), Result.ErrorData.IsValid())) return;
		const TSharedPtr<FJsonObject> Data = Result.ErrorData->AsObject();
		TestEqual(TEXT("Action error class"), Data->GetStringField(TEXT("class")), FString(TEXT("not_found")));
		TestFalse(TEXT("Rejected typo did not mutate"), Data->GetBoolField(TEXT("executed")));
		const TArray<TSharedPtr<FJsonValue>>& Suggestions = Data->GetArrayField(TEXT("suggestions"));
		if (TestTrue(TEXT("Actual action returns nonempty ranked suggestions"), !Suggestions.IsEmpty()))
		{
			TestEqual(TEXT("Suggestion identifies real candidate"), Suggestions[0]->AsObject()->GetStringField(Data->GetStringField(TEXT("kind"))), Candidate);
		}
		TestFalse(TEXT("Lookup leaves package clean"), Package->IsDirty());
	};
	auto AssetParams = MakeShared<FJsonObject>();
	AssetParams->SetStringField(TEXT("asset_path"), Folder / TEXT("M_Helath"));
	AddExpectedError(TEXT("LoadAsset failed:"), EAutomationExpectedErrorFlags::Contains, 1);
	CheckMiss(FMonolithMaterialActions::GetMaterialProperties(AssetParams), Path);

	auto ExpressionParams = MakeShared<FJsonObject>();
	ExpressionParams->SetStringField(TEXT("asset_path"), Path);
	ExpressionParams->SetStringField(TEXT("expression_name"), TEXT("HelathConstant"));
	CheckMiss(FMonolithMaterialActions::GetExpressionDetails(ExpressionParams), Expression->GetName());
	return true;
}

#endif
