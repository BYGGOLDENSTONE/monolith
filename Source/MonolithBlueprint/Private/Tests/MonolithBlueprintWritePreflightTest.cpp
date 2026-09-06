#include "Misc/AutomationTest.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "MonolithBlueprintVariableActions.h"
#include "MonolithBlueprintStructActions.h"
#include "MonolithBlueprintComponentActions.h"
#include "MonolithBlueprintComponentResolver.h"
#include "MonolithBlueprintNodeActions.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "UObject/Package.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "GameFramework/Character.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_SwitchInteger.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithBlueprintWritePreflightTest,
	"Monolith.Blueprint.Writes.Preflight", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithBlueprintWritePreflightTest::RunTest(const FString&)
{
	TArray<UBlueprint*> Fixtures;
	ON_SCOPE_EXIT
	{
		for (UBlueprint* BP : Fixtures)
		{
			FAssetRegistryModule::AssetDeleted(BP);
			BP->GetOutermost()->SetDirtyFlag(false);
			BP->ClearFlags(RF_Public | RF_Standalone);
			BP->MarkAsGarbage();
		}
	};
	auto Create = [&](UClass* Parent)
	{
		const FString Name = TEXT("BP_WritePreflight_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		UPackage* Package = CreatePackage(*(TEXT("/Game/Tests/Monolith/Blueprint/") + Name));
		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(Parent, Package, FName(*Name), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (BP) { Fixtures.Add(BP); FAssetRegistryModule::AssetCreated(BP); }
		return BP;
	};
	UBlueprint* Parent = Create(ACharacter::StaticClass());
	if (!TestNotNull(TEXT("Parent fixture"), Parent)) return false;
	USCS_Node* Component = Parent->SimpleConstructionScript->CreateNode(USceneComponent::StaticClass(), TEXT("FixtureScene"));
	Parent->SimpleConstructionScript->AddNode(Component);
	FKismetEditorUtilities::CompileBlueprint(Parent);
	UBlueprint* Child = Create(Parent->GeneratedClass);
	if (!TestNotNull(TEXT("Child fixture"), Child)) return false;
	FKismetEditorUtilities::CompileBlueprint(Child);

	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), Child->GetPathName());
	Params->SetStringField(TEXT("component_name"), TEXT("FixtureScene"));
	Params->SetStringField(TEXT("property_name"), TEXT("NoSuchProperty"));
	Params->SetStringField(TEXT("value"), TEXT("true"));
	Child->GetOutermost()->SetDirtyFlag(false);
	TestFalse(TEXT("Missing inherited property refused"), FMonolithBlueprintComponentActions::HandleSetComponentProperty(Params).bSuccess);
	TestNull(TEXT("Refusal creates no inherited override"), MonolithBlueprintComponentResolver::FindExistingIchOverride(Child, Component));
	TestFalse(TEXT("Refusal leaves child clean"), Child->GetOutermost()->IsDirty());
	Params->SetStringField(TEXT("property_name"), TEXT("bVisible"));
	Params->SetStringField(TEXT("value"), TEXT("false trailing_invalid_text"));
	TestFalse(TEXT("Partially parsed component value refused"), FMonolithBlueprintComponentActions::HandleSetComponentProperty(Params).bSuccess);
	TestNull(TEXT("Bad value creates no inherited override"), MonolithBlueprintComponentResolver::FindExistingIchOverride(Child, Component));
	TestFalse(TEXT("Bad value leaves child clean"), Child->GetOutermost()->IsDirty());
	TestFalse(TEXT("Root alias respects required skeletal mesh class"),
		MonolithBlueprintComponentResolver::Resolve(Child, TEXT("Root"), USkeletalMeshComponent::StaticClass(), false).IsValid());

	FEdGraphPinType Bool; Bool.PinCategory = UEdGraphSchema_K2::PC_Boolean;
	FBlueprintEditorUtils::AddMemberVariable(Child, TEXT("Existing"), Bool);
	UEdGraph* Function = FBlueprintEditorUtils::CreateNewGraph(Child, TEXT("FixtureFunction"), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(Child, Function, false, nullptr);
	FKismetEditorUtilities::CompileBlueprint(Child);
	Child->GetOutermost()->SetDirtyFlag(false);
	Params->SetStringField(TEXT("type"), TEXT("map:string:object:MonolithMissingClass"));
	Params->SetStringField(TEXT("name"), TEXT("Existing"));
	TestFalse(TEXT("set_variable_type rejects unresolved nested type"), FMonolithBlueprintVariableActions::HandleSetVariableType(Params).bSuccess);
	TestEqual(TEXT("Existing variable type unchanged"), Child->NewVariables[0].VarType.PinCategory, FName(UEdGraphSchema_K2::PC_Boolean));
	Params->SetStringField(TEXT("name"), TEXT("Local"));
	Params->SetStringField(TEXT("function_name"), TEXT("FixtureFunction"));
	TestFalse(TEXT("add_local_variable rejects unresolved nested type"), FMonolithBlueprintVariableActions::HandleAddLocalVariable(Params).bSuccess);
	TArray<UK2Node_FunctionEntry*> Entries; Function->GetNodesOfClass(Entries);
	if (TestEqual(TEXT("Function has entry"), Entries.Num(), 1)) TestEqual(TEXT("No local variable created"), Entries[0]->LocalVariables.Num(), 0);
	Params->SetStringField(TEXT("variable_name"), TEXT("Replicated"));
	TestFalse(TEXT("add_replicated_variable rejects unresolved nested type"), FMonolithBlueprintVariableActions::HandleAddReplicatedVariable(Params).bSuccess);
	TestEqual(TEXT("No replicated variable created"), Child->NewVariables.Num(), 1);
	TestFalse(TEXT("Invalid types leave Blueprint clean"), Child->GetOutermost()->IsDirty());

	UEdGraph* Graph = Child->UbergraphPages[0];
	FGraphNodeCreator<UK2Node_SwitchInteger> Creator(*Graph);
	UK2Node_SwitchInteger* Switch = Creator.CreateNode(); Creator.Finalize();
	Switch->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, TEXT("0"));
	Params->SetStringField(TEXT("graph_name"), Graph->GetName());
	Params->SetStringField(TEXT("node_id"), Switch->NodeGuid.ToString());
	Params->SetStringField(TEXT("property_name"), TEXT("NodeGuid"));
	Params->SetStringField(TEXT("value"), TEXT("0"));
	Child->GetOutermost()->SetDirtyFlag(false);
	TestFalse(TEXT("Internal node identity property is protected"), FMonolithBlueprintNodeActions::HandleSetGraphNodeProperty(Params).bSuccess);
	TestFalse(TEXT("Protected edit leaves Blueprint clean"), Child->GetOutermost()->IsDirty());
	Params->SetStringField(TEXT("property_name"), TEXT("StartIndex"));
	Params->SetStringField(TEXT("value"), TEXT("7"));
	const auto Edited = FMonolithBlueprintNodeActions::HandleSetGraphNodeProperty(Params);
	if (TestTrue(FString::Printf(TEXT("Editable switch property succeeds: %s"), *Edited.ErrorMessage), Edited.bSuccess))
	{
		TestEqual(TEXT("Actual switch start index changed"), Switch->StartIndex, 7);
		TestNotNull(TEXT("Switch reconstruction renames existing case pin"), Switch->FindPin(TEXT("7"), EGPD_Output));
		TestNull(TEXT("Obsolete case name removed"), Switch->FindPin(TEXT("0"), EGPD_Output));
		TestFalse(TEXT("Graph property change defaults to no save"), Edited.Result->GetBoolField(TEXT("saved")));
		TestTrue(TEXT("Current pins returned"), Edited.Result->HasField(TEXT("pins")));
	}
	// An in-memory Engine-mount fixture proves direct handler calls cannot bypass the
	// protection merely because the property is editable. Nothing is saved to disk.
	const FString EngineName = TEXT("BP_EngineGuard_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* EnginePackage = CreatePackage(*(TEXT("/Engine/MonolithTests/") + EngineName));
	UBlueprint* EngineBP = NewObject<UBlueprint>(EnginePackage, *EngineName, RF_Public | RF_Standalone);
	Fixtures.Add(EngineBP); FAssetRegistryModule::AssetCreated(EngineBP);
	UEdGraph* EngineGraph = NewObject<UEdGraph>(EngineBP, TEXT("GuardGraph"));
	EngineGraph->Schema = UEdGraphSchema_K2::StaticClass(); EngineBP->UbergraphPages.Add(EngineGraph);
	UK2Node_SwitchInteger* EngineNode = NewObject<UK2Node_SwitchInteger>(EngineGraph);
	EngineNode->CreateNewGuid(); EngineGraph->AddNode(EngineNode);
	EnginePackage->SetDirtyFlag(false);
	Params->SetStringField(TEXT("asset_path"), EngineBP->GetPathName());
	Params->SetStringField(TEXT("graph_name"), EngineGraph->GetName());
	Params->SetStringField(TEXT("node_id"), EngineNode->NodeGuid.ToString());
	Params->SetStringField(TEXT("property_name"), TEXT("StartIndex"));
	Params->SetStringField(TEXT("value"), TEXT("99"));
	const auto EngineRefused = FMonolithBlueprintNodeActions::HandleSetGraphNodeProperty(Params);
	TestFalse(TEXT("Editable property on Engine asset refused"), EngineRefused.bSuccess);
	if (TestTrue(TEXT("Engine refusal has structured data"), EngineRefused.ErrorData.IsValid()))
		TestEqual(TEXT("Engine refusal is path protection, not lookup failure"), EngineRefused.ErrorData->AsObject()->GetStringField(TEXT("reason")), FString(TEXT("path_not_writable")));
	TestEqual(TEXT("Engine node value unchanged"), EngineNode->StartIndex, 0);
	TestFalse(TEXT("Engine package remains clean"), EnginePackage->IsDirty());

	const FString StructPath = TEXT("/Game/Tests/Monolith/Blueprint/S_Preflight_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	auto StructParams = MakeShared<FJsonObject>();
	StructParams->SetStringField(TEXT("save_path"), StructPath);
	auto Field = MakeShared<FJsonObject>();
	Field->SetStringField(TEXT("name"), TEXT("Value"));
	Field->SetStringField(TEXT("type"), TEXT("MonolithInvalidType"));
	StructParams->SetArrayField(TEXT("fields"), {MakeShared<FJsonValueObject>(Field)});
	TestFalse(TEXT("Invalid struct field type rejected"), FMonolithBlueprintStructActions::HandleCreateUserDefinedStruct(StructParams).bSuccess);
	TestNull(TEXT("Invalid struct creates no package"), FindPackage(nullptr, *StructPath));

	return true;
}
#endif
