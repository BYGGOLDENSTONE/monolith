#include "Misc/AutomationTest.h"
#include "MonolithAssetUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAssetSuggestionsTest,
	"Monolith.Core.AssetSuggestions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAssetSuggestionsTest::RunTest(const FString& Parameters)
{
	const FString Root = TEXT("/Game/Tests/Monolith/Core/Suggestions_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TArray<UObject*> Assets;
	TArray<FString> Filenames;
	ON_SCOPE_EXIT
	{
		for (UObject* Asset : Assets)
		{
			FAssetRegistryModule::AssetDeleted(Asset);
			Asset->GetPackage()->SetDirtyFlag(false);
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->MarkAsGarbage();
		}
		for (const FString& Filename : Filenames)
		{
			TestFalse(TEXT("Fixture cleanup never writes an asset"), IFileManager::Get().FileExists(*Filename));
		}
	};
	auto AddAsset = [&](const FString& Suffix, UClass* Class)
	{
		const FString Path = Root / Suffix;
		UPackage* Package = CreatePackage(*Path);
		UObject* Asset = NewObject<UObject>(Package, Class, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone);
		Assets.Add(Asset);
		FAssetRegistryModule::AssetCreated(Asset);
		Package->SetDirtyFlag(false);
		Filenames.Add(FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension()));
		return Path;
	};
	const FString Health = AddAsset(TEXT("Curves/Health"), UCurveFloat::StaticClass());
	const FString Maximum = AddAsset(TEXT("Curves/HealthMaximum"), UCurveVector::StaticClass());
	const FString Texture = AddAsset(TEXT("Curves/HealthTexture"), UTexture2D::StaticClass());
	const FString Elsewhere = AddAsset(TEXT("Elsewhere/Health"), UCurveFloat::StaticClass());
	const FString Nested = AddAsset(TEXT("Curves/Deeper/HealthNested"), UCurveFloat::StaticClass());
	Assets[1]->GetPackage()->SetDirtyFlag(true); // Preserve both clean and already-dirty packages.
	TArray<bool> DirtyStates;
	for (UObject* Asset : Assets) DirtyStates.Add(Asset->GetPackage()->IsDirty());
	for (const FString& Filename : Filenames) TestFalse(TEXT("Unsaved fixture has no disk file"), IFileManager::Get().FileExists(*Filename));

	const FString Typo = Root / TEXT("Curves/Helath");
	const TArray<FString> Candidates = FMonolithAssetUtils::GetAssetPathCandidates(Typo, UCurveBase::StaticClass());
	TestEqual(TEXT("Only matching sibling classes are candidates"), Candidates.Num(), 2);
	TestTrue(TEXT("Float curve is included"), Candidates.Contains(Health));
	TestTrue(TEXT("Vector curve subclass is included"), Candidates.Contains(Maximum));
	TestFalse(TEXT("Unrelated class is excluded"), Candidates.Contains(Texture));
	TestFalse(TEXT("Other folders are excluded when siblings exist"), Candidates.Contains(Elsewhere));
	TestFalse(TEXT("Nested folders are never scanned recursively"), Candidates.Contains(Nested));
	TestTrue(TEXT("Folder typo uses bounded immediate-sibling fallback"),
		FMonolithAssetUtils::GetAssetPathCandidates(Root / TEXT("Curvse/Helath"), UCurveBase::StaticClass()).Contains(Health));
	TestTrue(TEXT("Empty branch returns no candidates"),
		FMonolithAssetUtils::GetAssetPathCandidates(Root / TEXT("Empty/Deep/Missing"), UCurveBase::StaticClass()).IsEmpty());
	TestTrue(TEXT("Malformed path returns no candidates"), FMonolithAssetUtils::GetAssetPathCandidates(TEXT("/Game/Bad Path/Missing")).IsEmpty());
	const FString ObjectTypo = Typo + TEXT(".Helath");
	const FMonolithActionResult Result = FMonolithAssetUtils::AssetNotFound(TEXT("curve"), ObjectTypo, UCurveBase::StaticClass());
	TestEqual(TEXT("Missing asset uses not_found"), Result.ErrorCode, FMonolithJsonUtils::ErrNotFound);
	const TSharedPtr<FJsonObject> Data = Result.ErrorData->AsObject();
	TestEqual(TEXT("Object paths canonicalize to package needle"), Data->GetStringField(TEXT("needle")), Typo);
	TestEqual(TEXT("Raw request is preserved"), Data->GetStringField(TEXT("requested_path")), ObjectTypo);
	TestFalse(TEXT("Suggestion failure did not execute a mutation"), Data->GetBoolField(TEXT("executed")));
	TestTrue(TEXT("Typo has ranked suggestions"), Data->GetArrayField(TEXT("suggestions")).Num() > 0);
	TestEqual(TEXT("Empty paths are invalid parameters"), FMonolithAssetUtils::AssetNotFound(TEXT("asset"), TEXT(" ")).ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
	for (int32 Index = 0; Index < Assets.Num(); ++Index)
	{
		TestEqual(TEXT("Suggestion lookup preserves package dirty state"), Assets[Index]->GetPackage()->IsDirty(), DirtyStates[Index]);
		TestFalse(TEXT("Suggestion lookup does not save assets"), IFileManager::Get().FileExists(*Filenames[Index]));
	}
	for (int32 Index = 0; Index < 257; ++Index)
	{
		AddAsset(FString::Printf(TEXT("Bounded/Curve_%03d"), Index), UCurveFloat::StaticClass());
	}
	TestEqual(TEXT("Candidate collection stops at its documented bound"),
		FMonolithAssetUtils::GetAssetPathCandidates(Root / TEXT("Bounded/Curv"), UCurveFloat::StaticClass()).Num(), 256);
	return true;
}

#endif
