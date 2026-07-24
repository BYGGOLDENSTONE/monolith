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

			// spawn_volume's `properties` are curated snake_case aliases, not
			// UPROPERTY names, so there is nothing to look up by reflection. Their
			// canary is ResolveLayout's own honoured-key check, asserted above.
			if (E.KindToken == TEXT("volume"))
			{
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

#endif // WITH_DEV_AUTOMATION_TESTS
