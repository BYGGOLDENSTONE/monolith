// Copyright tumourlove. All Rights Reserved.

// =============================================================================
// MonolithMeshLightActionsTests.cpp
//
// Phase 2 (Goldenstone roadmap) — light placement + configuration.
//
// WHAT THESE TESTS PROVE HEADLESS
//   - every one of the five light types spawns with the expected ACTOR class and
//     resolves to the expected light COMPONENT class (incl. ASkyLight, whose
//     USkyLightComponent is NOT a ULightComponent),
//   - `properties` writes go through UE reflection and read back with
//     `mesh.get_light_properties`,
//   - an unknown / mistyped property name is a clean error AND applies nothing
//     (no partial write),
//   - an unknown light type, an unknown preset, a type-mismatched preset, a
//     non-light actor and a null world all produce actionable errors,
//   - every property name in the SHIPPED preset/read-back JSON actually resolves
//     on the matching engine component class (this is the engine-upgrade canary).
//
// WHAT THESE TESTS DO NOT PROVE
//   - anything about rendered output. The nightly suite runs `-nullrhi`, so
//     "the room looks lit" is not assertable here; it is the human acceptance step.
//   - the no-open-level branch of the ACTIONS. The automation run always has an
//     editor world, so the guard is exercised through its helper
//     (`RequireEditorWorld`) rather than by tearing the world down.
// =============================================================================

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "MonolithMeshLightActions.h"
#include "MonolithMeshLevelDesignActions.h"
#include "MonolithMeshUtils.h"
#include "MonolithToolRegistry.h"

#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/LightComponent.h"
#include "Components/LightComponentBase.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace MonolithLightTestUtils
{
	static UWorld* GetTestWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	/**
	 * Registration is skipped in some commandlet contexts, so register on demand.
	 * Mirrors the pattern the `jobs` namespace tests established.
	 */
	static void EnsureRegistered()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("mesh"), TEXT("place_light")))
		{
			FMonolithMeshLevelDesignActions::RegisterActions(Registry);
		}
		if (!Registry.HasAction(TEXT("mesh"), TEXT("get_light_properties")))
		{
			FMonolithMeshLightActions::RegisterActions(Registry);
		}
	}

	static TSharedPtr<FJsonObject> MakeParams()
	{
		return MakeShared<FJsonObject>();
	}

	static void SetLocation(const TSharedPtr<FJsonObject>& Params, double X, double Y, double Z)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(X));
		Arr.Add(MakeShared<FJsonValueNumber>(Y));
		Arr.Add(MakeShared<FJsonValueNumber>(Z));
		Params->SetArrayField(TEXT("location"), Arr);
	}

	static FMonolithActionResult Exec(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		EnsureRegistered();
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), Action, Params);
	}

	/** Spawn a light through the real action. Returns the spawned actor (or null). */
	static AActor* PlaceLight(const FString& Type, const FString& Label,
		const TSharedPtr<FJsonObject>& ExtraProps, const FString& Preset,
		FMonolithActionResult& OutResult)
	{
		TSharedPtr<FJsonObject> Params = MakeParams();
		Params->SetStringField(TEXT("type"), Type);
		Params->SetStringField(TEXT("name"), Label);
		SetLocation(Params, 0.0, 0.0, 500.0);
		if (ExtraProps.IsValid())
		{
			Params->SetObjectField(TEXT("properties"), ExtraProps);
		}
		if (!Preset.IsEmpty())
		{
			Params->SetStringField(TEXT("preset"), Preset);
		}

		OutResult = Exec(TEXT("place_light"), Params);
		if (!OutResult.bSuccess || !OutResult.Result.IsValid())
		{
			return nullptr;
		}
		FString SpawnedName;
		OutResult.Result->TryGetStringField(TEXT("actor_name"), SpawnedName);
		FString Err;
		return MonolithMeshUtils::FindActorByName(SpawnedName, Err);
	}

	static void Destroy(AActor* Actor)
	{
		if (Actor && IsValid(Actor))
		{
			if (UWorld* World = Actor->GetWorld())
			{
				World->DestroyActor(Actor);
			}
		}
	}

	/** Component class expected for each canonical light token. */
	static UClass* ExpectedComponentClass(const FString& Token)
	{
		if (Token == TEXT("directional")) { return UDirectionalLightComponent::StaticClass(); }
		if (Token == TEXT("point"))       { return UPointLightComponent::StaticClass(); }
		if (Token == TEXT("spot"))        { return USpotLightComponent::StaticClass(); }
		if (Token == TEXT("rect"))        { return URectLightComponent::StaticClass(); }
		if (Token == TEXT("sky"))         { return USkyLightComponent::StaticClass(); }
		return nullptr;
	}
}

// ============================================================================
// 1. Every light type spawns with the expected class — including SkyLight,
//    which no pre-existing Monolith light path could place.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLightsSpawnEveryTypeTest,
	"Monolith.Mesh.Lights.SpawnsEveryLightType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLightsSpawnEveryTypeTest::RunTest(const FString& /*Parameters*/)
{
	UWorld* World = MonolithLightTestUtils::GetTestWorld();
	if (!TestNotNull(TEXT("Editor world available"), World))
	{
		return false;
	}

	const TArray<FString>& Tokens = FMonolithMeshLightActions::GetLightTypeTokens();
	TestEqual(TEXT("Five canonical light types are exposed"), Tokens.Num(), 5);

	for (const FString& Token : Tokens)
	{
		FMonolithActionResult Result;
		const FString Label = FString::Printf(TEXT("MonolithTestLight_%s"), *Token);
		AActor* Actor = MonolithLightTestUtils::PlaceLight(Token, Label, nullptr, FString(), Result);

		if (!TestTrue(FString::Printf(TEXT("place_light succeeded for '%s' (%s)"), *Token, *Result.ErrorMessage),
			Result.bSuccess))
		{
			continue;
		}
		if (!TestNotNull(FString::Printf(TEXT("spawned actor found for '%s'"), *Token), Actor))
		{
			continue;
		}

		FString Err;
		UClass* ExpectedActorClass = FMonolithMeshLightActions::ResolveLightActorClass(Token, Err);
		TestTrue(FString::Printf(TEXT("'%s' spawned the expected actor class (got %s)"),
				*Token, *Actor->GetClass()->GetName()),
			Actor->GetClass() == ExpectedActorClass);

		ULightComponentBase* Comp = FMonolithMeshLightActions::ResolveLightComponent(Actor, Err);
		if (TestNotNull(FString::Printf(TEXT("'%s' actor resolved a light component (%s)"), *Token, *Err), Comp))
		{
			TestTrue(FString::Printf(TEXT("'%s' component is a %s"), *Token,
					*MonolithLightTestUtils::ExpectedComponentClass(Token)->GetName()),
				Comp->IsA(MonolithLightTestUtils::ExpectedComponentClass(Token)));
			TestEqual(FString::Printf(TEXT("'%s' round-trips to its own token"), *Token),
				FMonolithMeshLightActions::TokenForLightComponent(Comp), Token);
		}

		MonolithLightTestUtils::Destroy(Actor);
	}

	return true;
}

// ============================================================================
// 2. Reflection write + typed read-back round trip.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLightsPropertyRoundTripTest,
	"Monolith.Mesh.Lights.PropertyRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLightsPropertyRoundTripTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("Editor world available"), MonolithLightTestUtils::GetTestWorld()))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetNumberField(TEXT("Intensity"), 1234.0);
	Props->SetNumberField(TEXT("AttenuationRadius"), 777.0);
	Props->SetBoolField(TEXT("bUseTemperature"), true);

	FMonolithActionResult Spawn;
	AActor* Actor = MonolithLightTestUtils::PlaceLight(
		TEXT("point"), TEXT("MonolithTestLight_RoundTrip"), Props, FString(), Spawn);
	if (!TestTrue(FString::Printf(TEXT("place_light with properties succeeded (%s)"), *Spawn.ErrorMessage), Spawn.bSuccess)
		|| !TestNotNull(TEXT("spawned actor found"), Actor))
	{
		MonolithLightTestUtils::Destroy(Actor);
		return false;
	}

	TSharedPtr<FJsonObject> ReadParams = MonolithLightTestUtils::MakeParams();
	ReadParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	TArray<TSharedPtr<FJsonValue>> Names;
	Names.Add(MakeShared<FJsonValueString>(TEXT("Intensity")));
	Names.Add(MakeShared<FJsonValueString>(TEXT("AttenuationRadius")));
	Names.Add(MakeShared<FJsonValueString>(TEXT("bUseTemperature")));
	ReadParams->SetArrayField(TEXT("properties"), Names);

	FMonolithActionResult Read = MonolithLightTestUtils::Exec(TEXT("get_light_properties"), ReadParams);
	if (TestTrue(FString::Printf(TEXT("get_light_properties succeeded (%s)"), *Read.ErrorMessage), Read.bSuccess))
	{
		TestEqual(TEXT("light_type reported"), Read.Result->GetStringField(TEXT("light_type")), FString(TEXT("point")));
		const TSharedPtr<FJsonObject> Out = Read.Result->GetObjectField(TEXT("properties"));
		TestEqual(TEXT("Intensity round-tripped"), Out->GetNumberField(TEXT("Intensity")), 1234.0, 0.01);
		TestEqual(TEXT("AttenuationRadius round-tripped"), Out->GetNumberField(TEXT("AttenuationRadius")), 777.0, 0.01);
		TestTrue(TEXT("bUseTemperature round-tripped"), Out->GetBoolField(TEXT("bUseTemperature")));
	}

	// Default (data-driven) read-back set: no filter, no all=true.
	TSharedPtr<FJsonObject> DefaultParams = MonolithLightTestUtils::MakeParams();
	DefaultParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	FMonolithActionResult Defaults = MonolithLightTestUtils::Exec(TEXT("get_light_properties"), DefaultParams);
	if (TestTrue(FString::Printf(TEXT("default read-back succeeded (%s)"), *Defaults.ErrorMessage), Defaults.bSuccess))
	{
		TestTrue(TEXT("default read-back set is non-empty"),
			Defaults.Result->GetNumberField(TEXT("property_count")) > 0.0);
		TestTrue(TEXT("default read-back includes Intensity"),
			Defaults.Result->GetObjectField(TEXT("properties"))->HasField(TEXT("Intensity")));
	}

	MonolithLightTestUtils::Destroy(Actor);
	return true;
}

// ============================================================================
// 3. Unknown property name: clean error, and NOTHING is written.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLightsUnknownPropertyTest,
	"Monolith.Mesh.Lights.UnknownPropertyRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLightsUnknownPropertyTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("Editor world available"), MonolithLightTestUtils::GetTestWorld()))
	{
		return false;
	}

	// Spawn a known-good light first.
	TSharedPtr<FJsonObject> Good = MakeShared<FJsonObject>();
	Good->SetNumberField(TEXT("Intensity"), 500.0);

	FMonolithActionResult Spawn;
	AActor* Actor = MonolithLightTestUtils::PlaceLight(
		TEXT("point"), TEXT("MonolithTestLight_BadProp"), Good, FString(), Spawn);
	if (!TestTrue(TEXT("baseline light spawned"), Spawn.bSuccess) || !TestNotNull(TEXT("actor found"), Actor))
	{
		MonolithLightTestUtils::Destroy(Actor);
		return false;
	}

	// Now push a mix of one valid and one bogus key.
	TSharedPtr<FJsonObject> Mixed = MakeShared<FJsonObject>();
	Mixed->SetNumberField(TEXT("Intensity"), 999.0);
	Mixed->SetNumberField(TEXT("Intensitty"), 1.0);   // typo — must be rejected

	TSharedPtr<FJsonObject> SetParams = MonolithLightTestUtils::MakeParams();
	SetParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	SetParams->SetObjectField(TEXT("properties"), Mixed);

	FMonolithActionResult Bad = MonolithLightTestUtils::Exec(TEXT("set_light_properties"), SetParams);
	TestFalse(TEXT("set_light_properties fails on an unknown property"), Bad.bSuccess);
	TestTrue(TEXT("error names the offending key"), Bad.ErrorMessage.Contains(TEXT("Intensitty")));
	TestTrue(TEXT("error says the write was rejected"), Bad.ErrorMessage.Contains(TEXT("unknown field")));
	TestTrue(TEXT("error offers a did-you-mean candidate"), Bad.ErrorMessage.Contains(TEXT("did you mean")));

	// The valid sibling key must NOT have been applied.
	ULightComponentBase* Comp = Actor->FindComponentByClass<ULightComponentBase>();
	if (TestNotNull(TEXT("light component present"), Comp))
	{
		TestEqual(TEXT("no partial write — Intensity is untouched"), Comp->Intensity, 500.0f, 0.01f);
	}

	// Reading an unknown property is an error too, not a silent omission.
	TSharedPtr<FJsonObject> ReadParams = MonolithLightTestUtils::MakeParams();
	ReadParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	TArray<TSharedPtr<FJsonValue>> Names;
	Names.Add(MakeShared<FJsonValueString>(TEXT("NoSuchProperty")));
	ReadParams->SetArrayField(TEXT("properties"), Names);
	FMonolithActionResult BadRead = MonolithLightTestUtils::Exec(TEXT("get_light_properties"), ReadParams);
	TestFalse(TEXT("get_light_properties fails on an unknown property"), BadRead.bSuccess);
	TestTrue(TEXT("read error names the offending key"), BadRead.ErrorMessage.Contains(TEXT("NoSuchProperty")));

	MonolithLightTestUtils::Destroy(Actor);
	return true;
}

// ============================================================================
// 4. Sky light is configurable — the case every pre-existing path missed.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLightsSkyLightConfigurableTest,
	"Monolith.Mesh.Lights.SkyLightConfigurable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLightsSkyLightConfigurableTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("Editor world available"), MonolithLightTestUtils::GetTestWorld()))
	{
		return false;
	}

	FMonolithActionResult Spawn;
	AActor* Actor = MonolithLightTestUtils::PlaceLight(
		TEXT("sky"), TEXT("MonolithTestLight_Sky"), nullptr, FString(), Spawn);
	if (!TestTrue(FString::Printf(TEXT("sky light spawned (%s)"), *Spawn.ErrorMessage), Spawn.bSuccess)
		|| !TestNotNull(TEXT("actor found"), Actor))
	{
		MonolithLightTestUtils::Destroy(Actor);
		return false;
	}

	USkyLightComponent* Sky = Actor->FindComponentByClass<USkyLightComponent>();
	TestNotNull(TEXT("USkyLightComponent present"), Sky);
	TestNull(TEXT("a sky light is NOT a ULightComponent (why the old path missed it)"),
		Actor->FindComponentByClass<ULightComponent>());

	// Curated param + reflection param on the same call.
	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetNumberField(TEXT("SkyDistanceThreshold"), 123456.0);

	TSharedPtr<FJsonObject> SetParams = MonolithLightTestUtils::MakeParams();
	SetParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	SetParams->SetNumberField(TEXT("intensity"), 2.5);
	SetParams->SetObjectField(TEXT("properties"), Props);

	FMonolithActionResult Set = MonolithLightTestUtils::Exec(TEXT("set_light_properties"), SetParams);
	if (TestTrue(FString::Printf(TEXT("set_light_properties works on a sky light (%s)"), *Set.ErrorMessage), Set.bSuccess))
	{
		TestEqual(TEXT("light_type reported as sky"),
			Set.Result->GetStringField(TEXT("light_type")), FString(TEXT("sky")));
	}
	if (Sky)
	{
		TestEqual(TEXT("curated intensity applied to the sky light"), Sky->Intensity, 2.5f, 0.01f);
		TestEqual(TEXT("reflection write applied to the sky light"), Sky->SkyDistanceThreshold, 123456.0f, 1.0f);
	}

	// A ULightComponent-only property must fail loudly on a sky light.
	TSharedPtr<FJsonObject> BadProps = MakeShared<FJsonObject>();
	BadProps->SetNumberField(TEXT("Temperature"), 5000.0);
	TSharedPtr<FJsonObject> BadParams = MonolithLightTestUtils::MakeParams();
	BadParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	BadParams->SetObjectField(TEXT("properties"), BadProps);
	FMonolithActionResult Bad = MonolithLightTestUtils::Exec(TEXT("set_light_properties"), BadParams);
	TestFalse(TEXT("Temperature is rejected on a sky light"), Bad.bSuccess);
	TestTrue(TEXT("error names the component class"),
		Bad.ErrorMessage.Contains(TEXT("SkyLightComponent")));

	MonolithLightTestUtils::Destroy(Actor);
	return true;
}

// ============================================================================
// 5. The SHIPPED preset/read-back data is valid against this engine build.
//    This is the engine-upgrade canary: if Epic renames a light UPROPERTY, the
//    JSON goes stale and this test — not a user — finds out first.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLightsPresetDataValidTest,
	"Monolith.Mesh.Lights.PresetDataIsValid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLightsPresetDataValidTest::RunTest(const FString& /*Parameters*/)
{
	TArray<FString> Warnings;
	const TMap<FString, FMonolithMeshLightActions::FPreset> Presets = FMonolithMeshLightActions::LoadPresets(Warnings);

	TestEqual(FString::Printf(TEXT("no preset load warnings (%s)"), *FString::Join(Warnings, TEXT("; "))),
		Warnings.Num(), 0);
	TestTrue(TEXT("built-in presets loaded"), Presets.Num() > 0);

	for (const TPair<FString, FMonolithMeshLightActions::FPreset>& Pair : Presets)
	{
		const FMonolithMeshLightActions::FPreset& P = Pair.Value;

		FString TypeError;
		UClass* ActorClass = FMonolithMeshLightActions::ResolveLightActorClass(P.TypeToken, TypeError);
		if (!TestNotNull(FString::Printf(TEXT("preset '%s' names a valid light type (%s)"), *P.Name, *TypeError), ActorClass))
		{
			continue;
		}

		UClass* CompClass = MonolithLightTestUtils::ExpectedComponentClass(
			P.TypeToken.ToLower());
		if (!TestNotNull(FString::Printf(TEXT("preset '%s' maps to a component class"), *P.Name), CompClass))
		{
			continue;
		}

		if (!TestTrue(FString::Printf(TEXT("preset '%s' has properties"), *P.Name), P.Properties.IsValid()))
		{
			continue;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Prop : P.Properties->Values)
		{
			TestNotNull(
				FString::Printf(TEXT("preset '%s' property '%s' exists on %s"),
					*P.Name, *Prop.Key, *CompClass->GetName()),
				CompClass->FindPropertyByName(FName(*Prop.Key)));
		}
	}

	// Read-back key sets must resolve too.
	for (const FString& Token : FMonolithMeshLightActions::GetLightTypeTokens())
	{
		UClass* CompClass = MonolithLightTestUtils::ExpectedComponentClass(Token);
		const TArray<FString> Keys = FMonolithMeshLightActions::LoadReadbackKeys(Token);
		TestTrue(FString::Printf(TEXT("read-back set for '%s' is non-empty"), *Token), Keys.Num() > 0);
		for (const FString& Key : Keys)
		{
			TestNotNull(
				FString::Printf(TEXT("read-back key '%s' exists on %s"), *Key, *CompClass->GetName()),
				CompClass->FindPropertyByName(FName(*Key)));
		}
	}

	return true;
}

// ============================================================================
// 6. A preset actually applies, end to end, with values that live only in JSON.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLightsPresetAppliesTest,
	"Monolith.Mesh.Lights.PresetApplies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLightsPresetAppliesTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("Editor world available"), MonolithLightTestUtils::GetTestWorld()))
	{
		return false;
	}

	// Pick a real preset out of the data rather than naming one in C++, so the
	// test keeps working if the shipped library is re-authored.
	TArray<FString> Warnings;
	const TMap<FString, FMonolithMeshLightActions::FPreset> Presets = FMonolithMeshLightActions::LoadPresets(Warnings);

	const FMonolithMeshLightActions::FPreset* Chosen = nullptr;
	for (const TPair<FString, FMonolithMeshLightActions::FPreset>& Pair : Presets)
	{
		if (Pair.Value.TypeToken == TEXT("point") && Pair.Value.Properties.IsValid()
			&& Pair.Value.Properties->HasField(TEXT("Intensity")))
		{
			Chosen = &Pair.Value;
			break;
		}
	}
	if (!TestNotNull(TEXT("a point-light preset with an Intensity exists in the data"), Chosen))
	{
		return false;
	}

	const double ExpectedIntensity = Chosen->Properties->GetNumberField(TEXT("Intensity"));

	FMonolithActionResult Spawn;
	AActor* Actor = MonolithLightTestUtils::PlaceLight(
		TEXT("point"), TEXT("MonolithTestLight_Preset"), nullptr, Chosen->Name, Spawn);
	if (!TestTrue(FString::Printf(TEXT("place_light with preset '%s' succeeded (%s)"), *Chosen->Name, *Spawn.ErrorMessage),
			Spawn.bSuccess)
		|| !TestNotNull(TEXT("actor found"), Actor))
	{
		MonolithLightTestUtils::Destroy(Actor);
		return false;
	}

	ULightComponentBase* Comp = Actor->FindComponentByClass<ULightComponentBase>();
	if (TestNotNull(TEXT("light component present"), Comp))
	{
		TestEqual(TEXT("preset Intensity landed on the component"),
			static_cast<double>(Comp->Intensity), ExpectedIntensity, 0.01);
	}

	MonolithLightTestUtils::Destroy(Actor);
	return true;
}

// ============================================================================
// 7. Error paths: unknown type, unknown preset, type-mismatched preset.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLightsErrorPathsTest,
	"Monolith.Mesh.Lights.ErrorsAreActionable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLightsErrorPathsTest::RunTest(const FString& /*Parameters*/)
{
	// --- Unknown light type ---
	FString TypeError;
	TestNull(TEXT("an unknown light type does not resolve"),
		FMonolithMeshLightActions::ResolveLightActorClass(TEXT("banana"), TypeError));
	TestTrue(TEXT("unknown-type error lists the valid types"), TypeError.Contains(TEXT("directional")));
	TestTrue(TEXT("unknown-type error echoes the bad token"), TypeError.Contains(TEXT("banana")));

	// Aliases still resolve, so the error above is about a genuine miss.
	FString AliasError;
	TestNotNull(TEXT("'PointLight' resolves as an alias"),
		FMonolithMeshLightActions::ResolveLightActorClass(TEXT("PointLight"), AliasError));
	TestNotNull(TEXT("'APointLight' resolves as an alias"),
		FMonolithMeshLightActions::ResolveLightActorClass(TEXT("APointLight"), AliasError));
	TestNotNull(TEXT("'SkyLight' resolves as an alias"),
		FMonolithMeshLightActions::ResolveLightActorClass(TEXT("SkyLight"), AliasError));

	// --- No open level ---
	FString WorldError;
	TestFalse(TEXT("a null world is refused"),
		FMonolithMeshLightActions::RequireEditorWorld(nullptr, WorldError));
	TestTrue(TEXT("no-level error says what to do"), WorldError.Contains(TEXT("No open level")));
	if (UWorld* World = MonolithLightTestUtils::GetTestWorld())
	{
		FString Unused;
		TestTrue(TEXT("a real world is accepted"),
			FMonolithMeshLightActions::RequireEditorWorld(World, Unused));
	}

	// --- Unknown preset ---
	FMonolithMeshLightActions::FPreset Preset;
	FString PresetError;
	TestFalse(TEXT("an unknown preset is refused"),
		FMonolithMeshLightActions::ResolvePreset(TEXT("not_a_preset"), Preset, PresetError));
	TestTrue(TEXT("unknown-preset error lists the available names"),
		PresetError.Contains(TEXT("Available presets")));

	// --- Non-light actor ---
	UWorld* World = MonolithLightTestUtils::GetTestWorld();
	if (TestNotNull(TEXT("Editor world available"), World))
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		FVector Loc = FVector::ZeroVector;
		FRotator Rot = FRotator::ZeroRotator;
		AActor* Plain = World->SpawnActor(AActor::StaticClass(), &Loc, &Rot, SpawnParams);
		if (TestNotNull(TEXT("plain actor spawned"), Plain))
		{
			FString CompError;
			TestNull(TEXT("a plain actor has no light component"),
				FMonolithMeshLightActions::ResolveLightComponent(Plain, CompError));
			TestTrue(TEXT("non-light error suggests place_light"), CompError.Contains(TEXT("place_light")));
			MonolithLightTestUtils::Destroy(Plain);
		}
	}

	// --- Type-mismatched preset (a directional recipe on a point light) ---
	TArray<FString> Warnings;
	const TMap<FString, FMonolithMeshLightActions::FPreset> Presets = FMonolithMeshLightActions::LoadPresets(Warnings);
	FString DirectionalPreset;
	for (const TPair<FString, FMonolithMeshLightActions::FPreset>& Pair : Presets)
	{
		if (Pair.Value.TypeToken == TEXT("directional")) { DirectionalPreset = Pair.Key; break; }
	}
	if (!DirectionalPreset.IsEmpty() && World)
	{
		FMonolithActionResult Result;
		AActor* Actor = MonolithLightTestUtils::PlaceLight(
			TEXT("point"), TEXT("MonolithTestLight_Mismatch"), nullptr, DirectionalPreset, Result);
		TestFalse(TEXT("a directional preset is refused on a point light"), Result.bSuccess);
		TestTrue(TEXT("mismatch error names the preset"), Result.ErrorMessage.Contains(DirectionalPreset));
		TestNull(TEXT("the failed spawn left no actor behind"), Actor);
		MonolithLightTestUtils::Destroy(Actor);
	}

	return true;
}

// ============================================================================
// 8. mesh.list_light_presets — discovery surface.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshLightsListPresetsTest,
	"Monolith.Mesh.Lights.ListPresets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshLightsListPresetsTest::RunTest(const FString& /*Parameters*/)
{
	TSharedPtr<FJsonObject> Params = MonolithLightTestUtils::MakeParams();
	FMonolithActionResult All = MonolithLightTestUtils::Exec(TEXT("list_light_presets"), Params);
	if (!TestTrue(FString::Printf(TEXT("list_light_presets succeeded (%s)"), *All.ErrorMessage), All.bSuccess))
	{
		return false;
	}
	const double TotalCount = All.Result->GetNumberField(TEXT("total_count"));
	TestTrue(TEXT("presets were listed"), TotalCount > 0.0);
	TestEqual(TEXT("unfiltered count equals total"), All.Result->GetNumberField(TEXT("count")), TotalCount);
	TestEqual(TEXT("light_types is advertised"),
		All.Result->GetArrayField(TEXT("light_types")).Num(), 5);

	TSharedPtr<FJsonObject> Filtered = MonolithLightTestUtils::MakeParams();
	Filtered->SetStringField(TEXT("type"), TEXT("sky"));
	FMonolithActionResult Sky = MonolithLightTestUtils::Exec(TEXT("list_light_presets"), Filtered);
	if (TestTrue(FString::Printf(TEXT("filtered list succeeded (%s)"), *Sky.ErrorMessage), Sky.bSuccess))
	{
		const TArray<TSharedPtr<FJsonValue>>& Arr = Sky.Result->GetArrayField(TEXT("presets"));
		TestTrue(TEXT("at least one sky preset ships"), Arr.Num() > 0);
		TestTrue(TEXT("filter narrowed the list"), Arr.Num() < static_cast<int32>(TotalCount));
		for (const TSharedPtr<FJsonValue>& V : Arr)
		{
			TestEqual(TEXT("every filtered preset is a sky preset"),
				V->AsObject()->GetStringField(TEXT("type")), FString(TEXT("sky")));
		}
	}

	TSharedPtr<FJsonObject> BadFilter = MonolithLightTestUtils::MakeParams();
	BadFilter->SetStringField(TEXT("type"), TEXT("banana"));
	FMonolithActionResult Bad = MonolithLightTestUtils::Exec(TEXT("list_light_presets"), BadFilter);
	TestFalse(TEXT("an unknown type filter is refused"), Bad.bSuccess);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
