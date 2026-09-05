#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintNotFoundTest,
	"Monolith.Blueprint.Errors.NotFoundSuggestions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintNotFoundTest::RunTest(const FString& /*Parameters*/)
{
	const FString Name = TEXT("BP_Suggestions_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Path = TEXT("/Game/Tests/Monolith/Blueprint/") + Name;
	UPackage* Package = CreatePackage(*Path);
	UBlueprint* BP = NewObject<UBlueprint>(Package, *Name, RF_Public | RF_Standalone);
	if (!TestNotNull(TEXT("Create GUID-owned Blueprint"), BP)) return false;
	FAssetRegistryModule::AssetCreated(BP);
	ON_SCOPE_EXIT
	{
		FAssetRegistryModule::AssetDeleted(BP);
		BP->ClearFlags(RF_Public | RF_Standalone);
		BP->MarkAsGarbage();
		Package->SetDirtyFlag(false);
	};
	UEdGraph* Graph = NewObject<UEdGraph>(BP, TEXT("MovementGraph"));
	BP->UbergraphPages.Add(Graph);

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
	CheckMissing(FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), TEXT("list_graphs"), Params));
	Params->SetStringField(TEXT("asset_path"), BP->GetPathName());
	Params->SetStringField(TEXT("graph_name"), TEXT("MovmentGraph"));
	CheckMissing(FMonolithToolRegistry::Get().ExecuteAction(TEXT("blueprint"), TEXT("get_graph_data"), Params));
	TestEqual(TEXT("Fixture graph remains intact"), BP->UbergraphPages.Num(), 1);
	return true;
}
#endif
