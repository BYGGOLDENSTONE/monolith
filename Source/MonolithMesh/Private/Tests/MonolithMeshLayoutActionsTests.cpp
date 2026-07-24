// Copyright tumourlove. All Rights Reserved.

// =============================================================================
// MonolithMeshLayoutActionsTests.cpp
//
// Phase 2 (Goldenstone roadmap), slice 4 — the data-driven level layout system.
//
// WHAT THESE TESTS PROVE HEADLESS
//   - a layout document places real actors through the EXISTING spawn actions and
//     tags every one of them with its layout id + entry id,
//   - IDENTITY / IDEMPOTENCY: applying the same layout twice leaves exactly the
//     same number of actors (no duplicates) and reports the previous instance it
//     retired; on_existing="skip" leaves the already-placed actor objects alone,
//   - the ALL-OR-NOTHING contract: one unresolvable entry fails the call, reports
//     EVERY problem, and leaves zero actors of that layout in the level,
//   - the validation surface: duplicate ids, unknown kinds, unknown fields (checked
//     against the target action's real param schema), bad presets, preset/type
//     mismatches and malformed vectors are all refused before anything is placed,
//   - COMPOSABILITY: an entry that names a light preset really does get the
//     preset's property values, read back through mesh.get_light_properties,
//   - removal by tag deletes one layout without touching another,
//   - SHIPPED DATA: every entry of every layout in Config/MonolithLevelLayouts.json
//     resolves against the real engine — classes, mesh assets, light/atmosphere
//     type tokens, preset names, preset/type compatibility, and every UPROPERTY
//     name inside a `properties` bag. This is the engine-upgrade canary: if UE
//     renames a light property or moves an engine mesh, this test goes red before
//     a user hits it.
//   - SCALE: a 150-entry layout is applied and its wall time logged (see the
//     LargeLayoutTiming test output) — the measurement behind the decision to keep
//     apply synchronous instead of routing it through the Phase 1 job system.
//   - the FREE-FORM PROPERTY CHANNEL on `kind: "actor"`: mesh.spawn_actor's two
//     bags (`properties` -> the actor, `component_properties` -> its root
//     component) are written by reflection, read back off the placed objects, and
//     a mistyped name is refused in PHASE 1 with a did-you-mean and zero world
//     mutation — both called directly and through a layout.
//   - `kind: "volume"`: a layout places real ATriggerVolume/APainCausingVolume
//     actors through mesh.spawn_volume, and a property key that volume type does
//     not honour is an error rather than a silent no-op.
//   - PURE PASS-THROUGH, PROVEN NOT ASSUMED: the newest atmosphere type
//     (`volumetric_cloud`) works inside a layout with no layout-side change — by
//     preset AND by raw properties AND by alias — and captures back as a canonical
//     `kind: atmosphere` / `type: volumetric_cloud` entry instead of being skipped.
//
// WHAT THESE TESTS DO NOT PROVE
//   - anything about rendered output. The nightly suite runs `-nullrhi`. Whether
//     the demo room LOOKS right is the human acceptance step (apply it in a real
//     editor, then editor.capture_viewport).
//   - undo. The apply runs inside one editor transaction, but driving Ctrl+Z from
//     an automation test that never ticks the editor is not meaningful, so the
//     transaction is exercised only in that it opens and closes cleanly.
//   - the no-level-open branch: the automation run always has an editor world.
// =============================================================================

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "MonolithMeshAtmosphereActions.h"
#include "MonolithMeshLayoutActions.h"
#include "MonolithMeshLevelDesignActions.h"
#include "MonolithMeshLightActions.h"
#include "MonolithMeshSceneActions.h"
#include "MonolithMeshUtils.h"
#include "MonolithToolRegistry.h"
#include "Reflection/MonolithReflectionWalker.h"

#include "MonolithMeshVolumeActions.h"

#include "Components/LightComponentBase.h"
#include "Components/VolumetricCloudComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TriggerVolume.h"
#include "Engine/World.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "GameFramework/Actor.h"
#include "Reflection/MonolithReflectionReader.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// --- fixture Blueprint construction (see CreateOrReuseTestActorBlueprint) ---
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/PainCausingVolume.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Sound/AudioVolume.h"
#include "UObject/SavePackage.h"

namespace MonolithLayoutTestUtils
{
	static UWorld* GetTestWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	/** Registration is skipped in some commandlet contexts — register on demand. */
	static void EnsureRegistered()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("mesh"), TEXT("spawn_actor")))
		{
			FMonolithMeshSceneActions::RegisterActions(Registry);
		}
		if (!Registry.HasAction(TEXT("mesh"), TEXT("place_light")))
		{
			FMonolithMeshLevelDesignActions::RegisterActions(Registry);
		}
		if (!Registry.HasAction(TEXT("mesh"), TEXT("get_light_properties")))
		{
			FMonolithMeshLightActions::RegisterActions(Registry);
		}
		if (!Registry.HasAction(TEXT("mesh"), TEXT("spawn_atmosphere")))
		{
			FMonolithMeshAtmosphereActions::RegisterActions(Registry);
		}
		if (!Registry.HasAction(TEXT("mesh"), TEXT("spawn_volume")))
		{
			FMonolithMeshVolumeActions::RegisterActions(Registry);
		}
		// Layout actions must register AFTER the actions they resolve schemas against.
		if (!Registry.HasAction(TEXT("mesh"), TEXT("apply_level_layout")))
		{
			FMonolithMeshLayoutActions::RegisterActions(Registry);
		}
	}

	static FMonolithActionResult Exec(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		EnsureRegistered();
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), Action, Params);
	}

	static TArray<TSharedPtr<FJsonValue>> Vec(double X, double Y, double Z)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(X));
		Arr.Add(MakeShared<FJsonValueNumber>(Y));
		Arr.Add(MakeShared<FJsonValueNumber>(Z));
		return Arr;
	}

	/** One entry object: {id, kind, ...}. */
	static TSharedPtr<FJsonObject> Entry(const FString& Id, const FString& Kind)
	{
		auto E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("id"), Id);
		E->SetStringField(TEXT("kind"), Kind);
		return E;
	}

	/** A layout body from a list of entries. */
	static TSharedPtr<FJsonObject> Body(const TArray<TSharedPtr<FJsonObject>>& Entries)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const TSharedPtr<FJsonObject>& E : Entries)
		{
			Arr.Add(MakeShared<FJsonValueObject>(E));
		}
		auto B = MakeShared<FJsonObject>();
		B->SetArrayField(TEXT("entries"), Arr);
		return B;
	}

	static TSharedPtr<FJsonObject> ApplyParams(const FString& LayoutId, const TSharedPtr<FJsonObject>& InBody)
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("layout"), LayoutId);
		P->SetObjectField(TEXT("layout_json"), InBody);
		return P;
	}

	static FMonolithActionResult Remove(const FString& LayoutId)
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("layout"), LayoutId);
		return Exec(TEXT("remove_level_layout"), P);
	}

	static int32 CountInLevel(const FString& LayoutId)
	{
		return FMonolithMeshLayoutActions::FindLayoutActors(GetTestWorld(), LayoutId).Num();
	}

	// --- capture helpers -------------------------------------------------

	/** The document body mesh.capture_level_layout returned, or null. */
	static TSharedPtr<FJsonObject> BodyOf(const FMonolithActionResult& R)
	{
		const TSharedPtr<FJsonObject>* B = nullptr;
		if (R.bSuccess && R.Result.IsValid() && R.Result->TryGetObjectField(TEXT("layout_json"), B) && B)
		{
			return *B;
		}
		return nullptr;
	}

	/** One entry of a captured body by its id, or null. */
	static TSharedPtr<FJsonObject> EntryById(const TSharedPtr<FJsonObject>& InBody, const FString& Id)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!InBody.IsValid() || !InBody->TryGetArrayField(TEXT("entries"), Arr) || !Arr)
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* E = nullptr;
			FString EntryId;
			if (V.IsValid() && V->TryGetObject(E) && E && (*E)->TryGetStringField(TEXT("id"), EntryId) && EntryId == Id)
			{
				return *E;
			}
		}
		return nullptr;
	}

	/** A named property bag on an entry, or null. */
	static TSharedPtr<FJsonObject> BagOf(const TSharedPtr<FJsonObject>& InEntry, const TCHAR* Key)
	{
		const TSharedPtr<FJsonObject>* B = nullptr;
		if (InEntry.IsValid() && InEntry->TryGetObjectField(Key, B) && B)
		{
			return *B;
		}
		return nullptr;
	}

	/** One component of an [x,y,z] array field. */
	static double AxisOf(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, int32 Index)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetArrayField(Key, Arr) || !Arr || !Arr->IsValidIndex(Index))
		{
			return TNumericLimits<double>::Max();
		}
		return (*Arr)[Index]->AsNumber();
	}

	/** Capture params with the common defaults filled in. */
	static TSharedPtr<FJsonObject> CaptureParams(const FString& LayoutId)
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("layout"), LayoutId);
		return P;
	}

	/** The layout actor placed for a given entry id, or nullptr. */
	static AActor* ActorForEntry(const FString& LayoutId, const FString& EntryId)
	{
		for (AActor* Actor : FMonolithMeshLayoutActions::FindLayoutActors(GetTestWorld(), LayoutId))
		{
			if (FMonolithMeshLayoutActions::GetEntryId(Actor) == EntryId)
			{
				return Actor;
			}
		}
		return nullptr;
	}

	/** A layout document body serialised to a stable string, for identity comparisons. */
	static FString Canonicalise(const TSharedPtr<FJsonObject>& InBody)
	{
		FString Out;
		if (!InBody.IsValid())
		{
			return Out;
		}
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(InBody.ToSharedRef(), Writer);
		return Out;
	}

	/** How many actors of exactly this class are in the test world. */
	static int32 CountActorsOfClass(UClass* Class)
	{
		int32 Count = 0;
		UWorld* World = GetTestWorld();
		if (!World || !Class)
		{
			return Count;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (IsValid(*It) && It->GetClass() == Class) { ++Count; }
		}
		return Count;
	}

	/**
	 * Create (or reclaim) a throwaway ACTOR Blueprint whose ROOT COMPONENT comes from
	 * the SimpleConstructionScript, and return its generated class.
	 *
	 * The SCS root is the POINT, not an implementation detail. A Blueprint built this
	 * way has NO root component on its class default object, so mesh.spawn_actor cannot
	 * pre-validate `component_properties` against the CDO and has to defer them to the
	 * post-spawn write — the one write in the spawn path whose rejection must roll the
	 * spawn back. Blueprint actors were unreachable until this slice, so that branch had
	 * never been exercised; the tests below drive it deliberately.
	 *
	 * MonolithDev is not assumed to contain any particular Blueprint: the fixture is
	 * built here, so the suite carries no dependency on project content.
	 *
	 * Re-run safety follows MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint —
	 * FullyLoad() before FindObject/SavePackage, or a fixture left on disk by an earlier
	 * run is only partially loaded and SavePackage is fatal.
	 */
	static UClass* CreateOrReuseTestActorBlueprint(const FString& AssetPath, FString& OutError)
	{
		OutError.Reset();

		FString PackagePath, AssetName;
		if (!AssetPath.Split(TEXT("/"), &PackagePath, &AssetName,
				ESearchCase::IgnoreCase, ESearchDir::FromEnd) || AssetName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Cannot split asset path '%s'"), *AssetPath);
			return nullptr;
		}

		UPackage* Package = CreatePackage(*AssetPath);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("CreatePackage failed for '%s'"), *AssetPath);
			return nullptr;
		}
		Package->FullyLoad();

		UBlueprint* BP = FindObject<UBlueprint>(Package, *AssetName);
		if (!BP)
		{
			BP = FKismetEditorUtilities::CreateBlueprint(
				AActor::StaticClass(), Package, FName(*AssetName), BPTYPE_Normal);
		}
		if (!BP)
		{
			OutError = FString::Printf(TEXT("CreateBlueprint failed for '%s'"), *AssetPath);
			return nullptr;
		}

		if (!BP->SimpleConstructionScript)
		{
			OutError = TEXT("the fixture Blueprint has no SimpleConstructionScript");
			return nullptr;
		}
		if (BP->SimpleConstructionScript->GetAllNodes().Num() == 0)
		{
			USCS_Node* Node = BP->SimpleConstructionScript->CreateNode(
				USceneComponent::StaticClass(), TEXT("FixtureRoot"));
			if (!Node)
			{
				OutError = TEXT("could not create the fixture's SCS root node");
				return nullptr;
			}
			BP->SimpleConstructionScript->AddNode(Node);
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		}

		FKismetEditorUtilities::CompileBlueprint(BP);
		FAssetRegistryModule::AssetCreated(BP);
		Package->MarkPackageDirty();

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		const FString Filename = FPackageName::LongPackageNameToFilename(
			AssetPath, FPackageName::GetAssetPackageExtension());
		if (!UPackage::SavePackage(Package, BP, *Filename, SaveArgs))
		{
			OutError = FString::Printf(TEXT("SavePackage failed for '%s'"), *Filename);
			return nullptr;
		}

		if (!BP->GeneratedClass)
		{
			OutError = TEXT("the fixture Blueprint compiled to no generated class");
			return nullptr;
		}
		return BP->GeneratedClass;
	}

	/** The one fixture Blueprint this file uses. */
	static const TCHAR* FixtureBlueprintPath() { return TEXT("/Game/Tests/Monolith/Mesh/BP_MonolithLayoutFixture"); }

	/** A minimal two-entry layout: one cube-less StaticMeshActor + one warm point light. */
	static TSharedPtr<FJsonObject> MakeSimpleBody()
	{
		TSharedPtr<FJsonObject> Prop = Entry(TEXT("marker"), TEXT("actor"));
		Prop->SetStringField(TEXT("class"), TEXT("StaticMeshActor"));
		Prop->SetArrayField(TEXT("location"), Vec(100.0, 0.0, 0.0));

		TSharedPtr<FJsonObject> Lamp = Entry(TEXT("lamp"), TEXT("light"));
		Lamp->SetStringField(TEXT("type"), TEXT("point"));
		Lamp->SetStringField(TEXT("preset"), TEXT("bulb_warm_60w"));
		Lamp->SetArrayField(TEXT("location"), Vec(100.0, 0.0, 200.0));

		return Body({ Prop, Lamp });
	}
}

// ============================================================================
// 1. Registration
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutRegistrationTest,
	"Monolith.Mesh.Layouts.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutRegistrationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;
	EnsureRegistered();

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const TCHAR* Expected[] = {
		TEXT("list_level_layouts"), TEXT("describe_level_layout"), TEXT("apply_level_layout"),
		TEXT("remove_level_layout"), TEXT("save_level_layout"), TEXT("capture_level_layout"),
	};
	for (const TCHAR* Action : Expected)
	{
		TestTrue(FString::Printf(TEXT("mesh.%s is registered"), Action),
			Registry.HasAction(TEXT("mesh"), Action));
	}

	// The kind table must only name actions that actually exist, or every layout
	// using that kind would fail at resolve time with a confusing message.
	for (const FMonolithMeshLayoutActions::FKind& Kind : FMonolithMeshLayoutActions::GetKinds())
	{
		TestTrue(FString::Printf(TEXT("kind '%s' delegates to a registered action %s.%s"),
			*Kind.Token, *Kind.Namespace, *Kind.Action),
			Registry.HasAction(Kind.Namespace, Kind.Action));
	}

	return true;
}

// ============================================================================
// 2. Shipped data is valid — the engine-upgrade canary.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutShippedDataTest,
	"Monolith.Mesh.Layouts.ShippedLayoutDataIsValid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutShippedDataTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;
	EnsureRegistered();

	const FString BuiltinFile = FMonolithMeshLayoutActions::Library().GetBuiltinFile();
	TestTrue(FString::Printf(TEXT("built-in layout file ships with the plugin: %s"), *BuiltinFile),
		IFileManager::Get().FileExists(*BuiltinFile));

	TArray<FString> Warnings;
	const TMap<FString, FMonolithNamedJsonObject> Layouts = FMonolithMeshLayoutActions::LoadLayouts(Warnings);

	TestTrue(TEXT("at least one layout ships"), Layouts.Num() > 0);
	for (const FString& W : Warnings)
	{
		AddWarning(FString::Printf(TEXT("layout library warning: %s"), *W));
	}

	for (const TPair<FString, FMonolithNamedJsonObject>& Pair : Layouts)
	{
		// Only assert on built-ins — a user file in Saved/ is the user's business.
		if (Pair.Value.SourceFile != BuiltinFile)
		{
			continue;
		}

		TArray<FMonolithMeshLayoutActions::FResolvedEntry> Resolved;
		TArray<FString> Problems;
		const bool bOk = FMonolithMeshLayoutActions::ResolveLayout(
			Pair.Key, Pair.Value.Object, FVector::ZeroVector, FString(), Resolved, Problems);

		TestTrue(FString::Printf(TEXT("shipped layout '%s' resolves cleanly (%d problem(s): %s)"),
			*Pair.Key, Problems.Num(), *FString::Join(Problems, TEXT("; "))), bOk);

		if (!bOk)
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("shipped layout '%s' has entries"), *Pair.Key), Resolved.Num() > 0);

		// Deeper canary: every UPROPERTY name in an entry's property bags must
		// resolve on the real engine struct the write would target. ResolveLayout
		// now validates the ACTOR and VOLUME bags itself (so bOk above already
		// covers those), but the light/atmosphere bags are validated by their write
		// path at apply time, and shipped data must never name a property that moved.
		auto CheckBag = [&](const FString& EntryId, const TCHAR* BagName,
			const TSharedPtr<FJsonObject>& Bag, UStruct* Target)
		{
			TestNotNull(*FString::Printf(
				TEXT("'%s.%s': could resolve the struct its `%s` are written into"),
				*Pair.Key, *EntryId, BagName), Target);
			if (!Target || !Bag.IsValid())
			{
				return;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Prop : Bag->Values)
			{
				TestNotNull(*FString::Printf(TEXT("'%s.%s': %s property '%s' exists on %s"),
					*Pair.Key, *EntryId, BagName, *Prop.Key, *Target->GetName()),
					FMonolithReflectionWalker::FindPropertyForwarding(Target, Prop.Key));
			}
		};

		for (const FMonolithMeshLayoutActions::FResolvedEntry& E : Resolved)
		{
			FString TypeStr;
			E.Params->TryGetStringField(TEXT("type"), TypeStr);

			// spawn_volume's `properties` bag is a reflection channel like the others
			// now, so it gets the same canary — but its keys may be written in the
			// curated snake_case spelling, which is a legal alias, so they are
			// translated before being looked up.
			if (E.KindToken == TEXT("volume"))
			{
				const TSharedPtr<FJsonObject>* VolPropsPtr = nullptr;
				if (!E.Params->TryGetObjectField(TEXT("properties"), VolPropsPtr) || !VolPropsPtr)
				{
					continue;
				}
				FString VolumeTypeError;
				UClass* VolumeClass =
					FMonolithMeshVolumeActions::ResolveVolumeClass(TypeStr, VolumeTypeError);
				CheckBag(E.EntryId, TEXT("properties"),
					FMonolithMeshVolumeActions::TranslateVolumeProperties(*VolPropsPtr), VolumeClass);
				continue;
			}

			const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
			const bool bHasProps = E.Params->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr;

			if (E.KindToken == TEXT("actor"))
			{
				// Two bags, two targets: `properties` -> the actor class,
				// `component_properties` -> its root component class.
				const TSharedPtr<FJsonObject>* CompPtr = nullptr;
				const bool bHasComp =
					E.Params->TryGetObjectField(TEXT("component_properties"), CompPtr) && CompPtr;
				if (!bHasProps && !bHasComp)
				{
					continue;
				}

				FString ClassOrMesh;
				E.Params->TryGetStringField(TEXT("class_or_mesh"), ClassOrMesh);
				UClass* Cls = nullptr;
				UStaticMesh* Mesh = nullptr;
				FString ResolveErr;
				UClass* ActorClass = nullptr;
				if (FMonolithMeshSceneActions::ResolveSpawnTarget(ClassOrMesh, Cls, Mesh, ResolveErr))
				{
					ActorClass = Mesh ? AStaticMeshActor::StaticClass() : Cls;
				}

				if (bHasProps)
				{
					CheckBag(E.EntryId, TEXT("properties"), *PropsPtr, ActorClass);
				}
				if (bHasComp)
				{
					UStruct* CompClass = nullptr;
					if (ActorClass)
					{
						if (AActor* CDO = Cast<AActor>(ActorClass->GetDefaultObject()))
						{
							if (USceneComponent* Root = CDO->GetRootComponent())
							{
								CompClass = Root->GetClass();
							}
						}
					}
					CheckBag(E.EntryId, TEXT("component_properties"), *CompPtr, CompClass);
				}
				continue;
			}

			if (!bHasProps)
			{
				continue;
			}

			UStruct* Target = nullptr;
			if (E.KindToken == TEXT("light"))
			{
				FString Err;
				if (UClass* ActorClass = FMonolithMeshLightActions::ResolveLightActorClass(TypeStr, Err))
				{
					if (AActor* CDO = Cast<AActor>(ActorClass->GetDefaultObject()))
					{
						FString CompErr;
						if (ULightComponentBase* Comp = FMonolithMeshLightActions::ResolveLightComponent(CDO, CompErr))
						{
							Target = Comp->GetClass();
						}
					}
				}
			}
			else if (E.KindToken == TEXT("atmosphere"))
			{
				Target = FMonolithMeshAtmosphereActions::ResolveSectionStruct(TypeStr);
			}

			CheckBag(E.EntryId, TEXT("properties"), *PropsPtr, Target);
		}
	}

	return true;
}

// ============================================================================
// 3. list + describe
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutListDescribeTest,
	"Monolith.Mesh.Layouts.ListAndDescribe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutListDescribeTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;

	FMonolithActionResult ListResult = Exec(TEXT("list_level_layouts"), MakeShared<FJsonObject>());
	TestTrue(TEXT("list_level_layouts succeeds"), ListResult.bSuccess);
	if (!ListResult.bSuccess) { return false; }

	const TArray<TSharedPtr<FJsonValue>>* LayoutArr = nullptr;
	TestTrue(TEXT("list returns a layouts array"),
		ListResult.Result->TryGetArrayField(TEXT("layouts"), LayoutArr) && LayoutArr);
	if (!LayoutArr) { return false; }

	bool bFoundDemo = false;
	for (const TSharedPtr<FJsonValue>& V : *LayoutArr)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!V->TryGetObject(Obj)) { continue; }
		FString Id;
		(*Obj)->TryGetStringField(TEXT("layout"), Id);
		if (Id == TEXT("demo_lit_room"))
		{
			bFoundDemo = true;
			bool bValid = false;
			(*Obj)->TryGetBoolField(TEXT("valid"), bValid);
			TestTrue(TEXT("shipped demo_lit_room is reported valid by list"), bValid);
			double EntryCount = 0;
			(*Obj)->TryGetNumberField(TEXT("entry_count"), EntryCount);
			TestTrue(TEXT("demo_lit_room has entries"), EntryCount > 0);
		}
	}
	TestTrue(TEXT("list includes the shipped demo_lit_room layout"), bFoundDemo);

	// describe resolves against the live engine and reports world status.
	auto DescribeParams = MakeShared<FJsonObject>();
	DescribeParams->SetStringField(TEXT("layout"), TEXT("demo_lit_room"));
	FMonolithActionResult Describe = Exec(TEXT("describe_level_layout"), DescribeParams);
	TestTrue(TEXT("describe_level_layout succeeds"), Describe.bSuccess);
	if (Describe.bSuccess)
	{
		bool bValid = false;
		Describe.Result->TryGetBoolField(TEXT("valid"), bValid);
		TestTrue(TEXT("describe reports demo_lit_room valid"), bValid);

		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		TestTrue(TEXT("describe returns entries"),
			Describe.Result->TryGetArrayField(TEXT("entries"), Entries) && Entries);
		if (Entries && Entries->Num() > 0)
		{
			const TSharedPtr<FJsonObject>* First = nullptr;
			(*Entries)[0]->TryGetObject(First);
			FString PlacesVia;
			(*First)->TryGetStringField(TEXT("places_via"), PlacesVia);
			TestTrue(TEXT("describe names the action each entry places through"),
				PlacesVia.StartsWith(TEXT("mesh.")));
			bool bInLevel = true;
			(*First)->TryGetBoolField(TEXT("in_level"), bInLevel);
			TestFalse(TEXT("nothing from the shipped demo layout is in the level yet"), bInLevel);
		}
	}

	// Unknown layout name is an actionable error that lists what IS available.
	auto BadParams = MakeShared<FJsonObject>();
	BadParams->SetStringField(TEXT("layout"), TEXT("no_such_layout_xyz"));
	FMonolithActionResult Bad = Exec(TEXT("describe_level_layout"), BadParams);
	TestFalse(TEXT("unknown layout id is refused"), Bad.bSuccess);
	TestTrue(TEXT("unknown layout error lists the available layouts"),
		Bad.ErrorMessage.Contains(TEXT("demo_lit_room")));

	return true;
}

// ============================================================================
// 4. apply places tagged, foldered, labelled actors — and composes presets.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutApplyTest,
	"Monolith.Mesh.Layouts.ApplyPlacesTaggedActors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutApplyTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;

	const FString LayoutId = TEXT("monolith_test_apply");
	Remove(LayoutId);

	FMonolithActionResult Result = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, MakeSimpleBody()));
	TestTrue(FString::Printf(TEXT("apply succeeds (%s)"), *Result.ErrorMessage), Result.bSuccess);
	if (!Result.bSuccess) { return false; }

	double Placed = 0;
	Result.Result->TryGetNumberField(TEXT("placed"), Placed);
	TestEqual(TEXT("both entries were placed"), static_cast<int32>(Placed), 2);
	TestEqual(TEXT("both actors carry the layout tag"), CountInLevel(LayoutId), 2);

	// Identity: each actor carries its entry id, labels and folder are derived.
	TSet<FString> FoundEntries;
	for (AActor* Actor : FMonolithMeshLayoutActions::FindLayoutActors(GetTestWorld(), LayoutId))
	{
		const FString EntryId = FMonolithMeshLayoutActions::GetEntryId(Actor);
		FoundEntries.Add(EntryId);
		TestEqual(*FString::Printf(TEXT("entry '%s' actor label is derived from the layout"), *EntryId),
			Actor->GetActorLabel(), FString::Printf(TEXT("%s.%s"), *LayoutId, *EntryId));
		TestEqual(*FString::Printf(TEXT("entry '%s' actor is in the layout folder"), *EntryId),
			Actor->GetFolderPath().ToString(), FString::Printf(TEXT("Monolith/Layouts/%s"), *LayoutId));
	}
	TestTrue(TEXT("the 'marker' entry is present"), FoundEntries.Contains(TEXT("marker")));
	TestTrue(TEXT("the 'lamp' entry is present"), FoundEntries.Contains(TEXT("lamp")));

	// Composability: the lamp entry named a preset instead of restating values, so
	// the placed light must actually carry the preset's Intensity.
	FMonolithMeshLightActions::FPreset Preset;
	FString PresetError;
	if (FMonolithMeshLightActions::ResolvePreset(TEXT("bulb_warm_60w"), Preset, PresetError))
	{
		double Expected = 0;
		const bool bPresetHasIntensity =
			Preset.Properties.IsValid() && Preset.Properties->TryGetNumberField(TEXT("Intensity"), Expected);
		TestTrue(TEXT("the bulb_warm_60w preset defines an Intensity to compare against"), bPresetHasIntensity);

		if (bPresetHasIntensity)
		{
			auto ReadParams = MakeShared<FJsonObject>();
			ReadParams->SetStringField(TEXT("actor_name"), FString::Printf(TEXT("%s.lamp"), *LayoutId));
			TArray<TSharedPtr<FJsonValue>> Wanted;
			Wanted.Add(MakeShared<FJsonValueString>(TEXT("Intensity")));
			ReadParams->SetArrayField(TEXT("properties"), Wanted);

			FMonolithActionResult Read = Exec(TEXT("get_light_properties"), ReadParams);
			TestTrue(FString::Printf(TEXT("get_light_properties on the placed lamp succeeds (%s)"),
				*Read.ErrorMessage), Read.bSuccess);
			if (Read.bSuccess)
			{
				const TSharedPtr<FJsonObject>* Props = nullptr;
				double Actual = -1.0;
				if (Read.Result->TryGetObjectField(TEXT("properties"), Props) && Props)
				{
					(*Props)->TryGetNumberField(TEXT("Intensity"), Actual);
				}
				TestTrue(FString::Printf(
					TEXT("the layout's preset reference produced the preset value (expected %f, got %f)"),
					Expected, Actual), FMath::IsNearlyEqual(Actual, Expected, 0.01));
			}
		}
	}
	else
	{
		AddWarning(FString::Printf(TEXT("bulb_warm_60w preset unavailable: %s"), *PresetError));
	}

	Remove(LayoutId);
	TestEqual(TEXT("cleanup removed the layout"), CountInLevel(LayoutId), 0);
	return true;
}

// ============================================================================
// 5. Idempotency — applying twice does not duplicate.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutIdempotencyTest,
	"Monolith.Mesh.Layouts.ApplyIsIdempotent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutIdempotencyTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;

	const FString LayoutId = TEXT("monolith_test_idempotent");
	Remove(LayoutId);

	FMonolithActionResult First = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, MakeSimpleBody()));
	TestTrue(FString::Printf(TEXT("first apply succeeds (%s)"), *First.ErrorMessage), First.bSuccess);
	if (!First.bSuccess) { return false; }
	TestEqual(TEXT("first apply places 2 actors"), CountInLevel(LayoutId), 2);

	FMonolithActionResult Second = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, MakeSimpleBody()));
	TestTrue(FString::Printf(TEXT("second apply succeeds (%s)"), *Second.ErrorMessage), Second.bSuccess);
	if (!Second.bSuccess) { Remove(LayoutId); return false; }

	TestEqual(TEXT("re-applying the same layout does NOT duplicate actors"), CountInLevel(LayoutId), 2);

	double RemovedPrevious = 0;
	Second.Result->TryGetNumberField(TEXT("removed_previous"), RemovedPrevious);
	TestEqual(TEXT("the second apply reports retiring the previous instance"),
		static_cast<int32>(RemovedPrevious), 2);

	// Labels must not drift across re-applies ("lamp" must not become "lamp2").
	bool bFoundLamp = false;
	for (AActor* Actor : FMonolithMeshLayoutActions::FindLayoutActors(GetTestWorld(), LayoutId))
	{
		if (FMonolithMeshLayoutActions::GetEntryId(Actor) == TEXT("lamp"))
		{
			bFoundLamp = true;
			TestEqual(TEXT("label is stable across re-apply"),
				Actor->GetActorLabel(), FString::Printf(TEXT("%s.lamp"), *LayoutId));
		}
	}
	TestTrue(TEXT("the lamp entry survived the re-apply"), bFoundLamp);

	// Removing an entry from the document removes it from the level (the world
	// matches the document, not the union of every apply).
	TSharedPtr<FJsonObject> Shrunk = MakeSimpleBody();
	{
		TSharedPtr<FJsonObject> Only = Entry(TEXT("lamp"), TEXT("light"));
		Only->SetStringField(TEXT("type"), TEXT("point"));
		Only->SetArrayField(TEXT("location"), Vec(100.0, 0.0, 200.0));
		Shrunk = Body({ Only });
	}
	FMonolithActionResult Third = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Shrunk));
	TestTrue(FString::Printf(TEXT("shrunk apply succeeds (%s)"), *Third.ErrorMessage), Third.bSuccess);
	TestEqual(TEXT("an entry deleted from the document disappears from the level"),
		CountInLevel(LayoutId), 1);

	Remove(LayoutId);
	TestEqual(TEXT("cleanup removed the layout"), CountInLevel(LayoutId), 0);
	return true;
}

// ============================================================================
// 6. on_existing = skip
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutSkipModeTest,
	"Monolith.Mesh.Layouts.SkipModeLeavesExistingActors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutSkipModeTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;

	const FString LayoutId = TEXT("monolith_test_skip");
	Remove(LayoutId);

	// Place only the lamp first.
	TSharedPtr<FJsonObject> Lamp = Entry(TEXT("lamp"), TEXT("light"));
	Lamp->SetStringField(TEXT("type"), TEXT("point"));
	Lamp->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 200.0));

	FMonolithActionResult First = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body({ Lamp })));
	TestTrue(FString::Printf(TEXT("first apply succeeds (%s)"), *First.ErrorMessage), First.bSuccess);
	if (!First.bSuccess) { return false; }

	AActor* OriginalLamp = nullptr;
	for (AActor* Actor : FMonolithMeshLayoutActions::FindLayoutActors(GetTestWorld(), LayoutId))
	{
		if (FMonolithMeshLayoutActions::GetEntryId(Actor) == TEXT("lamp")) { OriginalLamp = Actor; }
	}
	TestNotNull(TEXT("the lamp was placed"), OriginalLamp);

	// Now apply the full document in skip mode: the lamp must be left ALONE
	// (same actor object), only the missing entry is added.
	auto SkipParams = ApplyParams(LayoutId, MakeSimpleBody());
	SkipParams->SetStringField(TEXT("on_existing"), TEXT("skip"));
	FMonolithActionResult Second = Exec(TEXT("apply_level_layout"), SkipParams);
	TestTrue(FString::Printf(TEXT("skip-mode apply succeeds (%s)"), *Second.ErrorMessage), Second.bSuccess);
	if (!Second.bSuccess) { Remove(LayoutId); return false; }

	double Placed = 0, Skipped = 0, RemovedPrevious = -1;
	Second.Result->TryGetNumberField(TEXT("placed"), Placed);
	Second.Result->TryGetNumberField(TEXT("skipped"), Skipped);
	Second.Result->TryGetNumberField(TEXT("removed_previous"), RemovedPrevious);
	TestEqual(TEXT("skip mode places only the missing entry"), static_cast<int32>(Placed), 1);
	TestEqual(TEXT("skip mode reports the entry it skipped"), static_cast<int32>(Skipped), 1);
	TestEqual(TEXT("skip mode never removes anything"), static_cast<int32>(RemovedPrevious), 0);
	TestEqual(TEXT("the layout now has both entries"), CountInLevel(LayoutId), 2);

	bool bSameObject = false;
	for (AActor* Actor : FMonolithMeshLayoutActions::FindLayoutActors(GetTestWorld(), LayoutId))
	{
		if (Actor == OriginalLamp) { bSameObject = true; }
	}
	TestTrue(TEXT("skip mode left the ORIGINAL lamp actor untouched"), bSameObject);

	Remove(LayoutId);
	return true;
}

// ============================================================================
// 7. All-or-nothing: one bad entry places nothing.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutAllOrNothingTest,
	"Monolith.Mesh.Layouts.InvalidEntryPlacesNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutAllOrNothingTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;

	const FString LayoutId = TEXT("monolith_test_allornothing");
	Remove(LayoutId);

	TSharedPtr<FJsonObject> Good = Entry(TEXT("good"), TEXT("actor"));
	Good->SetStringField(TEXT("class"), TEXT("StaticMeshActor"));
	Good->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));

	TSharedPtr<FJsonObject> BadClass = Entry(TEXT("bad_class"), TEXT("actor"));
	BadClass->SetStringField(TEXT("class"), TEXT("NoSuchActorClassXyz"));
	BadClass->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 100.0));

	TSharedPtr<FJsonObject> BadPreset = Entry(TEXT("bad_preset"), TEXT("light"));
	BadPreset->SetStringField(TEXT("type"), TEXT("point"));
	BadPreset->SetStringField(TEXT("preset"), TEXT("no_such_preset_xyz"));
	BadPreset->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 200.0));

	FMonolithActionResult Result =
		Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body({ Good, BadClass, BadPreset })));

	TestFalse(TEXT("a layout with an unresolvable entry is refused"), Result.bSuccess);
	TestEqual(TEXT("NOTHING was placed — not even the valid entry"), CountInLevel(LayoutId), 0);
	TestTrue(TEXT("the error names the bad class entry"), Result.ErrorMessage.Contains(TEXT("bad_class")));
	TestTrue(TEXT("the error names the bad preset entry TOO (all problems, not just the first)"),
		Result.ErrorMessage.Contains(TEXT("bad_preset")));
	TestTrue(TEXT("the error says the level is unchanged"),
		Result.ErrorMessage.Contains(TEXT("NOTHING was placed")));

	// The same document must not disturb an already-applied good instance either.
	FMonolithActionResult GoodApply = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body({ Good })));
	TestTrue(FString::Printf(TEXT("the good-only layout applies (%s)"), *GoodApply.ErrorMessage),
		GoodApply.bSuccess);
	TestEqual(TEXT("one actor placed"), CountInLevel(LayoutId), 1);

	FMonolithActionResult BadAgain =
		Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body({ Good, BadClass })));
	TestFalse(TEXT("the broken document is refused again"), BadAgain.bSuccess);
	TestEqual(TEXT("the previously applied instance survived the failed apply"), CountInLevel(LayoutId), 1);

	Remove(LayoutId);
	return true;
}

// ============================================================================
// 8. Validation contract — ids, kinds, fields, vectors.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutValidationTest,
	"Monolith.Mesh.Layouts.ValidationContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutValidationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;

	const FString LayoutId = TEXT("monolith_test_validation");
	Remove(LayoutId);

	auto Apply = [&](const TSharedPtr<FJsonObject>& InBody)
	{
		return Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, InBody));
	};

	// Duplicate entry ids — the identity mechanism would be ambiguous.
	{
		TSharedPtr<FJsonObject> A = Entry(TEXT("dup"), TEXT("actor"));
		A->SetStringField(TEXT("class"), TEXT("StaticMeshActor"));
		A->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));
		TSharedPtr<FJsonObject> B = Entry(TEXT("dup"), TEXT("actor"));
		B->SetStringField(TEXT("class"), TEXT("StaticMeshActor"));
		B->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 50.0));

		FMonolithActionResult R = Apply(Body({ A, B }));
		TestFalse(TEXT("duplicate entry ids are refused"), R.bSuccess);
		TestTrue(TEXT("duplicate-id error explains why ids must be unique"),
			R.ErrorMessage.Contains(TEXT("duplicate id")));
	}

	// Missing id.
	{
		auto NoId = MakeShared<FJsonObject>();
		NoId->SetStringField(TEXT("kind"), TEXT("actor"));
		NoId->SetStringField(TEXT("class"), TEXT("StaticMeshActor"));
		NoId->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));
		FMonolithActionResult R = Apply(Body({ NoId }));
		TestFalse(TEXT("an entry with no id is refused"), R.bSuccess);
		TestTrue(TEXT("the missing-id error explains what ids are for"),
			R.ErrorMessage.Contains(TEXT("id")));
	}

	// Unknown kind.
	{
		TSharedPtr<FJsonObject> E = Entry(TEXT("weird"), TEXT("teleporter"));
		FMonolithActionResult R = Apply(Body({ E }));
		TestFalse(TEXT("an unknown kind is refused"), R.bSuccess);
		TestTrue(TEXT("the unknown-kind error lists the valid kinds"),
			R.ErrorMessage.Contains(TEXT("atmosphere")));
	}

	// Unknown field, validated against the TARGET ACTION's real schema.
	{
		TSharedPtr<FJsonObject> E = Entry(TEXT("typo"), TEXT("light"));
		E->SetStringField(TEXT("type"), TEXT("point"));
		E->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));
		E->SetNumberField(TEXT("inensity"), 1000.0);
		FMonolithActionResult R = Apply(Body({ E }));
		TestFalse(TEXT("an unknown entry field is refused"), R.bSuccess);
		TestTrue(TEXT("the unknown-field error names the field"),
			R.ErrorMessage.Contains(TEXT("inensity")));
		TestTrue(TEXT("the unknown-field error lists what the target action does accept"),
			R.ErrorMessage.Contains(TEXT("attenuation_radius")));
	}

	// Missing required field of the target action.
	{
		TSharedPtr<FJsonObject> E = Entry(TEXT("nowhere"), TEXT("actor"));
		E->SetStringField(TEXT("class"), TEXT("StaticMeshActor"));
		FMonolithActionResult R = Apply(Body({ E }));
		TestFalse(TEXT("an entry missing a required field is refused"), R.bSuccess);
		TestTrue(TEXT("the error names the missing field"), R.ErrorMessage.Contains(TEXT("location")));
	}

	// Malformed vector.
	{
		TSharedPtr<FJsonObject> E = Entry(TEXT("badvec"), TEXT("actor"));
		E->SetStringField(TEXT("class"), TEXT("StaticMeshActor"));
		E->SetStringField(TEXT("location"), TEXT("over there"));
		FMonolithActionResult R = Apply(Body({ E }));
		TestFalse(TEXT("a malformed location is refused"), R.bSuccess);
		TestTrue(TEXT("the malformed-vector error says the expected shape"),
			R.ErrorMessage.Contains(TEXT("[x, y, z]")));
	}

	// Preset targeting the wrong light type.
	{
		TSharedPtr<FJsonObject> E = Entry(TEXT("mismatch"), TEXT("light"));
		E->SetStringField(TEXT("type"), TEXT("point"));
		E->SetStringField(TEXT("preset"), TEXT("sun_midday"));
		E->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));
		FMonolithActionResult R = Apply(Body({ E }));
		TestFalse(TEXT("a preset that targets another light type is refused"), R.bSuccess);
		TestTrue(TEXT("the mismatch error names both types"),
			R.ErrorMessage.Contains(TEXT("sun_midday")));
	}

	// Empty entries array.
	{
		auto EmptyBody = MakeShared<FJsonObject>();
		EmptyBody->SetArrayField(TEXT("entries"), TArray<TSharedPtr<FJsonValue>>());
		FMonolithActionResult R = Apply(EmptyBody);
		TestFalse(TEXT("an empty layout is refused"), R.bSuccess);
	}

	// Neither layout nor layout_json.
	{
		FMonolithActionResult R = Exec(TEXT("apply_level_layout"), MakeShared<FJsonObject>());
		TestFalse(TEXT("apply with no layout at all is refused"), R.bSuccess);
		TestTrue(TEXT("the error explains both ways to name a layout"),
			R.ErrorMessage.Contains(TEXT("layout_json")));
	}

	TestEqual(TEXT("no validation failure placed anything"), CountInLevel(LayoutId), 0);
	Remove(LayoutId);
	return true;
}

// ============================================================================
// 9. Removal is scoped to one layout.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutRemoveTest,
	"Monolith.Mesh.Layouts.RemoveIsScopedToOneLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutRemoveTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;

	const FString LayoutA = TEXT("monolith_test_remove_a");
	const FString LayoutB = TEXT("monolith_test_remove_b");
	Remove(LayoutA);
	Remove(LayoutB);

	TestTrue(TEXT("layout A applies"),
		Exec(TEXT("apply_level_layout"), ApplyParams(LayoutA, MakeSimpleBody())).bSuccess);
	TestTrue(TEXT("layout B applies"),
		Exec(TEXT("apply_level_layout"), ApplyParams(LayoutB, MakeSimpleBody())).bSuccess);
	TestEqual(TEXT("layout A has 2 actors"), CountInLevel(LayoutA), 2);
	TestEqual(TEXT("layout B has 2 actors"), CountInLevel(LayoutB), 2);

	// Dry run reports without deleting.
	auto DryParams = MakeShared<FJsonObject>();
	DryParams->SetStringField(TEXT("layout"), LayoutA);
	DryParams->SetBoolField(TEXT("dry_run"), true);
	FMonolithActionResult Dry = Exec(TEXT("remove_level_layout"), DryParams);
	TestTrue(TEXT("remove dry_run succeeds"), Dry.bSuccess);
	if (Dry.bSuccess)
	{
		double Found = 0, RemovedCount = -1;
		Dry.Result->TryGetNumberField(TEXT("found"), Found);
		Dry.Result->TryGetNumberField(TEXT("removed"), RemovedCount);
		TestEqual(TEXT("dry run finds the actors"), static_cast<int32>(Found), 2);
		TestEqual(TEXT("dry run removes nothing"), static_cast<int32>(RemovedCount), 0);
	}
	TestEqual(TEXT("dry run left layout A alone"), CountInLevel(LayoutA), 2);

	FMonolithActionResult RemoveA = Remove(LayoutA);
	TestTrue(TEXT("remove succeeds"), RemoveA.bSuccess);
	TestEqual(TEXT("layout A is gone"), CountInLevel(LayoutA), 0);
	TestEqual(TEXT("layout B is untouched"), CountInLevel(LayoutB), 2);

	// Removing an unknown layout is a clean no-op, not an error.
	FMonolithActionResult RemoveNothing = Remove(TEXT("monolith_test_never_applied"));
	TestTrue(TEXT("removing a layout that was never applied succeeds"), RemoveNothing.bSuccess);
	if (RemoveNothing.bSuccess)
	{
		double Found = -1;
		RemoveNothing.Result->TryGetNumberField(TEXT("found"), Found);
		TestEqual(TEXT("and reports finding nothing"), static_cast<int32>(Found), 0);
	}

	Remove(LayoutB);
	TestEqual(TEXT("cleanup removed layout B"), CountInLevel(LayoutB), 0);
	return true;
}

// ============================================================================
// 10. dry_run touches nothing.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutDryRunTest,
	"Monolith.Mesh.Layouts.DryRunTouchesNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutDryRunTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;

	const FString LayoutId = TEXT("monolith_test_dryrun");
	Remove(LayoutId);

	auto Params = ApplyParams(LayoutId, MakeSimpleBody());
	Params->SetBoolField(TEXT("dry_run"), true);

	FMonolithActionResult Result = Exec(TEXT("apply_level_layout"), Params);
	TestTrue(FString::Printf(TEXT("dry run succeeds (%s)"), *Result.ErrorMessage), Result.bSuccess);
	if (!Result.bSuccess) { return false; }

	double WouldPlace = 0;
	Result.Result->TryGetNumberField(TEXT("would_place"), WouldPlace);
	TestEqual(TEXT("dry run reports what it would place"), static_cast<int32>(WouldPlace), 2);
	TestEqual(TEXT("dry run placed nothing"), CountInLevel(LayoutId), 0);

	// A dry run of a BROKEN layout still fails — validation is the point of it.
	TSharedPtr<FJsonObject> Bad = Entry(TEXT("bad"), TEXT("actor"));
	Bad->SetStringField(TEXT("class"), TEXT("NoSuchActorClassXyz"));
	Bad->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));
	auto BadParams = ApplyParams(LayoutId, Body({ Bad }));
	BadParams->SetBoolField(TEXT("dry_run"), true);
	TestFalse(TEXT("a dry run of a broken layout still reports the failure"),
		Exec(TEXT("apply_level_layout"), BadParams).bSuccess);

	return true;
}

// ============================================================================
// 11. Scale — measure a large layout.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutLargeTest,
	"Monolith.Mesh.Layouts.LargeLayoutTiming",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutLargeTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;

	const FString LayoutId = TEXT("monolith_test_large");
	const int32 Count = 150;
	Remove(LayoutId);

	TArray<TSharedPtr<FJsonObject>> Entries;
	Entries.Reserve(Count);
	for (int32 i = 0; i < Count; ++i)
	{
		TSharedPtr<FJsonObject> E = Entry(FString::Printf(TEXT("prop_%03d"), i), TEXT("actor"));
		E->SetStringField(TEXT("class"), TEXT("StaticMeshActor"));
		E->SetArrayField(TEXT("location"), Vec((i % 20) * 200.0, (i / 20) * 200.0, 0.0));
		Entries.Add(E);
	}

	const double Start = FPlatformTime::Seconds();
	FMonolithActionResult Result = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body(Entries)));
	const double WallMs = (FPlatformTime::Seconds() - Start) * 1000.0;

	TestTrue(FString::Printf(TEXT("a %d-entry layout applies (%s)"), Count, *Result.ErrorMessage),
		Result.bSuccess);
	if (!Result.bSuccess) { Remove(LayoutId); return false; }

	double ReportedMs = 0;
	Result.Result->TryGetNumberField(TEXT("elapsed_ms"), ReportedMs);
	AddInfo(FString::Printf(
		TEXT("SCALE: applied %d entries in %.1f ms wall (%.1f ms reported by the action, %.3f ms/entry). "
			 "This is the measurement behind keeping apply synchronous rather than routing it through "
			 "FMonolithJobManager."),
		Count, WallMs, ReportedMs, WallMs / static_cast<double>(Count)));

	TestEqual(TEXT("every entry was placed"), CountInLevel(LayoutId), Count);

	// And re-applying that many still does not duplicate.
	const double ReapplyStart = FPlatformTime::Seconds();
	FMonolithActionResult Again = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body(Entries)));
	const double ReapplyMs = (FPlatformTime::Seconds() - ReapplyStart) * 1000.0;
	TestTrue(TEXT("re-applying a large layout succeeds"), Again.bSuccess);
	AddInfo(FString::Printf(TEXT("SCALE: re-apply (place %d + retire %d) took %.1f ms wall."),
		Count, Count, ReapplyMs));
	TestEqual(TEXT("re-applying a large layout does not duplicate"), CountInLevel(LayoutId), Count);

	Remove(LayoutId);
	TestEqual(TEXT("cleanup removed the large layout"), CountInLevel(LayoutId), 0);
	return true;
}

// ============================================================================
// 12. save_level_layout round trip.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutSaveTest,
	"Monolith.Mesh.Layouts.SaveRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutSaveTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;

	const FString LayoutId = TEXT("monolith_test_saved");
	const FString FilePath = FMonolithMeshLayoutActions::Library().GetUserDirectory() / (LayoutId + TEXT(".json"));
	IFileManager::Get().Delete(*FilePath, false, true, true);

	// A broken document is never written.
	{
		TSharedPtr<FJsonObject> Bad = Entry(TEXT("bad"), TEXT("actor"));
		Bad->SetStringField(TEXT("class"), TEXT("NoSuchActorClassXyz"));
		Bad->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));

		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("layout"), LayoutId);
		P->SetObjectField(TEXT("layout_json"), Body({ Bad }));
		FMonolithActionResult R = Exec(TEXT("save_level_layout"), P);
		TestFalse(TEXT("an invalid layout is not saved"), R.bSuccess);
		TestFalse(TEXT("no file was written for the invalid layout"),
			IFileManager::Get().FileExists(*FilePath));
	}

	// A valid document is written and becomes a named layout.
	{
		TSharedPtr<FJsonObject> B = MakeSimpleBody();
		B->SetStringField(TEXT("description"), TEXT("automation round-trip layout"));

		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("layout"), LayoutId);
		P->SetObjectField(TEXT("layout_json"), B);
		FMonolithActionResult R = Exec(TEXT("save_level_layout"), P);
		TestTrue(FString::Printf(TEXT("a valid layout saves (%s)"), *R.ErrorMessage), R.bSuccess);
		TestTrue(TEXT("the file exists on disk"), IFileManager::Get().FileExists(*FilePath));

		// It is now loadable BY NAME — no layout_json needed.
		auto DescribeParams = MakeShared<FJsonObject>();
		DescribeParams->SetStringField(TEXT("layout"), LayoutId);
		FMonolithActionResult Describe = Exec(TEXT("describe_level_layout"), DescribeParams);
		TestTrue(FString::Printf(TEXT("the saved layout describes by name (%s)"), *Describe.ErrorMessage),
			Describe.bSuccess);
		if (Describe.bSuccess)
		{
			bool bValid = false;
			Describe.Result->TryGetBoolField(TEXT("valid"), bValid);
			TestTrue(TEXT("the saved layout is valid"), bValid);
			FString Description;
			Describe.Result->TryGetStringField(TEXT("description"), Description);
			TestEqual(TEXT("the description round-tripped"), Description,
				FString(TEXT("automation round-trip layout")));
		}

		// Saving again without overwrite is refused.
		FMonolithActionResult Again = Exec(TEXT("save_level_layout"), P);
		TestFalse(TEXT("saving over an existing layout without overwrite is refused"), Again.bSuccess);
		P->SetBoolField(TEXT("overwrite"), true);
		TestTrue(TEXT("overwrite=true allows the save"), Exec(TEXT("save_level_layout"), P).bSuccess);
	}

	IFileManager::Get().Delete(*FilePath, false, true, true);
	TestFalse(TEXT("cleanup removed the layout file"), IFileManager::Get().FileExists(*FilePath));
	return true;
}

// ============================================================================
// 13. mesh.spawn_actor's property channel, called DIRECTLY.
//
// Lives in this file because the channel exists for the layout system: `kind:
// "actor"` could not set arbitrary properties, and the fix had to be upstream in
// spawn_actor. Tested here at the source before test 14 tests it through a layout.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshSpawnActorPropertyChannelTest,
	"Monolith.Mesh.Scene.SpawnActorPropertyChannel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshSpawnActorPropertyChannelTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;

	UWorld* World = GetTestWorld();
	if (!World)
	{
		AddWarning(TEXT("no editor world — skipping"));
		return true;
	}

	auto CountActorsLabelled = [World](const FString& Label)
	{
		int32 Count = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (IsValid(*It) && It->GetActorLabel() == Label) { ++Count; }
		}
		return Count;
	};

	// --- 1. Valid bags land on the right two objects. ---
	const FString GoodLabel = TEXT("monolith_test_props_ok");
	{
		auto Props = MakeShared<FJsonObject>();
		Props->SetBoolField(TEXT("bEnableAutoLODGeneration"), false);

		auto CompProps = MakeShared<FJsonObject>();
		CompProps->SetStringField(TEXT("Mobility"), TEXT("Movable"));
		CompProps->SetBoolField(TEXT("bReceivesDecals"), false);

		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("class_or_mesh"), TEXT("StaticMeshActor"));
		P->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));
		P->SetStringField(TEXT("name"), GoodLabel);
		P->SetObjectField(TEXT("properties"), Props);
		P->SetObjectField(TEXT("component_properties"), CompProps);

		FMonolithActionResult R = Exec(TEXT("spawn_actor"), P);
		TestTrue(FString::Printf(TEXT("spawn_actor with valid property bags succeeds (%s)"), *R.ErrorMessage),
			R.bSuccess);

		if (R.bSuccess)
		{
			const TArray<TSharedPtr<FJsonValue>>* ActorSet = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* CompSet = nullptr;
			TestTrue(TEXT("the result reports which ACTOR properties were set"),
				R.Result->TryGetArrayField(TEXT("properties_set"), ActorSet) && ActorSet && ActorSet->Num() == 1);
			TestTrue(TEXT("the result reports which COMPONENT properties were set"),
				R.Result->TryGetArrayField(TEXT("component_properties_set"), CompSet) && CompSet && CompSet->Num() == 2);

			AActor* Spawned = nullptr;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (IsValid(*It) && It->GetActorLabel() == GoodLabel) { Spawned = *It; }
			}
			TestNotNull(TEXT("the actor is in the level"), Spawned);
			if (Spawned)
			{
				TestFalse(TEXT("the ACTOR bag really wrote onto the actor (bEnableAutoLODGeneration)"),
					Spawned->bEnableAutoLODGeneration);

				USceneComponent* Root = Spawned->GetRootComponent();
				TestNotNull(TEXT("the actor has a root component"), Root);
				if (Root)
				{
					TestEqual(TEXT("the COMPONENT bag really wrote onto the root component (Mobility)"),
						static_cast<int32>(Root->Mobility.GetValue()),
						static_cast<int32>(EComponentMobility::Movable));
					if (UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Root))
					{
						TestFalse(TEXT("and the second component key landed too (bReceivesDecals)"),
							Prim->bReceivesDecals);
					}
				}
				World->EditorDestroyActor(Spawned, true);
			}
		}
	}

	// --- 2. An unknown key on the ACTOR bag: error, did-you-mean, nothing spawned. ---
	const FString BadLabel = TEXT("monolith_test_props_bad");
	{
		auto Props = MakeShared<FJsonObject>();
		Props->SetBoolField(TEXT("bEnableAutoLODGenaration"), false); // typo

		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("class_or_mesh"), TEXT("StaticMeshActor"));
		P->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));
		P->SetStringField(TEXT("name"), BadLabel);
		P->SetObjectField(TEXT("properties"), Props);

		FMonolithActionResult R = Exec(TEXT("spawn_actor"), P);
		TestFalse(TEXT("an unknown ACTOR property name is refused"), R.bSuccess);
		TestTrue(TEXT("the error names the offending key"),
			R.ErrorMessage.Contains(TEXT("bEnableAutoLODGenaration")));
		TestTrue(TEXT("the error carries a did-you-mean candidate"),
			R.ErrorMessage.Contains(TEXT("did you mean")) &&
			R.ErrorMessage.Contains(TEXT("bEnableAutoLODGeneration")));
		TestEqual(TEXT("NOTHING was left behind by the refused spawn"), CountActorsLabelled(BadLabel), 0);
	}

	// --- 3. Same for the COMPONENT bag — and the rollback really is a rollback:
	//        the actor bag is fine, so the actor DOES get spawned before the
	//        component bag is rejected, and it must be destroyed again. ---
	{
		auto Props = MakeShared<FJsonObject>();
		Props->SetBoolField(TEXT("bEnableAutoLODGeneration"), false);

		auto CompProps = MakeShared<FJsonObject>();
		CompProps->SetStringField(TEXT("Mobilty"), TEXT("Movable")); // typo

		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("class_or_mesh"), TEXT("StaticMeshActor"));
		P->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));
		P->SetStringField(TEXT("name"), BadLabel);
		P->SetObjectField(TEXT("properties"), Props);
		P->SetObjectField(TEXT("component_properties"), CompProps);

		FMonolithActionResult R = Exec(TEXT("spawn_actor"), P);
		TestFalse(TEXT("an unknown COMPONENT property name is refused"), R.bSuccess);
		TestTrue(TEXT("the component error names the offending key"), R.ErrorMessage.Contains(TEXT("Mobilty")));
		TestTrue(TEXT("the component error suggests Mobility"), R.ErrorMessage.Contains(TEXT("Mobility")));
		TestEqual(TEXT("nothing was left behind by the refused component write either"),
			CountActorsLabelled(BadLabel), 0);
	}

	// --- 4. A non-object bag is a clean parameter error. ---
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("class_or_mesh"), TEXT("StaticMeshActor"));
		P->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));
		P->SetStringField(TEXT("name"), BadLabel);
		P->SetStringField(TEXT("properties"), TEXT("Mobility=Movable"));

		FMonolithActionResult R = Exec(TEXT("spawn_actor"), P);
		TestFalse(TEXT("a non-object `properties` is refused"), R.bSuccess);
		TestTrue(TEXT("the error says what shape is expected"), R.ErrorMessage.Contains(TEXT("JSON object")));
		TestEqual(TEXT("and nothing spawned"), CountActorsLabelled(BadLabel), 0);
	}

	return true;
}

// ============================================================================
// 14. The two gaps closed THROUGH A LAYOUT: kind "actor" + properties, and
//     the new kind "volume".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutActorPropertiesTest,
	"Monolith.Mesh.Layouts.ActorPropertiesThroughLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutActorPropertiesTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;

	const FString LayoutId = TEXT("monolith_test_actor_props");
	Remove(LayoutId);

	auto MakeEntry = [](const TCHAR* CompKey)
	{
		TSharedPtr<FJsonObject> E = Entry(TEXT("pedestal"), TEXT("actor"));
		E->SetStringField(TEXT("class"), TEXT("StaticMeshActor"));
		E->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));

		auto Props = MakeShared<FJsonObject>();
		Props->SetBoolField(TEXT("bEnableAutoLODGeneration"), false);
		E->SetObjectField(TEXT("properties"), Props);

		auto CompProps = MakeShared<FJsonObject>();
		CompProps->SetStringField(CompKey, TEXT("Movable"));
		CompProps->SetBoolField(TEXT("bReceivesDecals"), false);
		E->SetObjectField(TEXT("component_properties"), CompProps);
		return E;
	};

	// --- The good document: both bags travel through the layout untouched. ---
	FMonolithActionResult R =
		Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body({ MakeEntry(TEXT("Mobility")) })));
	TestTrue(FString::Printf(TEXT("a layout entry with property bags applies (%s)"), *R.ErrorMessage), R.bSuccess);
	if (!R.bSuccess) { Remove(LayoutId); return false; }

	TestEqual(TEXT("the entry was placed"), CountInLevel(LayoutId), 1);

	for (AActor* Actor : FMonolithMeshLayoutActions::FindLayoutActors(GetTestWorld(), LayoutId))
	{
		TestFalse(TEXT("the layout's `properties` bag reached the ACTOR"), Actor->bEnableAutoLODGeneration);

		USceneComponent* Root = Actor->GetRootComponent();
		TestNotNull(TEXT("the placed actor has a root component"), Root);
		if (Root)
		{
			TestEqual(TEXT("the layout's `component_properties` bag reached the ROOT COMPONENT"),
				static_cast<int32>(Root->Mobility.GetValue()), static_cast<int32>(EComponentMobility::Movable));
		}

		// The layout tag must survive a bag that rewrites actor-level state.
		TestFalse(TEXT("the actor still carries its entry tag"),
			FMonolithMeshLayoutActions::GetEntryId(Actor).IsEmpty());
	}

	Remove(LayoutId);
	TestEqual(TEXT("cleanup removed the layout"), CountInLevel(LayoutId), 0);

	// --- Phase 1 catches a bad property key with ZERO world mutation. ---
	{
		FMonolithActionResult Bad =
			Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body({ MakeEntry(TEXT("Mobilty")) })));
		TestFalse(TEXT("a mistyped component property fails the apply"), Bad.bSuccess);
		TestTrue(TEXT("the error names the entry"), Bad.ErrorMessage.Contains(TEXT("pedestal")));
		TestTrue(TEXT("the error names the mistyped key"), Bad.ErrorMessage.Contains(TEXT("Mobilty")));
		TestTrue(TEXT("the error carries a did-you-mean"), Bad.ErrorMessage.Contains(TEXT("did you mean")));
		TestTrue(TEXT("the failure is reported as a PHASE 1 refusal (nothing placed)"),
			Bad.ErrorMessage.Contains(TEXT("NOTHING was placed")));
		TestEqual(TEXT("no actor was created by the refused apply"), CountInLevel(LayoutId), 0);
	}

	// --- Same for a bad ACTOR-bag key, and describe reports it without touching anything. ---
	{
		TSharedPtr<FJsonObject> E = Entry(TEXT("pedestal"), TEXT("actor"));
		E->SetStringField(TEXT("class"), TEXT("StaticMeshActor"));
		E->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));
		auto Props = MakeShared<FJsonObject>();
		Props->SetBoolField(TEXT("bEnableAutoLODGenaration"), false);
		E->SetObjectField(TEXT("properties"), Props);

		auto DescribeParams = ApplyParams(LayoutId, Body({ E }));
		FMonolithActionResult Describe = Exec(TEXT("describe_level_layout"), DescribeParams);
		TestTrue(TEXT("describe still succeeds on a broken document"), Describe.bSuccess);
		if (Describe.bSuccess)
		{
			bool bValid = true;
			Describe.Result->TryGetBoolField(TEXT("valid"), bValid);
			TestFalse(TEXT("describe reports the document as invalid"), bValid);

			const TArray<TSharedPtr<FJsonValue>>* Problems = nullptr;
			Describe.Result->TryGetArrayField(TEXT("problems"), Problems);
			const FString Joined = Problems && Problems->Num() > 0 ? (*Problems)[0]->AsString() : FString();
			TestTrue(TEXT("the dry run names the bad actor property"),
				Joined.Contains(TEXT("bEnableAutoLODGenaration")));
		}
		TestEqual(TEXT("describe placed nothing"), CountInLevel(LayoutId), 0);
	}

	Remove(LayoutId);
	return true;
}

// ============================================================================
// 15. kind "volume".
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutVolumeKindTest,
	"Monolith.Mesh.Layouts.VolumeKind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutVolumeKindTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;

	// The kind table must name the real action, and it must be registered.
	TestNotNull(TEXT("the 'volume' kind exists"), FMonolithMeshLayoutActions::FindKind(TEXT("volume")));
	if (const FMonolithMeshLayoutActions::FKind* Kind = FMonolithMeshLayoutActions::FindKind(TEXT("volume")))
	{
		TestEqual(TEXT("the volume kind places through mesh.spawn_volume"), Kind->Action, FString(TEXT("spawn_volume")));
	}

	const FString LayoutId = TEXT("monolith_test_volume");
	Remove(LayoutId);

	// --- A trigger volume + a post-process volume with a honoured property key. ---
	TSharedPtr<FJsonObject> Trigger = Entry(TEXT("doorway"), TEXT("volume"));
	Trigger->SetStringField(TEXT("type"), TEXT("trigger"));
	Trigger->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 100.0));
	Trigger->SetArrayField(TEXT("extent"), Vec(60.0, 200.0, 110.0));

	TSharedPtr<FJsonObject> Pain = Entry(TEXT("lava"), TEXT("volume"));
	Pain->SetStringField(TEXT("type"), TEXT("pain"));
	Pain->SetArrayField(TEXT("location"), Vec(400.0, 0.0, 0.0));
	{
		auto Props = MakeShared<FJsonObject>();
		Props->SetNumberField(TEXT("damage_per_sec"), 25.0);
		Pain->SetObjectField(TEXT("properties"), Props);
	}

	FMonolithActionResult R = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body({ Trigger, Pain })));
	TestTrue(FString::Printf(TEXT("a layout with volume entries applies (%s)"), *R.ErrorMessage), R.bSuccess);
	if (!R.bSuccess) { Remove(LayoutId); return false; }

	TestEqual(TEXT("both volumes were placed and tagged"), CountInLevel(LayoutId), 2);

	bool bFoundTrigger = false;
	for (AActor* Actor : FMonolithMeshLayoutActions::FindLayoutActors(GetTestWorld(), LayoutId))
	{
		if (FMonolithMeshLayoutActions::GetEntryId(Actor) == TEXT("doorway"))
		{
			bFoundTrigger = true;
			TestTrue(TEXT("the 'trigger' type really spawned an ATriggerVolume"),
				Actor->IsA(ATriggerVolume::StaticClass()));
			TestEqual(TEXT("the volume is labelled from the layout"),
				Actor->GetActorLabel(), FString::Printf(TEXT("%s.doorway"), *LayoutId));
		}
	}
	TestTrue(TEXT("the trigger entry is present"), bFoundTrigger);

	Remove(LayoutId);
	TestEqual(TEXT("cleanup removed the volumes"), CountInLevel(LayoutId), 0);

	// --- An unknown volume type is refused in phase 1. ---
	{
		TSharedPtr<FJsonObject> E = Entry(TEXT("nope"), TEXT("volume"));
		E->SetStringField(TEXT("type"), TEXT("teleport"));
		E->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));
		FMonolithActionResult Bad = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body({ E })));
		TestFalse(TEXT("an unknown volume type is refused"), Bad.bSuccess);
		TestTrue(TEXT("the error lists the valid volume types"),
			Bad.ErrorMessage.Contains(TEXT("nav_modifier")));
		TestEqual(TEXT("nothing placed"), CountInLevel(LayoutId), 0);
	}

	// --- A property key the volume type does not honour is refused in phase 1,
	//     rather than being silently dropped (which would make the layout lie). ---
	{
		TSharedPtr<FJsonObject> E = Entry(TEXT("doorway"), TEXT("volume"));
		E->SetStringField(TEXT("type"), TEXT("trigger"));
		E->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));
		auto Props = MakeShared<FJsonObject>();
		Props->SetNumberField(TEXT("damage_per_sec"), 10.0);
		E->SetObjectField(TEXT("properties"), Props);

		FMonolithActionResult Bad = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body({ E })));
		TestFalse(TEXT("a property key this volume type does not honour is refused"), Bad.bSuccess);
		TestTrue(TEXT("the error names the key"), Bad.ErrorMessage.Contains(TEXT("damage_per_sec")));
		TestEqual(TEXT("nothing placed"), CountInLevel(LayoutId), 0);
	}

	// --- And the same check guards mesh.spawn_volume called directly. ---
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), TEXT("trigger"));
		P->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));
		auto Props = MakeShared<FJsonObject>();
		Props->SetNumberField(TEXT("blend_radius"), 100.0);
		P->SetObjectField(TEXT("properties"), Props);

		FMonolithActionResult Bad = Exec(TEXT("spawn_volume"), P);
		TestFalse(TEXT("spawn_volume refuses an unhonoured properties key"), Bad.bSuccess);
		TestTrue(TEXT("the error says nothing was spawned"), Bad.ErrorMessage.Contains(TEXT("Nothing was spawned")));
	}

	Remove(LayoutId);
	return true;
}

// ============================================================================
// 13b. `kind: "atmosphere"` + `type: "volumetric_cloud"` — the newest atmosphere
//      type through a layout, both directions.
//
// The layout system forwards every non-`kind` key straight to the target action,
// so a new atmosphere type is SUPPOSED to work here for free. "Supposed to" is not
// evidence: this test applies one, checks a real AVolumetricCloud was placed and
// tagged, and captures it back to make sure the classifier (TokenForActor) names
// the cloud rather than dropping it into `skipped`.
//
// Not proven: that anything was drawn. `-nullrhi`.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutVolumetricCloudTest,
	"Monolith.Mesh.Layouts.VolumetricCloudEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutVolumetricCloudTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;
	EnsureRegistered();

	// Pick a shipped cloud preset out of the DATA rather than naming one in C++.
	TArray<FString> Warnings;
	const TMap<FString, FMonolithJsonPreset> Presets =
		FMonolithMeshAtmosphereActions::Presets().LoadPresets(Warnings);
	FString CloudPreset;
	double PresetLayerHeight = 0.0;
	for (const TPair<FString, FMonolithJsonPreset>& Pair : Presets)
	{
		if (Pair.Value.TypeToken == TEXT("volumetric_cloud") && Pair.Value.Properties.IsValid()
			&& Pair.Value.Properties->HasField(TEXT("LayerHeight")))
		{
			CloudPreset = Pair.Value.Name;
			PresetLayerHeight = Pair.Value.Properties->GetNumberField(TEXT("LayerHeight"));
			break;
		}
	}
	if (!TestTrue(TEXT("a shipped volumetric_cloud preset with a LayerHeight exists"), !CloudPreset.IsEmpty()))
	{
		return false;
	}

	const FString LayoutId = TEXT("monolith_test_cloud");
	Remove(LayoutId);

	// One cloud from a preset, one from raw properties, plus the sun a cloud needs
	// to be lit at all — the smallest layout that is a real sky.
	TSharedPtr<FJsonObject> Sun = Entry(TEXT("sun"), TEXT("light"));
	Sun->SetStringField(TEXT("type"), TEXT("directional"));
	Sun->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 2000.0));
	Sun->SetArrayField(TEXT("rotation"), Vec(-35.0, 150.0, 0.0));

	TSharedPtr<FJsonObject> Sky = Entry(TEXT("sky"), TEXT("atmosphere"));
	Sky->SetStringField(TEXT("type"), TEXT("sky_atmosphere"));

	TSharedPtr<FJsonObject> PresetCloud = Entry(TEXT("weather"), TEXT("atmosphere"));
	PresetCloud->SetStringField(TEXT("type"), TEXT("volumetric_cloud"));
	PresetCloud->SetStringField(TEXT("preset"), CloudPreset);

	TSharedPtr<FJsonObject> RawCloud = Entry(TEXT("high_cirrus"), TEXT("atmosphere"));
	RawCloud->SetStringField(TEXT("type"), TEXT("cloud"));   // alias, straight through
	{
		auto Props = MakeShared<FJsonObject>();
		Props->SetNumberField(TEXT("LayerBottomAltitude"), 7.75);
		Props->SetNumberField(TEXT("LayerHeight"), 1.5);
		RawCloud->SetObjectField(TEXT("properties"), Props);
	}

	FMonolithActionResult R =
		Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body({ Sun, Sky, PresetCloud, RawCloud })));
	TestTrue(FString::Printf(TEXT("a layout with volumetric_cloud entries applies (%s)"), *R.ErrorMessage),
		R.bSuccess);
	if (!R.bSuccess) { Remove(LayoutId); return false; }

	TestEqual(TEXT("every entry was placed and tagged"), CountInLevel(LayoutId), 4);

	AActor* PresetActor = ActorForEntry(LayoutId, TEXT("weather"));
	if (TestNotNull(TEXT("the preset cloud entry placed an actor"), PresetActor))
	{
		TestTrue(TEXT("the entry really placed an AVolumetricCloud"),
			PresetActor->IsA(AVolumetricCloud::StaticClass()));
		TestEqual(TEXT("the placed cloud classifies back as volumetric_cloud"),
			FMonolithMeshAtmosphereActions::TokenForActor(PresetActor), FString(TEXT("volumetric_cloud")));
		if (UVolumetricCloudComponent* Comp = PresetActor->FindComponentByClass<UVolumetricCloudComponent>())
		{
			TestEqual(TEXT("the preset's LayerHeight reached the component through the layout"),
				static_cast<double>(Comp->LayerHeight), PresetLayerHeight, 0.0001);
		}
	}

	AActor* RawActor = ActorForEntry(LayoutId, TEXT("high_cirrus"));
	if (TestNotNull(TEXT("the alias-typed cloud entry placed an actor"), RawActor))
	{
		TestTrue(TEXT("the 'cloud' alias works inside a layout"),
			RawActor->IsA(AVolumetricCloud::StaticClass()));
		if (UVolumetricCloudComponent* Comp = RawActor->FindComponentByClass<UVolumetricCloudComponent>())
		{
			TestEqual(TEXT("a raw properties bag reached the cloud component"),
				Comp->LayerBottomAltitude, 7.75f, 0.0001f);
		}
	}

	// --- and the reverse direction: capture must not drop the clouds ------
	{
		FMonolithActionResult Cap = Exec(TEXT("capture_level_layout"), CaptureParams(LayoutId));
		if (TestTrue(FString::Printf(TEXT("capture_level_layout succeeded (%s)"), *Cap.ErrorMessage), Cap.bSuccess))
		{
			double SkippedCount = -1.0;
			Cap.Result->TryGetNumberField(TEXT("skipped_count"), SkippedCount);
			TestEqual(TEXT("no actor was skipped by the capture"), static_cast<int32>(SkippedCount), 0);

			const TSharedPtr<FJsonObject> Doc = BodyOf(Cap);
			const TSharedPtr<FJsonObject> Captured = EntryById(Doc, TEXT("high_cirrus"));
			if (TestTrue(TEXT("the cloud entry came back in the captured document"), Captured.IsValid()))
			{
				TestEqual(TEXT("it captured as an atmosphere entry"),
					Captured->GetStringField(TEXT("kind")), FString(TEXT("atmosphere")));
				TestEqual(TEXT("it captured as the CANONICAL type, not the alias"),
					Captured->GetStringField(TEXT("type")), FString(TEXT("volumetric_cloud")));
				const TSharedPtr<FJsonObject> Bag = BagOf(Captured, TEXT("properties"));
				if (TestTrue(TEXT("the non-default cloud values were captured"), Bag.IsValid()))
				{
					TestEqual(TEXT("LayerBottomAltitude survived the round trip"),
						Bag->GetNumberField(TEXT("LayerBottomAltitude")), 7.75, 0.0001);
				}
			}

			// The preset-driven cloud must collapse back to its preset name, exactly
			// like the fog/sky entries do — otherwise cloud documents would bloat.
			const TSharedPtr<FJsonObject> CapturedPreset = EntryById(Doc, TEXT("weather"));
			if (CapturedPreset.IsValid())
			{
				FString Name;
				CapturedPreset->TryGetStringField(TEXT("preset"), Name);
				TestEqual(TEXT("the preset-driven cloud captured back as its preset"), Name, CloudPreset);
			}
		}
	}

	Remove(LayoutId);
	TestEqual(TEXT("cleanup removed the layout"), CountInLevel(LayoutId), 0);
	return true;
}

// ============================================================================
// 14. CAPTURE — the shipped allowlist is a real engine allowlist
//
// Engine-upgrade canary for the reverse direction, matching the one the layouts
// themselves get: if UE renames one of the properties mesh.capture_level_layout
// is told to read, this goes red before a user silently stops getting it.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutCaptureKeysTest,
	"Monolith.Mesh.Layouts.CaptureKeysAreValid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutCaptureKeysTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;
	EnsureRegistered();

	TArray<FString> Warnings;

	const TArray<FString> ActorKeys = FMonolithMeshLayoutActions::LoadCaptureKeys(TEXT("actor"), Warnings);
	TestTrue(TEXT("the shipped `readback.actor` allowlist is not empty"), ActorKeys.Num() > 0);
	for (const FString& Key : ActorKeys)
	{
		TestNotNull(*FString::Printf(TEXT("capture key 'actor.%s' exists on AActor"), *Key),
			FMonolithReflectionWalker::FindPropertyForwarding(AActor::StaticClass(), Key));
	}

	const TArray<FString> CompKeys = FMonolithMeshLayoutActions::LoadCaptureKeys(TEXT("actor_component"), Warnings);
	TestTrue(TEXT("the shipped `readback.actor_component` allowlist is not empty"), CompKeys.Num() > 0);
	for (const FString& Key : CompKeys)
	{
		// The list deliberately spans component classes: a name only has to exist on
		// SOME root component type a layout can place, not on every one of them.
		const bool bResolves =
			FMonolithReflectionWalker::FindPropertyForwarding(USceneComponent::StaticClass(), Key) != nullptr ||
			FMonolithReflectionWalker::FindPropertyForwarding(UStaticMeshComponent::StaticClass(), Key) != nullptr;
		TestTrue(*FString::Printf(
			TEXT("capture key 'actor_component.%s' exists on USceneComponent or UStaticMeshComponent"), *Key),
			bResolves);
	}

	const TArray<FString> AtmoKeys = FMonolithMeshLayoutActions::LoadCaptureKeys(TEXT("atmosphere_actor"), Warnings);
	TestTrue(TEXT("the shipped `readback.atmosphere_actor` allowlist is not empty"), AtmoKeys.Num() > 0);
	for (const FString& Key : AtmoKeys)
	{
		TestNotNull(*FString::Printf(TEXT("capture key 'atmosphere_actor.%s' exists on APostProcessVolume"), *Key),
			FMonolithReflectionWalker::FindPropertyForwarding(APostProcessVolume::StaticClass(), Key));
	}

	// The volume read-back sets are per volume TYPE and resolve against the volume
	// ACTOR class that type token maps to — so the section name and the type token can
	// never drift apart without this going red.
	{
		const TCHAR* VolumeTypes[] = { TEXT("pain"), TEXT("audio") };
		for (const TCHAR* Type : VolumeTypes)
		{
			FString TypeError;
			UClass* VolumeClass = FMonolithMeshVolumeActions::ResolveVolumeClass(Type, TypeError);
			TestNotNull(*FString::Printf(TEXT("volume type '%s' still resolves (%s)"), Type, *TypeError),
				VolumeClass);
			if (!VolumeClass) { continue; }

			const TArray<FString> Keys = FMonolithMeshLayoutActions::LoadCaptureKeys(
				FString::Printf(TEXT("volume_%s"), Type), Warnings);
			TestTrue(*FString::Printf(TEXT("the shipped `readback.volume_%s` allowlist is not empty"), Type),
				Keys.Num() > 0);
			for (const FString& Key : Keys)
			{
				TestNotNull(*FString::Printf(TEXT("capture key 'volume_%s.%s' exists on %s"),
					Type, *Key, *VolumeClass->GetName()),
					FMonolithReflectionWalker::FindPropertyForwarding(VolumeClass, Key));
			}
		}
	}

	// Every curated spawn_volume alias must still name a real property, or an old
	// document would apply "successfully" while writing nothing.
	for (const FMonolithMeshVolumeActions::FVolumePropertyAlias& Alias :
		FMonolithMeshVolumeActions::GetVolumePropertyAliases())
	{
		bool bResolvesSomewhere = false;
		for (const FString& Type : FMonolithMeshVolumeActions::GetVolumeTokens())
		{
			FString TypeError;
			UClass* VolumeClass = FMonolithMeshVolumeActions::ResolveVolumeClass(Type, TypeError);
			if (VolumeClass && FMonolithReflectionWalker::FindPropertyForwarding(VolumeClass, Alias.Property))
			{
				bResolvesSomewhere = true;
				break;
			}
		}
		TestTrue(*FString::Printf(
			TEXT("the curated alias '%s' still names a real UPROPERTY ('%s') on some volume type"),
			Alias.Alias, Alias.Property), bResolvesSomewhere);
	}

	// AActor::Tags must NEVER be captured: apply owns the layout tags, and a
	// captured Tags array would fight the identity mechanism on re-apply.
	TestFalse(TEXT("AActor::Tags is deliberately absent from the actor allowlist"),
		ActorKeys.ContainsByPredicate([](const FString& K) { return K.Equals(TEXT("Tags")); }));

	return true;
}

// ============================================================================
// 15. CAPTURE — an applied layout captures back to the entries that were applied
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutCaptureFromTagTest,
	"Monolith.Mesh.Layouts.CaptureFromTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutCaptureFromTagTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;
	EnsureRegistered();

	const FString LayoutId = TEXT("monolith_test_capture");
	Remove(LayoutId);

	TSharedPtr<FJsonObject> Pad = Entry(TEXT("pad"), TEXT("actor"));
	Pad->SetStringField(TEXT("class"), TEXT("/Engine/BasicShapes/Cube.Cube"));
	Pad->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));
	Pad->SetArrayField(TEXT("scale"), Vec(2.0, 2.0, 0.5));
	{
		auto Comp = MakeShared<FJsonObject>();
		Comp->SetStringField(TEXT("Mobility"), TEXT("Movable"));
		Pad->SetObjectField(TEXT("component_properties"), Comp);
	}

	TSharedPtr<FJsonObject> Lamp = Entry(TEXT("lamp"), TEXT("light"));
	Lamp->SetStringField(TEXT("type"), TEXT("point"));
	Lamp->SetStringField(TEXT("preset"), TEXT("bulb_warm_60w"));
	Lamp->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 300.0));

	TSharedPtr<FJsonObject> Trig = Entry(TEXT("trig"), TEXT("volume"));
	Trig->SetStringField(TEXT("type"), TEXT("trigger"));
	Trig->SetArrayField(TEXT("location"), Vec(200.0, 0.0, 0.0));
	Trig->SetArrayField(TEXT("extent"), Vec(50.0, 60.0, 70.0));

	FMonolithActionResult Applied = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body({ Pad, Lamp, Trig })));
	TestTrue(FString::Printf(TEXT("the source layout applies (%s)"), *Applied.ErrorMessage), Applied.bSuccess);
	if (!Applied.bSuccess) { Remove(LayoutId); return false; }

	FMonolithActionResult R = Exec(TEXT("capture_level_layout"), CaptureParams(LayoutId));
	TestTrue(FString::Printf(TEXT("capture from the layout tag succeeds (%s)"), *R.ErrorMessage), R.bSuccess);
	if (!R.bSuccess) { Remove(LayoutId); return false; }

	int32 EntryCount = 0;
	R.Result->TryGetNumberField(TEXT("entry_count"), EntryCount);
	TestEqual(TEXT("every applied entry came back"), EntryCount, 3);

	bool bValid = false;
	R.Result->TryGetBoolField(TEXT("valid"), bValid);
	TestTrue(TEXT("the captured document resolves against the live engine"), bValid);

	const TSharedPtr<FJsonObject> CapturedBody = BodyOf(R);
	TestTrue(TEXT("the document body is returned in the response"), CapturedBody.IsValid());

	// --- the mesh actor ---
	const TSharedPtr<FJsonObject> PadEntry = EntryById(CapturedBody, TEXT("pad"));
	TestTrue(TEXT("the 'pad' entry survived the round trip by its entry tag"), PadEntry.IsValid());
	if (PadEntry.IsValid())
	{
		FString Kind, Class;
		PadEntry->TryGetStringField(TEXT("kind"), Kind);
		PadEntry->TryGetStringField(TEXT("class"), Class);
		TestEqual(TEXT("'pad' is captured as an actor entry"), Kind, FString(TEXT("actor")));
		TestEqual(TEXT("'pad' names the mesh asset it was placed from"),
			Class, FString(TEXT("/Engine/BasicShapes/Cube.Cube")));
		TestEqual(TEXT("'pad' kept its scale"), AxisOf(PadEntry, TEXT("scale"), 0), 2.0);

		const TSharedPtr<FJsonObject> CompBag = BagOf(PadEntry, TEXT("component_properties"));
		TestTrue(TEXT("'pad' captured its non-default component properties"), CompBag.IsValid());
		if (CompBag.IsValid())
		{
			FString Mobility;
			CompBag->TryGetStringField(TEXT("Mobility"), Mobility);
			TestEqual(TEXT("the component Mobility the layout set is read back"), Mobility, FString(TEXT("Movable")));
		}
	}

	// --- the light: the preset is preferred over restating its values ---
	const TSharedPtr<FJsonObject> LampEntry = EntryById(CapturedBody, TEXT("lamp"));
	TestTrue(TEXT("the 'lamp' entry survived"), LampEntry.IsValid());
	if (LampEntry.IsValid())
	{
		FString Kind, Type, Preset;
		LampEntry->TryGetStringField(TEXT("kind"), Kind);
		LampEntry->TryGetStringField(TEXT("type"), Type);
		LampEntry->TryGetStringField(TEXT("preset"), Preset);
		TestEqual(TEXT("'lamp' is captured as a light entry"), Kind, FString(TEXT("light")));
		TestEqual(TEXT("'lamp' kept its light type"), Type, FString(TEXT("point")));
		TestEqual(TEXT("an exact preset match is referenced by name instead of raw values"),
			Preset, FString(TEXT("bulb_warm_60w")));
		TestEqual(TEXT("'lamp' kept its height"), AxisOf(LampEntry, TEXT("location"), 2), 300.0);

		// Everything the preset covers must be gone from the raw bag — it was
		// PROVED equal, not assumed.
		const TSharedPtr<FJsonObject> Bag = BagOf(LampEntry, TEXT("properties"));
		if (Bag.IsValid())
		{
			TestFalse(TEXT("a preset-covered property is not restated in `properties`"),
				Bag->HasField(TEXT("AttenuationRadius")));
		}
	}

	// --- the volume: extent recovered exactly from the box builder ---
	const TSharedPtr<FJsonObject> TrigEntry = EntryById(CapturedBody, TEXT("trig"));
	TestTrue(TEXT("the 'trig' entry survived"), TrigEntry.IsValid());
	if (TrigEntry.IsValid())
	{
		FString Kind, Type;
		TrigEntry->TryGetStringField(TEXT("kind"), Kind);
		TrigEntry->TryGetStringField(TEXT("type"), Type);
		TestEqual(TEXT("'trig' is captured as a volume entry"), Kind, FString(TEXT("volume")));
		TestEqual(TEXT("'trig' kept its volume type"), Type, FString(TEXT("trigger")));
		TestEqual(TEXT("the brush half-extent X is recovered exactly"), AxisOf(TrigEntry, TEXT("extent"), 0), 50.0);
		TestEqual(TEXT("the brush half-extent Y is recovered exactly"), AxisOf(TrigEntry, TEXT("extent"), 1), 60.0);
		TestEqual(TEXT("the brush half-extent Z is recovered exactly"), AxisOf(TrigEntry, TEXT("extent"), 2), 70.0);
	}

	Remove(LayoutId);
	return true;
}

// ============================================================================
// 16. CAPTURE — the round trip. THE point of the whole surface.
//
//   apply -> tweak in the level -> capture -> remove -> re-apply -> the tweak
//   is still there.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutCaptureRoundTripTest,
	"Monolith.Mesh.Layouts.CaptureRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutCaptureRoundTripTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;
	EnsureRegistered();

	const FString LayoutId = TEXT("monolith_test_roundtrip");
	Remove(LayoutId);

	TSharedPtr<FJsonObject> Lamp = Entry(TEXT("lamp"), TEXT("light"));
	Lamp->SetStringField(TEXT("type"), TEXT("point"));
	Lamp->SetStringField(TEXT("preset"), TEXT("bulb_warm_60w"));
	Lamp->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 300.0));

	FMonolithActionResult Applied = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body({ Lamp })));
	TestTrue(FString::Printf(TEXT("the source layout applies (%s)"), *Applied.ErrorMessage), Applied.bSuccess);
	if (!Applied.bSuccess) { Remove(LayoutId); return false; }

	AActor* LampActor = ActorForEntry(LayoutId, TEXT("lamp"));
	TestNotNull(TEXT("the light actor is in the level"), LampActor);
	if (!LampActor) { Remove(LayoutId); return false; }
	const FString LampName = LampActor->GetActorNameOrLabel();

	// --- the "I nudged it because it looked better" step -------------------
	const double TweakedIntensity = 1234.0;
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("actor_name"), LampName);
		auto Props = MakeShared<FJsonObject>();
		Props->SetNumberField(TEXT("Intensity"), TweakedIntensity);
		P->SetObjectField(TEXT("properties"), Props);
		FMonolithActionResult Set = Exec(TEXT("set_light_properties"), P);
		TestTrue(FString::Printf(TEXT("the manual tweak lands (%s)"), *Set.ErrorMessage), Set.bSuccess);
	}
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("actor_name"), LampName);
		P->SetArrayField(TEXT("location"), Vec(10.0, 20.0, 330.0));
		FMonolithActionResult Moved = Exec(TEXT("move_actor"), P);
		TestTrue(FString::Printf(TEXT("the manual move lands (%s)"), *Moved.ErrorMessage), Moved.bSuccess);
	}

	// --- capture -----------------------------------------------------------
	FMonolithActionResult R = Exec(TEXT("capture_level_layout"), CaptureParams(LayoutId));
	TestTrue(FString::Printf(TEXT("capture succeeds (%s)"), *R.ErrorMessage), R.bSuccess);
	if (!R.bSuccess) { Remove(LayoutId); return false; }

	const TSharedPtr<FJsonObject> CapturedBody = BodyOf(R);
	const TSharedPtr<FJsonObject> LampEntry = EntryById(CapturedBody, TEXT("lamp"));
	TestTrue(TEXT("the tweaked light is in the captured document"), LampEntry.IsValid());
	if (!LampEntry.IsValid()) { Remove(LayoutId); return false; }

	TestEqual(TEXT("the captured location is the nudged one, not the document's"),
		AxisOf(LampEntry, TEXT("location"), 2), 330.0);

	// The preset no longer describes this light, so capture must NOT claim it does.
	FString Preset;
	LampEntry->TryGetStringField(TEXT("preset"), Preset);
	TestTrue(TEXT("a preset that no longer matches exactly is not referenced"), Preset.IsEmpty());

	const TSharedPtr<FJsonObject> Bag = BagOf(LampEntry, TEXT("properties"));
	TestTrue(TEXT("the light's differing values are written out as raw properties"), Bag.IsValid());
	if (Bag.IsValid())
	{
		double CapturedIntensity = 0.0;
		TestTrue(TEXT("Intensity is captured"), Bag->TryGetNumberField(TEXT("Intensity"), CapturedIntensity));
		TestEqual(TEXT("the captured Intensity is the tweaked value"), CapturedIntensity, TweakedIntensity);
		TestTrue(TEXT("the values the preset used to supply are now written out in full"),
			Bag->HasField(TEXT("AttenuationRadius")));
	}

	// --- re-apply the CAPTURED document into an empty slot ------------------
	Remove(LayoutId);
	TestEqual(TEXT("the level is empty of this layout before the re-apply"), CountInLevel(LayoutId), 0);

	FMonolithActionResult ReApplied = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, CapturedBody));
	TestTrue(FString::Printf(TEXT("the captured document re-applies (%s)"), *ReApplied.ErrorMessage),
		ReApplied.bSuccess);
	if (!ReApplied.bSuccess) { Remove(LayoutId); return false; }

	AActor* Rebuilt = ActorForEntry(LayoutId, TEXT("lamp"));
	TestNotNull(TEXT("the re-applied light is in the level"), Rebuilt);
	if (Rebuilt)
	{
		TestTrue(TEXT("the re-applied light is at the nudged location"),
			Rebuilt->GetActorLocation().Equals(FVector(10.0, 20.0, 330.0), 0.01));

		FString CompError;
		ULightComponentBase* Comp = FMonolithMeshLightActions::ResolveLightComponent(Rebuilt, CompError);
		TestNotNull(TEXT("the re-applied actor still has its light component"), Comp);
		if (Comp)
		{
			TestEqual(TEXT("THE ROUND TRIP: the hand tweak survived capture + re-apply"),
				static_cast<double>(Comp->Intensity), TweakedIntensity);
		}
	}

	Remove(LayoutId);
	return true;
}

// ============================================================================
// 17. CAPTURE — the "only what differs from the default" filter really filters
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutCaptureDefaultFilterTest,
	"Monolith.Mesh.Layouts.CaptureFiltersDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutCaptureDefaultFilterTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;
	EnsureRegistered();

	const FString LayoutId = TEXT("monolith_test_capture_defaults");
	Remove(LayoutId);

	// One point light with exactly ONE property moved off its default.
	TSharedPtr<FJsonObject> Lamp = Entry(TEXT("lamp"), TEXT("light"));
	Lamp->SetStringField(TEXT("type"), TEXT("point"));
	Lamp->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 120.0));
	{
		auto Props = MakeShared<FJsonObject>();
		Props->SetNumberField(TEXT("Intensity"), 777.0);
		Lamp->SetObjectField(TEXT("properties"), Props);
	}

	FMonolithActionResult Applied = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body({ Lamp })));
	TestTrue(FString::Printf(TEXT("the source layout applies (%s)"), *Applied.ErrorMessage), Applied.bSuccess);
	if (!Applied.bSuccess) { Remove(LayoutId); return false; }

	// A read-back key that is definitely IN the shipped `common` set and definitely
	// still at its default, because nothing above touched it.
	const TCHAR* UntouchedKey = TEXT("CastShadows");
	TestTrue(TEXT("the probe key really is in the shipped point-light read-back set"),
		FMonolithMeshLightActions::LoadReadbackKeys(TEXT("point")).Contains(UntouchedKey));

	// --- default: filtered ---
	{
		TSharedPtr<FJsonObject> P = CaptureParams(LayoutId);
		P->SetBoolField(TEXT("use_presets"), false);
		FMonolithActionResult R = Exec(TEXT("capture_level_layout"), P);
		TestTrue(FString::Printf(TEXT("capture succeeds (%s)"), *R.ErrorMessage), R.bSuccess);
		if (!R.bSuccess) { Remove(LayoutId); return false; }

		const TSharedPtr<FJsonObject> Bag = BagOf(EntryById(BodyOf(R), TEXT("lamp")), TEXT("properties"));
		TestTrue(TEXT("the changed property is captured"), Bag.IsValid() && Bag->HasField(TEXT("Intensity")));
		TestFalse(TEXT("a read-back property still at its default is NOT written out"),
			Bag.IsValid() && Bag->HasField(UntouchedKey));
	}

	// --- include_defaults=true: not filtered ---
	{
		TSharedPtr<FJsonObject> P = CaptureParams(LayoutId);
		P->SetBoolField(TEXT("use_presets"), false);
		P->SetBoolField(TEXT("include_defaults"), true);
		FMonolithActionResult R = Exec(TEXT("capture_level_layout"), P);
		TestTrue(FString::Printf(TEXT("capture with include_defaults succeeds (%s)"), *R.ErrorMessage), R.bSuccess);
		if (!R.bSuccess) { Remove(LayoutId); return false; }

		const TSharedPtr<FJsonObject> Bag = BagOf(EntryById(BodyOf(R), TEXT("lamp")), TEXT("properties"));
		TestTrue(TEXT("include_defaults=true writes the default-valued property too"),
			Bag.IsValid() && Bag->HasField(UntouchedKey));
		TestTrue(TEXT("include_defaults=true produces a strictly larger bag"),
			Bag.IsValid() && Bag->Values.Num() > 1);
	}

	Remove(LayoutId);
	return true;
}

// ============================================================================
// 18. CAPTURE — an actor the format cannot express is REPORTED, never dropped
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutCaptureUnrepresentableTest,
	"Monolith.Mesh.Layouts.CaptureReportsUnrepresentable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutCaptureUnrepresentableTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;
	EnsureRegistered();

	UWorld* World = GetTestWorld();
	TestNotNull(TEXT("there is an editor world"), World);
	if (!World) { return false; }

	const FString LayoutId = TEXT("monolith_test_capture_lossy");
	Remove(LayoutId);

	// A StaticMeshActor whose mesh only exists in memory. Nothing a layout can name
	// points at it — a procedurally built mesh is the real-world version of this.
	AStaticMeshActor* Orphan = World->SpawnActor<AStaticMeshActor>();
	TestNotNull(TEXT("the fixture actor spawned"), Orphan);
	if (!Orphan) { return false; }
	Orphan->SetActorLabel(TEXT("MonolithCaptureTransientMesh"));

	UStaticMeshComponent* OrphanComp = Orphan->GetStaticMeshComponent();
	if (OrphanComp)
	{
		OrphanComp->SetMobility(EComponentMobility::Movable);
		OrphanComp->SetStaticMesh(NewObject<UStaticMesh>(GetTransientPackage(), NAME_None, RF_Transient));
	}
	const bool bFixtureIsTransient =
		OrphanComp && OrphanComp->GetStaticMesh() &&
		OrphanComp->GetStaticMesh()->GetPackage() == GetTransientPackage();
	TestTrue(TEXT("the fixture really carries a transient (unsaveable) mesh"), bFixtureIsTransient);

	// A second, perfectly representable actor, so the call succeeds and we can see
	// that the refusal is REPORTED rather than making the whole capture vanish.
	AStaticMeshActor* Good = World->SpawnActor<AStaticMeshActor>();
	if (Good) { Good->SetActorLabel(TEXT("MonolithCaptureGoodActor")); }

	bool bPassed = true;
	if (bFixtureIsTransient && Good)
	{
		TSharedPtr<FJsonObject> P = CaptureParams(LayoutId);
		P->SetStringField(TEXT("source"), TEXT("actors"));
		TArray<TSharedPtr<FJsonValue>> Names;
		Names.Add(MakeShared<FJsonValueString>(TEXT("MonolithCaptureTransientMesh")));
		Names.Add(MakeShared<FJsonValueString>(TEXT("MonolithCaptureGoodActor")));
		P->SetArrayField(TEXT("actors"), Names);

		FMonolithActionResult R = Exec(TEXT("capture_level_layout"), P);
		TestTrue(FString::Printf(TEXT("a mixed capture still succeeds (%s)"), *R.ErrorMessage), R.bSuccess);
		if (R.bSuccess)
		{
			int32 EntryCount = 0, SkippedCount = 0;
			R.Result->TryGetNumberField(TEXT("entry_count"), EntryCount);
			R.Result->TryGetNumberField(TEXT("skipped_count"), SkippedCount);
			TestEqual(TEXT("only the representable actor became an entry"), EntryCount, 1);
			TestEqual(TEXT("the other actor is counted as skipped, not silently missing"), SkippedCount, 1);

			const TArray<TSharedPtr<FJsonValue>>* SkipArr = nullptr;
			TestTrue(TEXT("the response carries a per-actor skip list"),
				R.Result->TryGetArrayField(TEXT("skipped"), SkipArr) && SkipArr && SkipArr->Num() == 1);
			if (SkipArr && SkipArr->Num() == 1)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if ((*SkipArr)[0]->TryGetObject(Obj) && Obj)
				{
					FString Name, Reason;
					(*Obj)->TryGetStringField(TEXT("actor_name"), Name);
					(*Obj)->TryGetStringField(TEXT("reason"), Reason);
					TestEqual(TEXT("the skip names the actor"), Name, FString(TEXT("MonolithCaptureTransientMesh")));
					TestTrue(TEXT("the reason says WHY, in terms the user can act on"),
						Reason.Contains(TEXT("transient")));
				}
			}

			// And the same reason must reach the document, so it survives a save.
			const TSharedPtr<FJsonObject> CapturedBody = BodyOf(R);
			const TSharedPtr<FJsonObject> CaptureBlock = BagOf(CapturedBody, TEXT("capture"));
			TestTrue(TEXT("the document itself records what it could not represent"), CaptureBlock.IsValid());
		}
		else
		{
			bPassed = false;
		}
	}

	// Capturing ONLY the unrepresentable actor must fail with the reason in the
	// message — an empty document with no explanation would be the silent drop.
	if (bFixtureIsTransient)
	{
		TSharedPtr<FJsonObject> P = CaptureParams(LayoutId);
		P->SetStringField(TEXT("source"), TEXT("actors"));
		TArray<TSharedPtr<FJsonValue>> Names;
		Names.Add(MakeShared<FJsonValueString>(TEXT("MonolithCaptureTransientMesh")));
		P->SetArrayField(TEXT("actors"), Names);

		FMonolithActionResult R = Exec(TEXT("capture_level_layout"), P);
		TestFalse(TEXT("capturing nothing but an unrepresentable actor is an error"), R.bSuccess);
		TestTrue(TEXT("that error explains why, per actor"), R.ErrorMessage.Contains(TEXT("transient")));
	}

	if (IsValid(Orphan)) { World->EditorDestroyActor(Orphan, false); }
	if (IsValid(Good))   { World->EditorDestroyActor(Good, false); }
	return bPassed;
}

// ============================================================================
// 19. CAPTURE — the explicit-actor and editor-selection sources
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutCaptureSourcesTest,
	"Monolith.Mesh.Layouts.CaptureSources",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutCaptureSourcesTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;
	EnsureRegistered();

	UWorld* World = GetTestWorld();
	TestNotNull(TEXT("there is an editor world"), World);
	if (!World || !GEditor) { return false; }

	const FString LayoutId = TEXT("monolith_test_capture_sources");

	AStaticMeshActor* A = World->SpawnActor<AStaticMeshActor>();
	AStaticMeshActor* B = World->SpawnActor<AStaticMeshActor>();
	TestNotNull(TEXT("fixture A spawned"), A);
	TestNotNull(TEXT("fixture B spawned"), B);
	if (!A || !B) { return false; }
	A->SetActorLabel(TEXT("MonolithCapSrcA"));
	B->SetActorLabel(TEXT("MonolithCapSrcB"));
	A->SetActorLocation(FVector(11.0, 0.0, 0.0));

	// --- source = actors ---
	{
		TSharedPtr<FJsonObject> P = CaptureParams(LayoutId);
		P->SetStringField(TEXT("source"), TEXT("actors"));
		TArray<TSharedPtr<FJsonValue>> Names;
		Names.Add(MakeShared<FJsonValueString>(TEXT("MonolithCapSrcA")));
		Names.Add(MakeShared<FJsonValueString>(TEXT("MonolithCapSrcB")));
		P->SetArrayField(TEXT("actors"), Names);

		FMonolithActionResult R = Exec(TEXT("capture_level_layout"), P);
		TestTrue(FString::Printf(TEXT("capture from an explicit actor list succeeds (%s)"), *R.ErrorMessage),
			R.bSuccess);
		if (R.bSuccess)
		{
			int32 EntryCount = 0;
			R.Result->TryGetNumberField(TEXT("entry_count"), EntryCount);
			TestEqual(TEXT("both named actors became entries"), EntryCount, 2);

			// Untagged actors get their id from their label.
			const TSharedPtr<FJsonObject> EntryA = EntryById(BodyOf(R), TEXT("monolithcapsrca"));
			TestTrue(TEXT("an untagged actor's entry id is derived from its label"), EntryA.IsValid());
			if (EntryA.IsValid())
			{
				TestEqual(TEXT("its world location is captured"), AxisOf(EntryA, TEXT("location"), 0), 11.0);
			}
		}
	}

	// --- a typo captures NOTHING rather than a smaller layout ---
	{
		TSharedPtr<FJsonObject> P = CaptureParams(LayoutId);
		P->SetStringField(TEXT("source"), TEXT("actors"));
		TArray<TSharedPtr<FJsonValue>> Names;
		Names.Add(MakeShared<FJsonValueString>(TEXT("MonolithCapSrcA")));
		Names.Add(MakeShared<FJsonValueString>(TEXT("MonolithCapSrcTypo")));
		P->SetArrayField(TEXT("actors"), Names);

		FMonolithActionResult R = Exec(TEXT("capture_level_layout"), P);
		TestFalse(TEXT("an unknown actor name fails the whole capture"), R.bSuccess);
		TestTrue(TEXT("the error names the actor it could not find"),
			R.ErrorMessage.Contains(TEXT("MonolithCapSrcTypo")));
	}

	// --- source = selection, through the editor's own selection set ---
	{
		GEditor->SelectNone(false, true, false);
		GEditor->SelectActor(B, true, false);

		TSharedPtr<FJsonObject> P = CaptureParams(LayoutId);
		P->SetStringField(TEXT("source"), TEXT("selection"));
		FMonolithActionResult R = Exec(TEXT("capture_level_layout"), P);
		TestTrue(FString::Printf(TEXT("capture from the editor selection succeeds (%s)"), *R.ErrorMessage),
			R.bSuccess);
		if (R.bSuccess)
		{
			int32 EntryCount = 0;
			R.Result->TryGetNumberField(TEXT("entry_count"), EntryCount);
			TestEqual(TEXT("exactly the selected actor was captured"), EntryCount, 1);
			TestTrue(TEXT("and it is the one that was selected"),
				EntryById(BodyOf(R), TEXT("monolithcapsrcb")).IsValid());
		}

		GEditor->SelectNone(false, true, false);
		FMonolithActionResult Empty = Exec(TEXT("capture_level_layout"), P);
		TestFalse(TEXT("an empty selection is a clear error, not an empty document"), Empty.bSuccess);
		TestTrue(TEXT("the error says what to do about it"),
			Empty.ErrorMessage.Contains(TEXT("selected")));
	}

	if (IsValid(A)) { World->EditorDestroyActor(A, false); }
	if (IsValid(B)) { World->EditorDestroyActor(B, false); }
	return true;
}

// ============================================================================
// 20. CAPTURE — place_light must honour the rotation it was asked for
//
// Found BY the capture work, and it is the one thing that broke the round trip:
// AActor::PostSpawnInitialize composes the spawn transform with the root
// component's archetype transform, and ADirectionalLight / ASpotLight ship with
// a non-identity relative rotation (-46 / -90 pitch). So place_light used to
// return a light rotated 46 (or 90) degrees away from the request, and a captured
// rotation drifted by that amount again on EVERY re-apply.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLightRotationFidelityTest,
	"Monolith.Mesh.Layouts.PlaceLightHonoursRotation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLightRotationFidelityTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;
	EnsureRegistered();

	const FString LayoutId = TEXT("monolith_test_light_rotation");
	Remove(LayoutId);

	// The two types whose archetype root carries a rotation, plus one that does not.
	struct FCase { const TCHAR* Id; const TCHAR* Type; double Pitch; double Yaw; };
	const FCase Cases[] = {
		{ TEXT("sun"),  TEXT("directional"), -35.0, 150.0 },
		{ TEXT("cone"), TEXT("spot"),        -60.0,  20.0 },
		{ TEXT("rect"), TEXT("rect"),        -15.0, -75.0 },
	};

	TArray<TSharedPtr<FJsonObject>> Entries;
	for (const FCase& C : Cases)
	{
		TSharedPtr<FJsonObject> E = Entry(C.Id, TEXT("light"));
		E->SetStringField(TEXT("type"), C.Type);
		E->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 500.0));
		E->SetArrayField(TEXT("rotation"), Vec(C.Pitch, C.Yaw, 0.0));
		Entries.Add(E);
	}

	FMonolithActionResult Applied = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body(Entries)));
	TestTrue(FString::Printf(TEXT("the rotated-light layout applies (%s)"), *Applied.ErrorMessage), Applied.bSuccess);
	if (!Applied.bSuccess) { Remove(LayoutId); return false; }

	for (const FCase& C : Cases)
	{
		AActor* Actor = ActorForEntry(LayoutId, C.Id);
		TestNotNull(*FString::Printf(TEXT("the %s light is in the level"), C.Type), Actor);
		if (!Actor) { continue; }
		const FRotator Got = Actor->GetActorRotation();
		TestTrue(*FString::Printf(
			TEXT("a %s light really faces the rotation it was given (asked [%g, %g, 0], got [%g, %g, %g])"),
			C.Type, C.Pitch, C.Yaw, Got.Pitch, Got.Yaw, Got.Roll),
			Got.Equals(FRotator(C.Pitch, C.Yaw, 0.0), 0.01));
	}

	// ...and therefore capture -> re-apply does not drift it.
	FMonolithActionResult R = Exec(TEXT("capture_level_layout"), CaptureParams(LayoutId));
	TestTrue(FString::Printf(TEXT("capture succeeds (%s)"), *R.ErrorMessage), R.bSuccess);
	if (R.bSuccess)
	{
		const TSharedPtr<FJsonObject> CapturedBody = BodyOf(R);
		const TSharedPtr<FJsonObject> SunEntry = EntryById(CapturedBody, TEXT("sun"));
		TestTrue(TEXT("the directional light was captured"), SunEntry.IsValid());
		if (SunEntry.IsValid())
		{
			TestTrue(TEXT("the captured pitch is the authored one, not the archetype-composed one"),
				FMath::IsNearlyEqual(AxisOf(SunEntry, TEXT("rotation"), 0), -35.0, 0.01));
			TestTrue(TEXT("transform numbers are written at a readable precision"),
				FMath::IsNearlyEqual(AxisOf(SunEntry, TEXT("rotation"), 1), 150.0, 1e-9));
		}

		Remove(LayoutId);
		FMonolithActionResult ReApplied = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, CapturedBody));
		TestTrue(FString::Printf(TEXT("the captured document re-applies (%s)"), *ReApplied.ErrorMessage),
			ReApplied.bSuccess);
		if (ReApplied.bSuccess)
		{
			if (AActor* Sun = ActorForEntry(LayoutId, TEXT("sun")))
			{
				TestTrue(TEXT("ROUND TRIP: the directional light's rotation survives capture + re-apply"),
					Sun->GetActorRotation().Equals(FRotator(-35.0, 150.0, 0.0), 0.01));
			}
		}
	}

	Remove(LayoutId);
	return true;
}

// ============================================================================
// 21. CAPTURE — save goes through the existing validating writer
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutCaptureSaveTest,
	"Monolith.Mesh.Layouts.CaptureSavesThroughSaveAction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutCaptureSaveTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;
	EnsureRegistered();

	const FString LayoutId = TEXT("monolith_test_capture_save");
	const FString FilePath = FMonolithMeshLayoutActions::Library().GetUserDirectory() / (LayoutId + TEXT(".json"));
	IFileManager::Get().Delete(*FilePath, false, true, true);
	Remove(LayoutId);

	TSharedPtr<FJsonObject> Lamp = Entry(TEXT("lamp"), TEXT("light"));
	Lamp->SetStringField(TEXT("type"), TEXT("spot"));
	Lamp->SetStringField(TEXT("preset"), TEXT("spot_key_neutral"));
	Lamp->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 400.0));
	Lamp->SetArrayField(TEXT("rotation"), Vec(-60.0, 0.0, 0.0));

	FMonolithActionResult Applied = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body({ Lamp })));
	TestTrue(FString::Printf(TEXT("the source layout applies (%s)"), *Applied.ErrorMessage), Applied.bSuccess);
	if (!Applied.bSuccess) { Remove(LayoutId); return false; }

	TSharedPtr<FJsonObject> P = CaptureParams(LayoutId);
	P->SetBoolField(TEXT("save"), true);
	P->SetBoolField(TEXT("overwrite"), true);
	P->SetStringField(TEXT("description"), TEXT("captured by an automation test"));

	FMonolithActionResult R = Exec(TEXT("capture_level_layout"), P);
	TestTrue(FString::Printf(TEXT("capture with save=true succeeds (%s)"), *R.ErrorMessage), R.bSuccess);
	if (R.bSuccess)
	{
		bool bSaved = false;
		R.Result->TryGetBoolField(TEXT("saved"), bSaved);
		TestTrue(TEXT("the response reports that it wrote the document"), bSaved);
		TestTrue(TEXT("the file is on disk where save_level_layout puts user layouts"),
			IFileManager::Get().FileExists(*FilePath));

		// It is a real named layout now — the loader can see it, and it still resolves.
		TArray<FString> Warnings;
		const TMap<FString, FMonolithNamedJsonObject> Layouts = FMonolithMeshLayoutActions::LoadLayouts(Warnings);
		const FMonolithNamedJsonObject* Loaded = Layouts.Find(LayoutId);
		TestNotNull(TEXT("the captured layout is loadable by id"), Loaded);
		if (Loaded)
		{
			TArray<FMonolithMeshLayoutActions::FResolvedEntry> Resolved;
			TArray<FString> Problems;
			TestTrue(TEXT("and the saved document resolves against the live engine"),
				FMonolithMeshLayoutActions::ResolveLayout(
					LayoutId, Loaded->Object, FVector::ZeroVector, FString(), Resolved, Problems));
			TestEqual(TEXT("with the entry it captured"), Resolved.Num(), 1);

			FString Description;
			Loaded->Object->TryGetStringField(TEXT("description"), Description);
			TestEqual(TEXT("the description the caller supplied is stored"),
				Description, FString(TEXT("captured by an automation test")));
		}
	}

	IFileManager::Get().Delete(*FilePath, false, true, true);
	TestFalse(TEXT("cleanup removed the test layout file"), IFileManager::Get().FileExists(*FilePath));
	Remove(LayoutId);
	return true;
}

// ============================================================================
// 22. spawn_actor names a class by OBJECT PATH — which is what makes Blueprint
//     actors expressible at all, and therefore capturable.
//
//     Also the one branch that had never been exercised: a Blueprint whose root
//     component comes from the SimpleConstructionScript has no root on its CDO, so
//     `component_properties` cannot be pre-validated and falls through to the
//     post-spawn write. The rollback on THAT path is asserted here.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshSpawnActorByClassPathTest,
	"Monolith.Mesh.Layouts.SpawnActorByClassPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshSpawnActorByClassPathTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;
	EnsureRegistered();

	UWorld* World = GetTestWorld();
	TestNotNull(TEXT("there is an editor world"), World);
	if (!World) { return false; }

	FString FixtureError;
	UClass* BPClass = CreateOrReuseTestActorBlueprint(FixtureBlueprintPath(), FixtureError);
	TestNotNull(*FString::Printf(TEXT("the fixture Blueprint builds (%s)"), *FixtureError), BPClass);
	if (!BPClass) { return false; }

	// The premise of the whole test: this Blueprint's root really does come from the
	// SCS, so the CDO has none. If a future engine change makes CDOs carry SCS roots,
	// this assertion fails and the deferred-validation branch below stops being the
	// thing under test — which is exactly when someone should be told.
	AActor* CDO = Cast<AActor>(BPClass->GetDefaultObject());
	TestNotNull(TEXT("the fixture class has a CDO"), CDO);
	TestNull(TEXT("PREMISE: an SCS-rooted Blueprint's CDO has no root component"),
		CDO ? CDO->GetRootComponent() : nullptr);

	const FString ExplicitPath = BPClass->GetPathName();          // /Game/.../BP_X.BP_X_C
	const FString FriendlyPath = FixtureBlueprintPath();          // /Game/.../BP_X

	// --- ResolveSpawnTarget accepts both spellings and lands on the same class ---
	{
		UClass* Cls = nullptr;
		UStaticMesh* Mesh = nullptr;
		FString Err;
		TestTrue(*FString::Printf(TEXT("the generated-class path resolves (%s)"), *Err),
			FMonolithMeshSceneActions::ResolveSpawnTarget(ExplicitPath, Cls, Mesh, Err));
		TestEqual(TEXT("and it resolves to the Blueprint's generated class"), Cls, BPClass);

		Cls = nullptr; Mesh = nullptr;
		TestTrue(*FString::Printf(TEXT("the friendlier Blueprint-asset path resolves too (%s)"), *Err),
			FMonolithMeshSceneActions::ResolveSpawnTarget(FriendlyPath, Cls, Mesh, Err));
		TestEqual(TEXT("to exactly the same class"), Cls, BPClass);
	}

	// --- spawning by the explicit generated-class path ---
	const int32 Before = CountActorsOfClass(BPClass);
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("class_or_mesh"), ExplicitPath);
		P->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));
		P->SetStringField(TEXT("name"), TEXT("MonolithBPPathExplicit"));
		auto Comp = MakeShared<FJsonObject>();
		Comp->SetStringField(TEXT("Mobility"), TEXT("Static"));
		P->SetObjectField(TEXT("component_properties"), Comp);

		FMonolithActionResult R = Exec(TEXT("spawn_actor"), P);
		TestTrue(FString::Printf(TEXT("spawn_actor accepts a Blueprint class path (%s)"), *R.ErrorMessage),
			R.bSuccess);
		TestEqual(TEXT("and one actor of that class appeared"), CountActorsOfClass(BPClass), Before + 1);

		// The deferred `component_properties` write really landed on the SCS root.
		FString FindError;
		AActor* Spawned = MonolithMeshUtils::FindActorByName(TEXT("MonolithBPPathExplicit"), FindError);
		TestNotNull(TEXT("the spawned Blueprint actor is findable"), Spawned);
		if (Spawned)
		{
			TestEqual(TEXT("it is the Blueprint class, not its parent"), Spawned->GetClass(), BPClass);
			USceneComponent* Root = Spawned->GetRootComponent();
			TestNotNull(TEXT("the spawned actor DOES have an SCS root"), Root);
			if (Root)
			{
				TestEqual(TEXT("component_properties reached the SCS root after the spawn"),
					static_cast<int32>(Root->Mobility.GetValue()), static_cast<int32>(EComponentMobility::Static));
			}
			World->EditorDestroyActor(Spawned, false);
		}
	}

	// --- spawning by the friendlier Blueprint-asset path ---
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("class_or_mesh"), FriendlyPath);
		P->SetArrayField(TEXT("location"), Vec(50.0, 0.0, 0.0));
		P->SetStringField(TEXT("name"), TEXT("MonolithBPPathFriendly"));
		FMonolithActionResult R = Exec(TEXT("spawn_actor"), P);
		TestTrue(FString::Printf(TEXT("spawn_actor accepts the Blueprint asset path (%s)"), *R.ErrorMessage),
			R.bSuccess);

		FString FindError;
		if (AActor* Spawned = MonolithMeshUtils::FindActorByName(TEXT("MonolithBPPathFriendly"), FindError))
		{
			TestEqual(TEXT("which spawns the generated class"), Spawned->GetClass(), BPClass);
			World->EditorDestroyActor(Spawned, false);
		}
	}

	// --- THE ROLLBACK ON THE DEFERRED PATH ---------------------------------
	// `component_properties` could not be checked before the spawn (no CDO root), so
	// the rejection happens AFTER the actor exists. Nothing may survive it.
	{
		const int32 CountBefore = CountActorsOfClass(BPClass);

		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("class_or_mesh"), ExplicitPath);
		P->SetArrayField(TEXT("location"), Vec(100.0, 0.0, 0.0));
		P->SetStringField(TEXT("name"), TEXT("MonolithBPPathRollback"));
		auto Comp = MakeShared<FJsonObject>();
		Comp->SetNumberField(TEXT("NoSuchComponentProperty"), 1.0);
		P->SetObjectField(TEXT("component_properties"), Comp);

		FMonolithActionResult R = Exec(TEXT("spawn_actor"), P);
		TestFalse(TEXT("a bad component_properties key on an SCS-rooted Blueprint is refused"), R.bSuccess);
		TestTrue(TEXT("the error names the key that was refused"),
			R.ErrorMessage.Contains(TEXT("NoSuchComponentProperty")));
		TestEqual(TEXT("ROLLBACK: the actor the rejected write was spawned for is gone"),
			CountActorsOfClass(BPClass), CountBefore);

		FString FindError;
		TestNull(TEXT("and it is not findable by the label it would have got"),
			MonolithMeshUtils::FindActorByName(TEXT("MonolithBPPathRollback"), FindError));
	}

	// --- the ACTOR bag is still pre-validated: same class, but the CDO exists ---
	{
		const int32 CountBefore = CountActorsOfClass(BPClass);
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("class_or_mesh"), ExplicitPath);
		P->SetArrayField(TEXT("location"), Vec(150.0, 0.0, 0.0));
		auto Props = MakeShared<FJsonObject>();
		Props->SetNumberField(TEXT("NoSuchActorProperty"), 1.0);
		P->SetObjectField(TEXT("properties"), Props);

		FMonolithActionResult R = Exec(TEXT("spawn_actor"), P);
		TestFalse(TEXT("a bad `properties` key is refused"), R.bSuccess);
		TestEqual(TEXT("with nothing spawned at all (phase 1, against the CDO)"),
			CountActorsOfClass(BPClass), CountBefore);
	}

	// --- misses are actionable -------------------------------------------------
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("class_or_mesh"), TEXT("/Game/Tests/Monolith/Mesh/BP_ThisDoesNotExist"));
		P->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));
		FMonolithActionResult R = Exec(TEXT("spawn_actor"), P);
		TestFalse(TEXT("a path to nothing is refused"), R.bSuccess);
		TestTrue(TEXT("and the error says both accepted Blueprint spellings"),
			R.ErrorMessage.Contains(TEXT("BP_Barrel.BP_Barrel_C")));
	}
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("class_or_mesh"), TEXT("PontLight"));
		P->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));
		FMonolithActionResult R = Exec(TEXT("spawn_actor"), P);
		TestFalse(TEXT("a mistyped class name is refused"), R.bSuccess);
		TestTrue(TEXT("with a did-you-mean that names the class actually meant"),
			R.ErrorMessage.Contains(TEXT("PointLight")));
	}

	return true;
}

// ============================================================================
// 23. CAPTURE — a Blueprint actor round-trips.
//
//     apply (naming the Blueprint by class path) -> capture -> re-apply -> capture
//     again, and the two documents must be IDENTICAL. That is the property that
//     makes the document the source of truth; anything weaker is a one-way street.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutBlueprintRoundTripTest,
	"Monolith.Mesh.Layouts.BlueprintRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutBlueprintRoundTripTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;
	EnsureRegistered();

	FString FixtureError;
	UClass* BPClass = CreateOrReuseTestActorBlueprint(FixtureBlueprintPath(), FixtureError);
	TestNotNull(*FString::Printf(TEXT("the fixture Blueprint builds (%s)"), *FixtureError), BPClass);
	if (!BPClass) { return false; }

	const FString ClassPath = BPClass->GetPathName();
	const FString LayoutId = TEXT("monolith_test_bp_roundtrip");
	Remove(LayoutId);

	TSharedPtr<FJsonObject> Prop = Entry(TEXT("prop"), TEXT("actor"));
	Prop->SetStringField(TEXT("class"), ClassPath);
	Prop->SetArrayField(TEXT("location"), Vec(120.0, -40.0, 60.0));
	{
		auto Comp = MakeShared<FJsonObject>();
		Comp->SetStringField(TEXT("Mobility"), TEXT("Static"));
		Prop->SetObjectField(TEXT("component_properties"), Comp);
	}

	FMonolithActionResult Applied = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body({ Prop })));
	TestTrue(FString::Printf(TEXT("a layout can place a Blueprint actor (%s)"), *Applied.ErrorMessage),
		Applied.bSuccess);
	if (!Applied.bSuccess) { Remove(LayoutId); return false; }

	AActor* Placed = ActorForEntry(LayoutId, TEXT("prop"));
	TestNotNull(TEXT("the Blueprint actor is in the level, tagged"), Placed);
	if (Placed)
	{
		TestEqual(TEXT("and it is the Blueprint class"), Placed->GetClass(), BPClass);
	}

	// --- capture: the actor that used to be REFUSED is now an entry -------------
	FMonolithActionResult First = Exec(TEXT("capture_level_layout"), CaptureParams(LayoutId));
	TestTrue(FString::Printf(TEXT("capture succeeds (%s)"), *First.ErrorMessage), First.bSuccess);
	if (!First.bSuccess) { Remove(LayoutId); return false; }

	int32 SkippedCount = -1;
	First.Result->TryGetNumberField(TEXT("skipped_count"), SkippedCount);
	TestEqual(TEXT("the Blueprint actor is NOT skipped any more"), SkippedCount, 0);

	bool bValid = false;
	First.Result->TryGetBoolField(TEXT("valid"), bValid);
	TestTrue(TEXT("the captured document resolves against the live engine"), bValid);

	const TSharedPtr<FJsonObject> FirstBody = BodyOf(First);
	const TSharedPtr<FJsonObject> PropEntry = EntryById(FirstBody, TEXT("prop"));
	TestTrue(TEXT("the Blueprint entry is in the captured document"), PropEntry.IsValid());
	if (PropEntry.IsValid())
	{
		FString Kind, Class;
		PropEntry->TryGetStringField(TEXT("kind"), Kind);
		PropEntry->TryGetStringField(TEXT("class"), Class);
		TestEqual(TEXT("captured as an actor entry"), Kind, FString(TEXT("actor")));
		TestEqual(TEXT("naming the Blueprint by its generated-class OBJECT PATH"), Class, ClassPath);

		const TSharedPtr<FJsonObject> CompBag = BagOf(PropEntry, TEXT("component_properties"));
		TestTrue(TEXT("the SCS root's non-default properties are captured"), CompBag.IsValid());
		if (CompBag.IsValid())
		{
			FString Mobility;
			CompBag->TryGetStringField(TEXT("Mobility"), Mobility);
			TestEqual(TEXT("including the Mobility written after the spawn"), Mobility, FString(TEXT("Static")));
		}
	}

	// --- re-apply the captured document, then capture again ---------------------
	Remove(LayoutId);
	TestEqual(TEXT("the level is empty of this layout before the re-apply"), CountInLevel(LayoutId), 0);

	FMonolithActionResult ReApplied = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, FirstBody));
	TestTrue(FString::Printf(TEXT("the captured document re-applies (%s)"), *ReApplied.ErrorMessage),
		ReApplied.bSuccess);
	if (!ReApplied.bSuccess) { Remove(LayoutId); return false; }

	AActor* Rebuilt = ActorForEntry(LayoutId, TEXT("prop"));
	TestNotNull(TEXT("the re-applied Blueprint actor is in the level"), Rebuilt);
	if (Rebuilt)
	{
		TestEqual(TEXT("as the same Blueprint class"), Rebuilt->GetClass(), BPClass);
		if (USceneComponent* Root = Rebuilt->GetRootComponent())
		{
			TestEqual(TEXT("with the component property the document carried"),
				static_cast<int32>(Root->Mobility.GetValue()), static_cast<int32>(EComponentMobility::Static));
		}
	}

	FMonolithActionResult Second = Exec(TEXT("capture_level_layout"), CaptureParams(LayoutId));
	TestTrue(FString::Printf(TEXT("the second capture succeeds (%s)"), *Second.ErrorMessage), Second.bSuccess);
	if (Second.bSuccess)
	{
		TestEqual(TEXT("THE ROUND TRIP: capture -> apply -> capture produces an IDENTICAL document"),
			Canonicalise(BodyOf(Second)), Canonicalise(FirstBody));
	}

	Remove(LayoutId);
	return true;
}

// ============================================================================
// 24. CAPTURE — a volume's properties round-trip, and the curated aliases that
//     were the old public contract still mean exactly what they used to.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLayoutVolumePropertyRoundTripTest,
	"Monolith.Mesh.Layouts.VolumePropertyRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLayoutVolumePropertyRoundTripTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;
	EnsureRegistered();

	const FString LayoutId = TEXT("monolith_test_volume_props");
	Remove(LayoutId);

	const double Damage = 37.0;
	const double Priority = 7.0;

	TSharedPtr<FJsonObject> Lava = Entry(TEXT("lava"), TEXT("volume"));
	Lava->SetStringField(TEXT("type"), TEXT("pain"));
	Lava->SetArrayField(TEXT("location"), Vec(600.0, 0.0, 0.0));
	Lava->SetArrayField(TEXT("extent"), Vec(100.0, 100.0, 50.0));
	{
		auto Props = MakeShared<FJsonObject>();
		Props->SetNumberField(TEXT("DamagePerSec"), Damage);
		Props->SetBoolField(TEXT("bPainCausing"), false);
		Lava->SetObjectField(TEXT("properties"), Props);
	}

	TSharedPtr<FJsonObject> Reverb = Entry(TEXT("reverb"), TEXT("volume"));
	Reverb->SetStringField(TEXT("type"), TEXT("audio"));
	Reverb->SetArrayField(TEXT("location"), Vec(-600.0, 0.0, 0.0));
	Reverb->SetArrayField(TEXT("extent"), Vec(200.0, 200.0, 150.0));
	{
		auto Props = MakeShared<FJsonObject>();
		Props->SetNumberField(TEXT("Priority"), Priority);
		Reverb->SetObjectField(TEXT("properties"), Props);
	}

	FMonolithActionResult Applied = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, Body({ Lava, Reverb })));
	TestTrue(FString::Printf(TEXT("volume properties apply through reflection (%s)"), *Applied.ErrorMessage),
		Applied.bSuccess);
	if (!Applied.bSuccess) { Remove(LayoutId); return false; }

	// --- the values really landed on the live actors ---
	auto CheckLive = [&](const TCHAR* What)
	{
		APainCausingVolume* Pain = Cast<APainCausingVolume>(ActorForEntry(LayoutId, TEXT("lava")));
		TestNotNull(*FString::Printf(TEXT("%s: the pain volume is in the level"), What), Pain);
		if (Pain)
		{
			TestEqual(*FString::Printf(TEXT("%s: DamagePerSec"), What),
				static_cast<double>(Pain->DamagePerSec), Damage);
			TestFalse(*FString::Printf(TEXT("%s: bPainCausing was turned off"), What), (bool)Pain->bPainCausing);
		}
		AAudioVolume* Audio = Cast<AAudioVolume>(ActorForEntry(LayoutId, TEXT("reverb")));
		TestNotNull(*FString::Printf(TEXT("%s: the audio volume is in the level"), What), Audio);
		if (Audio)
		{
			TestEqual(*FString::Printf(TEXT("%s: Priority"), What),
				static_cast<double>(Audio->GetPriority()), Priority);
		}
	};
	CheckLive(TEXT("after apply"));

	// --- capture: the properties come back, in their canonical spelling ---------
	FMonolithActionResult First = Exec(TEXT("capture_level_layout"), CaptureParams(LayoutId));
	TestTrue(FString::Printf(TEXT("capture succeeds (%s)"), *First.ErrorMessage), First.bSuccess);
	if (!First.bSuccess) { Remove(LayoutId); return false; }

	const TSharedPtr<FJsonObject> FirstBody = BodyOf(First);
	{
		const TSharedPtr<FJsonObject> Bag = BagOf(EntryById(FirstBody, TEXT("lava")), TEXT("properties"));
		TestTrue(TEXT("the pain volume captured a `properties` bag"), Bag.IsValid());
		if (Bag.IsValid())
		{
			double Captured = 0.0;
			TestTrue(TEXT("DamagePerSec is read back"), Bag->TryGetNumberField(TEXT("DamagePerSec"), Captured));
			TestEqual(TEXT("with the value the level holds"), Captured, Damage);
			bool bCausing = true;
			TestTrue(TEXT("bPainCausing is read back"), Bag->TryGetBoolField(TEXT("bPainCausing"), bCausing));
			TestFalse(TEXT("as the non-default value"), bCausing);
			TestFalse(TEXT("a property still at its default is not restated"), Bag->HasField(TEXT("PainInterval")));
		}

		const TSharedPtr<FJsonObject> AudioBag = BagOf(EntryById(FirstBody, TEXT("reverb")), TEXT("properties"));
		TestTrue(TEXT("the audio volume captured a `properties` bag"), AudioBag.IsValid());
		if (AudioBag.IsValid())
		{
			double CapturedPriority = 0.0;
			TestTrue(TEXT("Priority is read back"), AudioBag->TryGetNumberField(TEXT("Priority"), CapturedPriority));
			TestEqual(TEXT("with the value the level holds"), CapturedPriority, Priority);
		}
	}

	// --- re-apply + re-capture must be identical --------------------------------
	Remove(LayoutId);
	FMonolithActionResult ReApplied = Exec(TEXT("apply_level_layout"), ApplyParams(LayoutId, FirstBody));
	TestTrue(FString::Printf(TEXT("the captured document re-applies (%s)"), *ReApplied.ErrorMessage),
		ReApplied.bSuccess);
	if (!ReApplied.bSuccess) { Remove(LayoutId); return false; }
	CheckLive(TEXT("after re-apply"));

	FMonolithActionResult Second = Exec(TEXT("capture_level_layout"), CaptureParams(LayoutId));
	TestTrue(FString::Printf(TEXT("the second capture succeeds (%s)"), *Second.ErrorMessage), Second.bSuccess);
	if (Second.bSuccess)
	{
		TestEqual(TEXT("THE ROUND TRIP: a volume with properties re-captures IDENTICALLY"),
			Canonicalise(BodyOf(Second)), Canonicalise(FirstBody));
	}
	Remove(LayoutId);

	// --- COMPATIBILITY: the curated snake_case aliases still work ---------------
	// They are the shape every existing caller and every saved document uses. They
	// are translated, not deprecated, and a capture of the result is canonical — so
	// an old document upgrades itself the first time it is captured.
	{
		const FString AliasLayout = TEXT("monolith_test_volume_alias");
		Remove(AliasLayout);

		TSharedPtr<FJsonObject> Old = Entry(TEXT("lava"), TEXT("volume"));
		Old->SetStringField(TEXT("type"), TEXT("pain"));
		Old->SetArrayField(TEXT("location"), Vec(600.0, 300.0, 0.0));
		auto Props = MakeShared<FJsonObject>();
		Props->SetNumberField(TEXT("damage_per_sec"), Damage);
		Props->SetBoolField(TEXT("pain_causing"), false);
		Old->SetObjectField(TEXT("properties"), Props);

		FMonolithActionResult R = Exec(TEXT("apply_level_layout"), ApplyParams(AliasLayout, Body({ Old })));
		TestTrue(FString::Printf(TEXT("a document written with the old aliases still applies (%s)"),
			*R.ErrorMessage), R.bSuccess);

		if (R.bSuccess)
		{
			APainCausingVolume* Pain = Cast<APainCausingVolume>(ActorForEntry(AliasLayout, TEXT("lava")));
			TestNotNull(TEXT("the alias document placed its volume"), Pain);
			if (Pain)
			{
				TestEqual(TEXT("`damage_per_sec` still means DamagePerSec"),
					static_cast<double>(Pain->DamagePerSec), Damage);
				TestFalse(TEXT("`pain_causing` still means bPainCausing"), (bool)Pain->bPainCausing);
			}

			FMonolithActionResult Cap = Exec(TEXT("capture_level_layout"), CaptureParams(AliasLayout));
			TestTrue(FString::Printf(TEXT("and it captures (%s)"), *Cap.ErrorMessage), Cap.bSuccess);
			if (Cap.bSuccess)
			{
				const TSharedPtr<FJsonObject> Bag =
					BagOf(EntryById(BodyOf(Cap), TEXT("lava")), TEXT("properties"));
				TestTrue(TEXT("into a bag"), Bag.IsValid());
				if (Bag.IsValid())
				{
					TestTrue(TEXT("written in the canonical spelling"), Bag->HasField(TEXT("DamagePerSec")));
					TestFalse(TEXT("not the alias — a capture is what upgrades an old document"),
						Bag->HasField(TEXT("damage_per_sec")));
				}
			}
		}
		Remove(AliasLayout);
	}

	// --- and mesh.spawn_volume called directly honours both spellings ------------
	{
		UWorld* World = GetTestWorld();
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), TEXT("pain"));
		P->SetArrayField(TEXT("location"), Vec(900.0, 0.0, 0.0));
		P->SetStringField(TEXT("name"), TEXT("MonolithAliasSpawnVolume"));
		auto Props = MakeShared<FJsonObject>();
		Props->SetNumberField(TEXT("damage_per_sec"), 12.0);
		P->SetObjectField(TEXT("properties"), Props);

		FMonolithActionResult R = Exec(TEXT("spawn_volume"), P);
		TestTrue(FString::Printf(TEXT("spawn_volume takes an alias key (%s)"), *R.ErrorMessage), R.bSuccess);
		if (R.bSuccess)
		{
			const TArray<TSharedPtr<FJsonValue>>* Applied2 = nullptr;
			if (R.Result->TryGetArrayField(TEXT("properties_set"), Applied2) && Applied2 && Applied2->Num() == 1)
			{
				TestEqual(TEXT("and reports the canonical property it actually wrote"),
					(*Applied2)[0]->AsString(), FString(TEXT("DamagePerSec")));
			}
			else
			{
				AddError(TEXT("spawn_volume did not report exactly one applied property"));
			}

			FString FindError;
			if (AActor* Spawned = MonolithMeshUtils::FindActorByName(TEXT("MonolithAliasSpawnVolume"), FindError))
			{
				if (APainCausingVolume* Pain = Cast<APainCausingVolume>(Spawned))
				{
					TestEqual(TEXT("the value landed"), static_cast<double>(Pain->DamagePerSec), 12.0);
				}
				if (World) { World->EditorDestroyActor(Spawned, false); }
			}
		}
	}

	// --- a key that is not a property AND not an alias is still refused ---------
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), TEXT("pain"));
		P->SetArrayField(TEXT("location"), Vec(0.0, 0.0, 0.0));
		auto Props = MakeShared<FJsonObject>();
		Props->SetNumberField(TEXT("DamagePerSecond"), 1.0);
		P->SetObjectField(TEXT("properties"), Props);

		FMonolithActionResult R = Exec(TEXT("spawn_volume"), P);
		TestFalse(TEXT("an unknown volume property is refused"), R.bSuccess);
		TestTrue(TEXT("the error says nothing was spawned"), R.ErrorMessage.Contains(TEXT("Nothing was spawned")));
		TestTrue(TEXT("and offers the name that was meant"), R.ErrorMessage.Contains(TEXT("DamagePerSec")));
	}

	return true;
}

// ============================================================================
// select_actors sub_action=focus reports what actually happened
//
// Focus used to call MoveViewportCamerasToActor and report success no matter
// what — including the two cases where no camera moves at all: an actor with no
// renderable bounds (nothing to frame), and a session where no viewport has
// focus yet (bActiveViewportOnly=true then targets a null client). Both made
// the scripted "focus, then editor::capture_viewport" loop return an unchanged
// frame with no explanation.
//
// Headless ceiling: under -nullrhi there is no realised level viewport, so the
// branch this reaches is "no viewport is open". What is proven in every mode is
// the honesty rule — focusing something that cannot be framed is never reported
// as a success.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshFocusReportsWhetherCameraMovedTest,
	"Monolith.Mesh.Scene.FocusReportsWhetherCameraMoved",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshFocusReportsWhetherCameraMovedTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithLayoutTestUtils;

	UWorld* World = GetTestWorld();
	TestNotNull(TEXT("there is an editor world"), World);
	if (!World) { return false; }

	// A bare AActor: a root scene component only, no primitive, therefore no
	// renderable bounds — the engine has nothing to frame.
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const FVector Loc(0.0, 0.0, 0.0);
	const FRotator Rot = FRotator::ZeroRotator;
	AActor* Boundless = World->SpawnActor(AActor::StaticClass(), &Loc, &Rot, SpawnParams);
	TestNotNull(TEXT("the boundless fixture actor spawned"), Boundless);
	if (!Boundless) { return false; }
	Boundless->SetActorLabel(TEXT("MonolithFocusBoundlessProbe"));

	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("sub_action"), TEXT("focus"));
		TArray<TSharedPtr<FJsonValue>> Names;
		Names.Add(MakeShared<FJsonValueString>(TEXT("MonolithFocusBoundlessProbe")));
		P->SetArrayField(TEXT("actors"), Names);

		FMonolithActionResult R = Exec(TEXT("select_actors"), P);

		// The whole point: no camera moved, so this is not a success.
		TestFalse(TEXT("focusing an unframeable actor is not reported as success"), R.bSuccess);
		TestTrue(TEXT("the refusal says no camera moved"),
			R.ErrorMessage.Contains(TEXT("No viewport camera moved")));
		TestTrue(TEXT("the refusal names the actor it was asked to frame"),
			R.ErrorMessage.Contains(TEXT("MonolithFocusBoundlessProbe")));
		TestTrue(TEXT("the refusal explains which condition applies"),
			R.ErrorMessage.Contains(TEXT("no renderable bounds")) ||
			R.ErrorMessage.Contains(TEXT("No level editor viewport is open")));
	}

	// An unknown actor name is still the earlier, different error — the new
	// branch must not have swallowed the resolution failure.
	{
		auto P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("sub_action"), TEXT("focus"));
		TArray<TSharedPtr<FJsonValue>> Names;
		Names.Add(MakeShared<FJsonValueString>(TEXT("MonolithFocusNoSuchActor_92831")));
		P->SetArrayField(TEXT("actors"), Names);

		FMonolithActionResult R = Exec(TEXT("select_actors"), P);
		TestFalse(TEXT("focusing an unknown actor fails"), R.bSuccess);
		TestTrue(TEXT("and fails at resolution, not at the camera check"),
			R.ErrorMessage.Contains(TEXT("No actors to focus on")));
	}

	World->EditorDestroyActor(Boundless, false);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
