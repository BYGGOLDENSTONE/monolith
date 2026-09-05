#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimMontage.h"
#include "EdGraph/EdGraph.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAnimationNotFoundTest,
	"Monolith.Animation.Errors.NotFoundSuggestions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAnimationNotFoundTest::RunTest(const FString& /*Parameters*/)
{
	const FString Name = TEXT("ABP_Suggestions_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Path = TEXT("/Game/Tests/Monolith/Animation/") + Name;
	UPackage* Package = CreatePackage(*Path);
	UAnimBlueprint* ABP = NewObject<UAnimBlueprint>(Package, *Name, RF_Public | RF_Standalone);
	if (!TestNotNull(TEXT("Create GUID-owned AnimBlueprint"), ABP)) return false;
	FAssetRegistryModule::AssetCreated(ABP);
	const FString MontageName = TEXT("AM_Suggestions_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* MontagePackage = CreatePackage(*(TEXT("/Game/Tests/Monolith/Animation/") + MontageName));
	UAnimMontage* Montage = NewObject<UAnimMontage>(MontagePackage, *MontageName, RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(Montage);
	ON_SCOPE_EXIT
	{
		for (UObject* Asset : TArray<UObject*>{ABP, Montage})
		{
			FAssetRegistryModule::AssetDeleted(Asset);
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->GetOutermost()->SetDirtyFlag(false);
			Asset->MarkAsGarbage();
		}
	};
	UEdGraph* Graph = NewObject<UEdGraph>(ABP, TEXT("MovementGraph"));
	ABP->FunctionGraphs.Add(Graph);
	FCompositeSection Section;
	Section.SectionName = TEXT("AttackStart");
	Montage->CompositeSections.Add(Section);

	auto CheckMissing = [&](const FMonolithActionResult& Result)
	{
		TestFalse(TEXT("Typo fails"), Result.bSuccess);
		TestEqual(TEXT("Not-found error code"), Result.ErrorCode, FMonolithJsonUtils::ErrNotFound);
		if (!TestTrue(TEXT("Structured error data"), Result.ErrorData.IsValid())) return;
		const TSharedPtr<FJsonObject> Data = Result.ErrorData->AsObject();
		TestEqual(TEXT("Not-found class"), Data->GetStringField(TEXT("class")), FString(TEXT("not_found")));
		TestFalse(TEXT("Typo did not execute a mutation"), Data->GetBoolField(TEXT("executed")));
		const TArray<TSharedPtr<FJsonValue>>* Suggestions = nullptr;
		TestTrue(TEXT("Nonempty suggestions"), Data->TryGetArrayField(TEXT("suggestions"), Suggestions) && Suggestions && !Suggestions->IsEmpty());
	};
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Path + TEXT("x"));
	CheckMissing(FMonolithToolRegistry::Get().ExecuteAction(TEXT("animation"), TEXT("get_graphs"), Params));
	Params->SetStringField(TEXT("asset_path"), ABP->GetPathName());
	Params->SetStringField(TEXT("graph_name"), TEXT("MovmentGraph"));
	CheckMissing(FMonolithToolRegistry::Get().ExecuteAction(TEXT("animation"), TEXT("get_blend_nodes"), Params));
	Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Montage->GetPathName());
	Params->SetStringField(TEXT("section_name"), TEXT("AtackStart"));
	Params->SetNumberField(TEXT("new_time"), 0.25);
	CheckMissing(FMonolithToolRegistry::Get().ExecuteAction(TEXT("animation"), TEXT("set_section_time"), Params));
	TestEqual(TEXT("Section unchanged after typo"), Montage->CompositeSections[0].SectionName, FName(TEXT("AttackStart")));
	return true;
}
#endif
