// Copyright tumourlove. All Rights Reserved.

// =============================================================================
// MonolithMeshAtmosphereActionsTests.cpp
//
// Phase 2 (Goldenstone roadmap), slice 2 — atmosphere + global illumination.
//
// WHAT THESE TESTS PROVE HEADLESS
//   - each of the three atmosphere types spawns with the expected ACTOR class and
//     resolves to the expected settings target (component for fog/sky, the
//     FPostProcessSettings struct member for a post-process volume),
//   - `properties` writes go through UE reflection and read back with
//     `mesh.get_atmosphere_properties`,
//   - writing a post-process key also flips its bOverride_ bit — the difference
//     between a write that shows up on screen and one that silently does nothing,
//   - an unknown / mistyped property name is a clean error with a did-you-mean AND
//     applies nothing (no partial write),
//   - Lumen values written with `mesh.set_lumen_settings` read back with
//     `mesh.get_lumen_settings`, including the enum-valued GI/reflection methods,
//   - a non-Lumen key is refused by set_lumen_settings with the full Lumen list,
//   - every property name in the SHIPPED preset/read-back JSON actually resolves on
//     the matching engine struct/class (the engine-upgrade canary — this is what
//     turns a UE property rename into a red test instead of a broken user).
//
// WHAT THESE TESTS DO NOT PROVE
//   - anything about rendered output. The nightly suite runs `-nullrhi`, so
//     "the fog looks right" / "Lumen bounce light appears" is NOT assertable here.
//     That is the human acceptance step.
//   - that the project renderer settings block matches what the Project Settings UI
//     shows; it is read from the URendererSettings CDO, which is the same source,
//     but no UI is instantiated headless.
//   - the no-open-level branch of the ACTIONS. The automation run always has an
//     editor world, so the guard is exercised through its helper (RequireEditorWorld).
// =============================================================================

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "MonolithMeshAtmosphereActions.h"
#include "MonolithMeshUtils.h"
#include "MonolithToolRegistry.h"
#include "Reflection/MonolithReflectionWalker.h"

#include "Editor.h"
#include "Engine/World.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/Scene.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "GameFramework/Actor.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace MonolithAtmosphereTestUtils
{
	static UWorld* GetTestWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	/** Registration is skipped in some commandlet contexts, so register on demand. */
	static void EnsureRegistered()
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("mesh"), TEXT("spawn_atmosphere")))
		{
			FMonolithMeshAtmosphereActions::RegisterActions(Registry);
		}
	}

	static TSharedPtr<FJsonObject> MakeParams()
	{
		return MakeShared<FJsonObject>();
	}

	static FMonolithActionResult Exec(const FString& Action, const TSharedPtr<FJsonObject>& Params)
	{
		EnsureRegistered();
		return FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), Action, Params);
	}

	/** Spawn an atmosphere actor through the real action. Returns the actor (or null). */
	static AActor* Spawn(const FString& Type, const FString& Label,
		const TSharedPtr<FJsonObject>& Props, const FString& Preset,
		FMonolithActionResult& OutResult)
	{
		TSharedPtr<FJsonObject> Params = MakeParams();
		Params->SetStringField(TEXT("type"), Type);
		Params->SetStringField(TEXT("name"), Label);
		if (Props.IsValid())
		{
			Params->SetObjectField(TEXT("properties"), Props);
		}
		if (!Preset.IsEmpty())
		{
			Params->SetStringField(TEXT("preset"), Preset);
		}

		OutResult = Exec(TEXT("spawn_atmosphere"), Params);
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

	/** Expected actor class per canonical token. */
	static UClass* ExpectedActorClass(const FString& Token)
	{
		if (Token == TEXT("post_process"))   { return APostProcessVolume::StaticClass(); }
		if (Token == TEXT("height_fog"))     { return AExponentialHeightFog::StaticClass(); }
		if (Token == TEXT("sky_atmosphere")) { return ASkyAtmosphere::StaticClass(); }
		return nullptr;
	}

	/** Expected settings-target struct per canonical token. */
	static UStruct* ExpectedTargetStruct(const FString& Token)
	{
		if (Token == TEXT("post_process"))   { return FPostProcessSettings::StaticStruct(); }
		if (Token == TEXT("height_fog"))     { return UExponentialHeightFogComponent::StaticClass(); }
		if (Token == TEXT("sky_atmosphere")) { return USkyAtmosphereComponent::StaticClass(); }
		return nullptr;
	}
}

// ============================================================================
// 1. Every atmosphere type spawns with the expected class and settings target.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshAtmosphereSpawnEveryTypeTest,
	"Monolith.Mesh.Atmosphere.SpawnsEveryType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshAtmosphereSpawnEveryTypeTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("Editor world available"), MonolithAtmosphereTestUtils::GetTestWorld()))
	{
		return false;
	}

	const TArray<FString>& Tokens = FMonolithMeshAtmosphereActions::GetAtmosphereTokens();
	TestEqual(TEXT("Three canonical atmosphere types are exposed"), Tokens.Num(), 3);

	for (const FString& Token : Tokens)
	{
		FMonolithActionResult Result;
		const FString Label = FString::Printf(TEXT("MonolithTestAtmos_%s"), *Token);
		AActor* Actor = MonolithAtmosphereTestUtils::Spawn(Token, Label, nullptr, FString(), Result);

		if (!TestTrue(FString::Printf(TEXT("spawn_atmosphere succeeded for '%s' (%s)"), *Token, *Result.ErrorMessage),
			Result.bSuccess))
		{
			continue;
		}
		if (!TestNotNull(FString::Printf(TEXT("spawned actor found for '%s'"), *Token), Actor))
		{
			continue;
		}

		TestTrue(FString::Printf(TEXT("'%s' spawned the expected actor class (got %s)"),
				*Token, *Actor->GetClass()->GetName()),
			Actor->GetClass() == MonolithAtmosphereTestUtils::ExpectedActorClass(Token));

		TestEqual(FString::Printf(TEXT("'%s' round-trips to its own token"), *Token),
			FMonolithMeshAtmosphereActions::TokenForActor(Actor), Token);

		FMonolithMeshAtmosphereActions::FTarget Target;
		FString Err;
		if (TestTrue(FString::Printf(TEXT("'%s' resolved a settings target (%s)"), *Token, *Err),
			FMonolithMeshAtmosphereActions::ResolveTarget(Actor, Target, Err)))
		{
			TestEqual(FString::Printf(TEXT("'%s' target struct"), *Token),
				static_cast<const UStruct*>(Target.Struct),
				static_cast<const UStruct*>(MonolithAtmosphereTestUtils::ExpectedTargetStruct(Token)));
			TestNotNull(FString::Printf(TEXT("'%s' target container"), *Token), Target.Container);

			// Only the post-process volume is override-aware — its settings are a
			// struct member gated by bOverride_ bits.
			TestEqual(FString::Printf(TEXT("'%s' override-awareness"), *Token),
				Target.bOverrideAware, Token == TEXT("post_process"));
		}

		// A post-process volume must come out with real brush geometry or it has no bounds.
		if (Token == TEXT("post_process"))
		{
			bool bBrushValid = false;
			Result.Result->TryGetBoolField(TEXT("brush_valid"), bBrushValid);
			TestTrue(TEXT("post-process volume spawned with valid brush geometry"), bBrushValid);
		}

		MonolithAtmosphereTestUtils::Destroy(Actor);
	}

	return true;
}

// ============================================================================
// 2. Fog: reflection write + typed read-back round trip (component target).
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshAtmosphereFogRoundTripTest,
	"Monolith.Mesh.Atmosphere.FogPropertyRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshAtmosphereFogRoundTripTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("Editor world available"), MonolithAtmosphereTestUtils::GetTestWorld()))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetNumberField(TEXT("FogDensity"), 0.037);
	Props->SetNumberField(TEXT("FogHeightFalloff"), 0.42);
	Props->SetBoolField(TEXT("bEnableVolumetricFog"), true);

	FMonolithActionResult Spawn;
	AActor* Actor = MonolithAtmosphereTestUtils::Spawn(
		TEXT("height_fog"), TEXT("MonolithTestAtmos_FogRoundTrip"), Props, FString(), Spawn);
	if (!TestTrue(FString::Printf(TEXT("spawn with properties succeeded (%s)"), *Spawn.ErrorMessage), Spawn.bSuccess)
		|| !TestNotNull(TEXT("spawned actor found"), Actor))
	{
		MonolithAtmosphereTestUtils::Destroy(Actor);
		return false;
	}

	UExponentialHeightFogComponent* Comp = Actor->FindComponentByClass<UExponentialHeightFogComponent>();
	if (TestNotNull(TEXT("fog component present"), Comp))
	{
		TestEqual(TEXT("FogDensity landed on the component"), Comp->FogDensity, 0.037f, 0.0001f);
		TestEqual(TEXT("FogHeightFalloff landed on the component"), Comp->FogHeightFalloff, 0.42f, 0.0001f);
		TestTrue(TEXT("bEnableVolumetricFog landed on the component"), Comp->bEnableVolumetricFog);
	}

	// Read the exact names back.
	TSharedPtr<FJsonObject> ReadParams = MonolithAtmosphereTestUtils::MakeParams();
	ReadParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	TArray<TSharedPtr<FJsonValue>> Names;
	Names.Add(MakeShared<FJsonValueString>(TEXT("FogDensity")));
	Names.Add(MakeShared<FJsonValueString>(TEXT("FogHeightFalloff")));
	Names.Add(MakeShared<FJsonValueString>(TEXT("bEnableVolumetricFog")));
	ReadParams->SetArrayField(TEXT("properties"), Names);

	FMonolithActionResult Read = MonolithAtmosphereTestUtils::Exec(TEXT("get_atmosphere_properties"), ReadParams);
	if (TestTrue(FString::Printf(TEXT("get_atmosphere_properties succeeded (%s)"), *Read.ErrorMessage), Read.bSuccess))
	{
		TestEqual(TEXT("atmosphere_type reported"),
			Read.Result->GetStringField(TEXT("atmosphere_type")), FString(TEXT("height_fog")));
		const TSharedPtr<FJsonObject> Out = Read.Result->GetObjectField(TEXT("properties"));
		TestEqual(TEXT("FogDensity round-tripped"), Out->GetNumberField(TEXT("FogDensity")), 0.037, 0.0001);
		TestEqual(TEXT("FogHeightFalloff round-tripped"), Out->GetNumberField(TEXT("FogHeightFalloff")), 0.42, 0.0001);
		TestTrue(TEXT("bEnableVolumetricFog round-tripped"), Out->GetBoolField(TEXT("bEnableVolumetricFog")));
		TestFalse(TEXT("a component target reports no override block"), Read.Result->HasField(TEXT("overrides")));
	}

	// Default (data-driven) read-back set.
	TSharedPtr<FJsonObject> DefaultParams = MonolithAtmosphereTestUtils::MakeParams();
	DefaultParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	FMonolithActionResult Defaults = MonolithAtmosphereTestUtils::Exec(TEXT("get_atmosphere_properties"), DefaultParams);
	if (TestTrue(FString::Printf(TEXT("default read-back succeeded (%s)"), *Defaults.ErrorMessage), Defaults.bSuccess))
	{
		TestTrue(TEXT("default read-back set is non-empty"),
			Defaults.Result->GetNumberField(TEXT("property_count")) > 0.0);
		TestTrue(TEXT("default read-back includes FogDensity"),
			Defaults.Result->GetObjectField(TEXT("properties"))->HasField(TEXT("FogDensity")));
	}

	MonolithAtmosphereTestUtils::Destroy(Actor);
	return true;
}

// ============================================================================
// 3. Post-process: the override bit is the whole game.
//    A written key must also flip bOverride_<Key>, or the value is inert.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshAtmospherePostProcessOverrideTest,
	"Monolith.Mesh.Atmosphere.PostProcessOverrideBits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshAtmospherePostProcessOverrideTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("Editor world available"), MonolithAtmosphereTestUtils::GetTestWorld()))
	{
		return false;
	}

	FMonolithActionResult Spawn;
	AActor* Actor = MonolithAtmosphereTestUtils::Spawn(
		TEXT("post_process"), TEXT("MonolithTestAtmos_Override"), nullptr, FString(), Spawn);
	if (!TestTrue(FString::Printf(TEXT("post-process volume spawned (%s)"), *Spawn.ErrorMessage), Spawn.bSuccess)
		|| !TestNotNull(TEXT("actor found"), Actor))
	{
		MonolithAtmosphereTestUtils::Destroy(Actor);
		return false;
	}

	APostProcessVolume* PPVol = Cast<APostProcessVolume>(Actor);
	if (!TestNotNull(TEXT("actor is an APostProcessVolume"), PPVol))
	{
		MonolithAtmosphereTestUtils::Destroy(Actor);
		return false;
	}

	TestFalse(TEXT("bOverride_BloomIntensity starts off"), PPVol->Settings.bOverride_BloomIntensity != 0);

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetNumberField(TEXT("BloomIntensity"), 0.75);
	Props->SetNumberField(TEXT("VignetteIntensity"), 0.9);

	TSharedPtr<FJsonObject> SetParams = MonolithAtmosphereTestUtils::MakeParams();
	SetParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	SetParams->SetObjectField(TEXT("properties"), Props);

	FMonolithActionResult Set = MonolithAtmosphereTestUtils::Exec(TEXT("set_atmosphere_properties"), SetParams);
	if (TestTrue(FString::Printf(TEXT("set_atmosphere_properties succeeded (%s)"), *Set.ErrorMessage), Set.bSuccess))
	{
		TestEqual(TEXT("target struct reported"),
			Set.Result->GetStringField(TEXT("target_struct")), FString(TEXT("PostProcessSettings")));
		TestEqual(TEXT("two values applied"), Set.Result->GetArrayField(TEXT("properties_set")).Num(), 2);
		TestEqual(TEXT("two override bits auto-enabled"),
			Set.Result->GetArrayField(TEXT("overrides_enabled")).Num(), 2);
	}

	TestEqual(TEXT("BloomIntensity landed"), PPVol->Settings.BloomIntensity, 0.75f, 0.0001f);
	TestTrue(TEXT("bOverride_BloomIntensity was enabled — without this the value is inert"),
		PPVol->Settings.bOverride_BloomIntensity != 0);
	TestTrue(TEXT("bOverride_VignetteIntensity was enabled"),
		PPVol->Settings.bOverride_VignetteIntensity != 0);

	// The caller can override the auto-override: an explicit false must be honoured.
	TSharedPtr<FJsonObject> Explicit = MakeShared<FJsonObject>();
	Explicit->SetNumberField(TEXT("BloomThreshold"), -0.5);
	Explicit->SetBoolField(TEXT("bOverride_BloomThreshold"), false);
	TSharedPtr<FJsonObject> ExplicitParams = MonolithAtmosphereTestUtils::MakeParams();
	ExplicitParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	ExplicitParams->SetObjectField(TEXT("properties"), Explicit);
	FMonolithActionResult Set2 = MonolithAtmosphereTestUtils::Exec(TEXT("set_atmosphere_properties"), ExplicitParams);
	if (TestTrue(FString::Printf(TEXT("explicit-override call succeeded (%s)"), *Set2.ErrorMessage), Set2.bSuccess))
	{
		TestEqual(TEXT("no bit was auto-injected when the caller named it"),
			Set2.Result->GetArrayField(TEXT("overrides_enabled")).Num(), 0);
	}
	TestEqual(TEXT("BloomThreshold value still landed"), PPVol->Settings.BloomThreshold, -0.5f, 0.0001f);
	TestFalse(TEXT("an explicit bOverride_ = false is honoured"), PPVol->Settings.bOverride_BloomThreshold != 0);

	// The read-back surfaces the override state alongside the values.
	TSharedPtr<FJsonObject> ReadParams = MonolithAtmosphereTestUtils::MakeParams();
	ReadParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	TArray<TSharedPtr<FJsonValue>> Names;
	Names.Add(MakeShared<FJsonValueString>(TEXT("BloomIntensity")));
	Names.Add(MakeShared<FJsonValueString>(TEXT("BloomThreshold")));
	ReadParams->SetArrayField(TEXT("properties"), Names);
	FMonolithActionResult Read = MonolithAtmosphereTestUtils::Exec(TEXT("get_atmosphere_properties"), ReadParams);
	if (TestTrue(FString::Printf(TEXT("read-back succeeded (%s)"), *Read.ErrorMessage), Read.bSuccess))
	{
		TestEqual(TEXT("BloomIntensity round-tripped"),
			Read.Result->GetObjectField(TEXT("properties"))->GetNumberField(TEXT("BloomIntensity")), 0.75, 0.0001);
		const TSharedPtr<FJsonObject> Overrides = Read.Result->GetObjectField(TEXT("overrides"));
		TestTrue(TEXT("overrides block reports the enabled bit"),
			Overrides->GetBoolField(TEXT("bOverride_BloomIntensity")));
		TestFalse(TEXT("overrides block reports the disabled bit"),
			Overrides->GetBoolField(TEXT("bOverride_BloomThreshold")));
	}

	MonolithAtmosphereTestUtils::Destroy(Actor);
	return true;
}

// ============================================================================
// 4. Unknown property name: clean error with a did-you-mean, and NOTHING written.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshAtmosphereUnknownPropertyTest,
	"Monolith.Mesh.Atmosphere.UnknownPropertyRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshAtmosphereUnknownPropertyTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("Editor world available"), MonolithAtmosphereTestUtils::GetTestWorld()))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Good = MakeShared<FJsonObject>();
	Good->SetNumberField(TEXT("FogDensity"), 0.05);

	FMonolithActionResult Spawn;
	AActor* Actor = MonolithAtmosphereTestUtils::Spawn(
		TEXT("height_fog"), TEXT("MonolithTestAtmos_BadProp"), Good, FString(), Spawn);
	if (!TestTrue(TEXT("baseline fog spawned"), Spawn.bSuccess) || !TestNotNull(TEXT("actor found"), Actor))
	{
		MonolithAtmosphereTestUtils::Destroy(Actor);
		return false;
	}

	// One valid key + one typo. The whole call must fail and change nothing.
	TSharedPtr<FJsonObject> Mixed = MakeShared<FJsonObject>();
	Mixed->SetNumberField(TEXT("FogDensity"), 0.99);
	Mixed->SetNumberField(TEXT("FogDensityy"), 1.0);

	TSharedPtr<FJsonObject> SetParams = MonolithAtmosphereTestUtils::MakeParams();
	SetParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	SetParams->SetObjectField(TEXT("properties"), Mixed);

	FMonolithActionResult Bad = MonolithAtmosphereTestUtils::Exec(TEXT("set_atmosphere_properties"), SetParams);
	TestFalse(TEXT("set_atmosphere_properties fails on an unknown property"), Bad.bSuccess);
	TestTrue(TEXT("error names the offending key"), Bad.ErrorMessage.Contains(TEXT("FogDensityy")));
	TestTrue(TEXT("error says the write was rejected"), Bad.ErrorMessage.Contains(TEXT("unknown field")));
	TestTrue(TEXT("error offers a did-you-mean candidate"), Bad.ErrorMessage.Contains(TEXT("did you mean")));

	UExponentialHeightFogComponent* Comp = Actor->FindComponentByClass<UExponentialHeightFogComponent>();
	if (TestNotNull(TEXT("fog component present"), Comp))
	{
		TestEqual(TEXT("no partial write — FogDensity is untouched"), Comp->FogDensity, 0.05f, 0.0001f);
	}

	// Reading an unknown property is an error too, not a silent omission.
	TSharedPtr<FJsonObject> ReadParams = MonolithAtmosphereTestUtils::MakeParams();
	ReadParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	TArray<TSharedPtr<FJsonValue>> Names;
	Names.Add(MakeShared<FJsonValueString>(TEXT("NoSuchProperty")));
	ReadParams->SetArrayField(TEXT("properties"), Names);
	FMonolithActionResult BadRead = MonolithAtmosphereTestUtils::Exec(TEXT("get_atmosphere_properties"), ReadParams);
	TestFalse(TEXT("get_atmosphere_properties fails on an unknown property"), BadRead.bSuccess);
	TestTrue(TEXT("read error names the offending key"), BadRead.ErrorMessage.Contains(TEXT("NoSuchProperty")));

	MonolithAtmosphereTestUtils::Destroy(Actor);
	return true;
}

// ============================================================================
// 5. Lumen: write through mesh.set_lumen_settings, read back through
//    mesh.get_lumen_settings — including the enum-valued method fields.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshAtmosphereLumenRoundTripTest,
	"Monolith.Mesh.Atmosphere.LumenRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshAtmosphereLumenRoundTripTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("Editor world available"), MonolithAtmosphereTestUtils::GetTestWorld()))
	{
		return false;
	}

	FMonolithActionResult Spawn;
	AActor* Actor = MonolithAtmosphereTestUtils::Spawn(
		TEXT("post_process"), TEXT("MonolithTestAtmos_Lumen"), nullptr, FString(), Spawn);
	if (!TestTrue(FString::Printf(TEXT("post-process volume spawned (%s)"), *Spawn.ErrorMessage), Spawn.bSuccess)
		|| !TestNotNull(TEXT("actor found"), Actor))
	{
		MonolithAtmosphereTestUtils::Destroy(Actor);
		return false;
	}
	APostProcessVolume* PPVol = Cast<APostProcessVolume>(Actor);

	TSharedPtr<FJsonObject> Lumen = MakeShared<FJsonObject>();
	Lumen->SetNumberField(TEXT("LumenSceneDetail"), 3.5);
	Lumen->SetNumberField(TEXT("LumenMaxTraceDistance"), 12345.0);
	Lumen->SetStringField(TEXT("DynamicGlobalIlluminationMethod"), TEXT("Lumen"));
	Lumen->SetStringField(TEXT("ReflectionMethod"), TEXT("Lumen"));

	TSharedPtr<FJsonObject> SetParams = MonolithAtmosphereTestUtils::MakeParams();
	SetParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	SetParams->SetObjectField(TEXT("properties"), Lumen);

	FMonolithActionResult Set = MonolithAtmosphereTestUtils::Exec(TEXT("set_lumen_settings"), SetParams);
	if (!TestTrue(FString::Printf(TEXT("set_lumen_settings succeeded (%s)"), *Set.ErrorMessage), Set.bSuccess))
	{
		MonolithAtmosphereTestUtils::Destroy(Actor);
		return false;
	}
	TestEqual(TEXT("four Lumen values applied"), Set.Result->GetArrayField(TEXT("properties_set")).Num(), 4);
	TestEqual(TEXT("four override bits auto-enabled"),
		Set.Result->GetArrayField(TEXT("overrides_enabled")).Num(), 4);

	if (PPVol)
	{
		TestEqual(TEXT("LumenSceneDetail landed"), PPVol->Settings.LumenSceneDetail, 3.5f, 0.0001f);
		TestEqual(TEXT("LumenMaxTraceDistance landed"), PPVol->Settings.LumenMaxTraceDistance, 12345.0f, 0.01f);
		TestTrue(TEXT("bOverride_LumenSceneDetail enabled"), PPVol->Settings.bOverride_LumenSceneDetail != 0);
		TestTrue(TEXT("bOverride_DynamicGlobalIlluminationMethod enabled"),
			PPVol->Settings.bOverride_DynamicGlobalIlluminationMethod != 0);
		TestEqual(TEXT("the GI method enum was written by name"),
			static_cast<int32>(PPVol->Settings.DynamicGlobalIlluminationMethod.GetValue()),
			static_cast<int32>(EDynamicGlobalIlluminationMethod::Lumen));
		TestEqual(TEXT("the reflection method enum was written by name"),
			static_cast<int32>(PPVol->Settings.ReflectionMethod.GetValue()),
			static_cast<int32>(EReflectionMethod::Lumen));
	}

	// Read it back through the action, by name.
	TSharedPtr<FJsonObject> ReadParams = MonolithAtmosphereTestUtils::MakeParams();
	ReadParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	FMonolithActionResult Read = MonolithAtmosphereTestUtils::Exec(TEXT("get_lumen_settings"), ReadParams);
	if (TestTrue(FString::Printf(TEXT("get_lumen_settings succeeded (%s)"), *Read.ErrorMessage), Read.bSuccess))
	{
		const TSharedPtr<FJsonObject> LumenOut = Read.Result->GetObjectField(TEXT("lumen"));
		TestEqual(TEXT("LumenSceneDetail read back"), LumenOut->GetNumberField(TEXT("LumenSceneDetail")), 3.5, 0.0001);
		TestEqual(TEXT("LumenMaxTraceDistance read back"),
			LumenOut->GetNumberField(TEXT("LumenMaxTraceDistance")), 12345.0, 0.01);
		TestTrue(TEXT("override state is reported alongside the values"),
			Read.Result->GetObjectField(TEXT("overrides"))->GetBoolField(TEXT("bOverride_LumenSceneDetail")));
		TestTrue(TEXT("the volume block names the volume"),
			Read.Result->GetObjectField(TEXT("volume"))->GetStringField(TEXT("actor_name"))
				== Actor->GetActorNameOrLabel());

		// The project block is a READ-ONLY diagnostic and must be present + populated.
		TestTrue(TEXT("project block present"), Read.Result->HasField(TEXT("project")));
		TestTrue(TEXT("project block is flagged read-only"),
			Read.Result->GetBoolField(TEXT("project_is_read_only")));
		TestTrue(TEXT("project block resolved at least one renderer setting"),
			Read.Result->GetObjectField(TEXT("project"))->Values.Num() > 0);
		TestFalse(TEXT("no project read-back key went unresolved"),
			Read.Result->HasField(TEXT("project_unresolved")));
	}

	// A non-Lumen post-process key must be refused, with the Lumen list in the message.
	TSharedPtr<FJsonObject> NotLumen = MakeShared<FJsonObject>();
	NotLumen->SetNumberField(TEXT("BloomIntensity"), 2.0);
	TSharedPtr<FJsonObject> BadParams = MonolithAtmosphereTestUtils::MakeParams();
	BadParams->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	BadParams->SetObjectField(TEXT("properties"), NotLumen);
	FMonolithActionResult Bad = MonolithAtmosphereTestUtils::Exec(TEXT("set_lumen_settings"), BadParams);
	TestFalse(TEXT("a non-Lumen key is refused by set_lumen_settings"), Bad.bSuccess);
	TestTrue(TEXT("refusal names the offending key"), Bad.ErrorMessage.Contains(TEXT("BloomIntensity")));
	TestTrue(TEXT("refusal points at the right action"),
		Bad.ErrorMessage.Contains(TEXT("mesh.set_atmosphere_properties")));
	if (PPVol)
	{
		TestFalse(TEXT("no partial write — bloom override untouched"),
			PPVol->Settings.bOverride_BloomIntensity != 0);
	}

	MonolithAtmosphereTestUtils::Destroy(Actor);
	return true;
}

// ============================================================================
// 6. The SHIPPED preset / read-back data is valid against this engine build.
//    Engine-upgrade canary: if Epic renames a property, this test goes red
//    before a user hits it.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshAtmospherePresetDataValidTest,
	"Monolith.Mesh.Atmosphere.PresetDataIsValid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshAtmospherePresetDataValidTest::RunTest(const FString& /*Parameters*/)
{
	TArray<FString> Warnings;
	const TMap<FString, FMonolithJsonPreset> Loaded =
		FMonolithMeshAtmosphereActions::Presets().LoadPresets(Warnings);

	TestEqual(FString::Printf(TEXT("no preset load warnings (%s)"), *FString::Join(Warnings, TEXT("; "))),
		Warnings.Num(), 0);
	TestTrue(TEXT("built-in presets loaded"), Loaded.Num() > 0);

	for (const TPair<FString, FMonolithJsonPreset>& Pair : Loaded)
	{
		const FMonolithJsonPreset& P = Pair.Value;

		UStruct* Struct = FMonolithMeshAtmosphereActions::ResolveSectionStruct(P.TypeToken);
		if (!TestNotNull(FString::Printf(TEXT("preset '%s' names a known type ('%s')"), *P.Name, *P.TypeToken), Struct))
		{
			continue;
		}
		TestNotEqual(FString::Printf(TEXT("preset '%s' does not target the read-only project section"), *P.Name),
			P.TypeToken, FString(TEXT("project")));

		if (!TestTrue(FString::Printf(TEXT("preset '%s' has properties"), *P.Name), P.Properties.IsValid()))
		{
			continue;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Prop : P.Properties->Values)
		{
			TestNotNull(
				FString::Printf(TEXT("preset '%s' property '%s' exists on %s"),
					*P.Name, *Prop.Key, *Struct->GetName()),
				FMonolithReflectionWalker::FindPropertyForwarding(Struct, Prop.Key));
		}
	}

	// Every read-back section must resolve, key by key, against its engine struct.
	for (const FString& Section : FMonolithMeshAtmosphereActions::GetSectionTokens())
	{
		UStruct* Struct = FMonolithMeshAtmosphereActions::ResolveSectionStruct(Section);
		if (!TestNotNull(FString::Printf(TEXT("section '%s' maps to a struct"), *Section), Struct))
		{
			continue;
		}
		TArray<FString> SectionWarnings;
		const TArray<FString> Keys = FMonolithMeshAtmosphereActions::LoadReadbackKeys(Section, SectionWarnings);
		TestTrue(FString::Printf(TEXT("read-back set for '%s' is non-empty"), *Section), Keys.Num() > 0);
		for (const FString& Key : Keys)
		{
			TestNotNull(
				FString::Printf(TEXT("read-back key '%s' exists on %s"), *Key, *Struct->GetName()),
				FMonolithReflectionWalker::FindPropertyForwarding(Struct, Key));
		}
	}

	// Every Lumen key must be override-gated — that assumption is what makes
	// set_lumen_settings actually take effect.
	TArray<FString> LumenWarnings;
	UStruct* PPSettings = FPostProcessSettings::StaticStruct();
	for (const FString& Key : FMonolithMeshAtmosphereActions::LoadReadbackKeys(TEXT("lumen"), LumenWarnings))
	{
		TestNotNull(
			FString::Printf(TEXT("Lumen key '%s' has a bOverride_ sibling"), *Key),
			FMonolithReflectionWalker::FindPropertyForwarding(
				PPSettings, FString::Printf(TEXT("bOverride_%s"), *Key)));
	}

	return true;
}

// ============================================================================
// 7. A preset applies end to end, with values that live only in JSON.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshAtmospherePresetAppliesTest,
	"Monolith.Mesh.Atmosphere.PresetApplies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshAtmospherePresetAppliesTest::RunTest(const FString& /*Parameters*/)
{
	if (!TestNotNull(TEXT("Editor world available"), MonolithAtmosphereTestUtils::GetTestWorld()))
	{
		return false;
	}

	// Pick a real preset out of the data rather than naming one in C++, so the test
	// keeps working if the shipped library is re-authored.
	TArray<FString> Warnings;
	const TMap<FString, FMonolithJsonPreset> Loaded =
		FMonolithMeshAtmosphereActions::Presets().LoadPresets(Warnings);

	const FMonolithJsonPreset* Chosen = nullptr;
	for (const TPair<FString, FMonolithJsonPreset>& Pair : Loaded)
	{
		if (Pair.Value.TypeToken == TEXT("height_fog") && Pair.Value.Properties.IsValid()
			&& Pair.Value.Properties->HasField(TEXT("FogDensity")))
		{
			Chosen = &Pair.Value;
			break;
		}
	}
	if (!TestNotNull(TEXT("a height_fog preset with a FogDensity exists in the data"), Chosen))
	{
		return false;
	}
	const double ExpectedDensity = Chosen->Properties->GetNumberField(TEXT("FogDensity"));

	FMonolithActionResult Spawn;
	AActor* Actor = MonolithAtmosphereTestUtils::Spawn(
		TEXT("height_fog"), TEXT("MonolithTestAtmos_Preset"), nullptr, Chosen->Name, Spawn);
	if (!TestTrue(FString::Printf(TEXT("spawn with preset '%s' succeeded (%s)"), *Chosen->Name, *Spawn.ErrorMessage),
			Spawn.bSuccess)
		|| !TestNotNull(TEXT("actor found"), Actor))
	{
		MonolithAtmosphereTestUtils::Destroy(Actor);
		return false;
	}

	UExponentialHeightFogComponent* Comp = Actor->FindComponentByClass<UExponentialHeightFogComponent>();
	if (TestNotNull(TEXT("fog component present"), Comp))
	{
		TestEqual(TEXT("preset FogDensity landed on the component"),
			static_cast<double>(Comp->FogDensity), ExpectedDensity, 0.0001);
	}

	MonolithAtmosphereTestUtils::Destroy(Actor);
	return true;
}

// ============================================================================
// 8. Error paths and the discovery surface.
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithMeshAtmosphereErrorPathsTest,
	"Monolith.Mesh.Atmosphere.ErrorsAreActionable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMeshAtmosphereErrorPathsTest::RunTest(const FString& /*Parameters*/)
{
	// --- Unknown type ---
	FString TypeError;
	TestNull(TEXT("an unknown atmosphere type does not resolve"),
		FMonolithMeshAtmosphereActions::ResolveAtmosphereActorClass(TEXT("banana"), TypeError));
	TestTrue(TEXT("unknown-type error lists the valid types"), TypeError.Contains(TEXT("height_fog")));
	TestTrue(TEXT("unknown-type error echoes the bad token"), TypeError.Contains(TEXT("banana")));

	// Aliases still resolve, so the error above is about a genuine miss.
	FString AliasError;
	TestNotNull(TEXT("'fog' resolves as an alias"),
		FMonolithMeshAtmosphereActions::ResolveAtmosphereActorClass(TEXT("fog"), AliasError));
	TestNotNull(TEXT("'PostProcessVolume' resolves as an alias"),
		FMonolithMeshAtmosphereActions::ResolveAtmosphereActorClass(TEXT("PostProcessVolume"), AliasError));
	TestNotNull(TEXT("'SkyAtmosphere' resolves as an alias"),
		FMonolithMeshAtmosphereActions::ResolveAtmosphereActorClass(TEXT("SkyAtmosphere"), AliasError));

	// --- No open level ---
	FString WorldError;
	TestFalse(TEXT("a null world is refused"),
		FMonolithMeshAtmosphereActions::RequireEditorWorld(nullptr, WorldError));
	TestTrue(TEXT("no-level error says what to do"), WorldError.Contains(TEXT("No open level")));

	// --- Unknown preset ---
	FMonolithJsonPreset Preset;
	FString PresetError;
	TestFalse(TEXT("an unknown preset is refused"),
		FMonolithMeshAtmosphereActions::Presets().ResolvePreset(TEXT("not_a_preset"), Preset, PresetError));
	TestTrue(TEXT("unknown-preset error lists the available names"),
		PresetError.Contains(TEXT("Available presets")));

	// --- Preset/target compatibility ---
	TestTrue(TEXT("a lumen preset is valid on a post-process volume"),
		FMonolithMeshAtmosphereActions::IsPresetCompatible(TEXT("lumen"), TEXT("post_process")));
	TestFalse(TEXT("a lumen preset is NOT valid on fog"),
		FMonolithMeshAtmosphereActions::IsPresetCompatible(TEXT("lumen"), TEXT("height_fog")));
	TestFalse(TEXT("a fog preset is NOT valid on a sky atmosphere"),
		FMonolithMeshAtmosphereActions::IsPresetCompatible(TEXT("height_fog"), TEXT("sky_atmosphere")));

	// --- Non-atmosphere actor ---
	UWorld* World = MonolithAtmosphereTestUtils::GetTestWorld();
	if (TestNotNull(TEXT("Editor world available"), World))
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		FVector Loc = FVector::ZeroVector;
		FRotator Rot = FRotator::ZeroRotator;
		AActor* Plain = World->SpawnActor(AActor::StaticClass(), &Loc, &Rot, SpawnParams);
		if (TestNotNull(TEXT("plain actor spawned"), Plain))
		{
			FMonolithMeshAtmosphereActions::FTarget Target;
			FString TargetError;
			TestFalse(TEXT("a plain actor is not an atmosphere target"),
				FMonolithMeshAtmosphereActions::ResolveTarget(Plain, Target, TargetError));
			TestTrue(TEXT("non-atmosphere error suggests spawn_atmosphere"),
				TargetError.Contains(TEXT("spawn_atmosphere")));
			MonolithAtmosphereTestUtils::Destroy(Plain);
		}
	}

	// --- Type-mismatched preset on a real actor (fog recipe on a sky atmosphere) ---
	TArray<FString> Warnings;
	const TMap<FString, FMonolithJsonPreset> Loaded =
		FMonolithMeshAtmosphereActions::Presets().LoadPresets(Warnings);
	FString FogPreset;
	for (const TPair<FString, FMonolithJsonPreset>& Pair : Loaded)
	{
		if (Pair.Value.TypeToken == TEXT("height_fog")) { FogPreset = Pair.Key; break; }
	}
	if (!FogPreset.IsEmpty() && World)
	{
		FMonolithActionResult Result;
		AActor* Actor = MonolithAtmosphereTestUtils::Spawn(
			TEXT("sky_atmosphere"), TEXT("MonolithTestAtmos_Mismatch"), nullptr, FogPreset, Result);
		TestFalse(TEXT("a fog preset is refused on a sky atmosphere"), Result.bSuccess);
		TestTrue(TEXT("mismatch error names the preset"), Result.ErrorMessage.Contains(FogPreset));
		TestNull(TEXT("the failed spawn left no actor behind"), Actor);
		MonolithAtmosphereTestUtils::Destroy(Actor);
	}

	// --- Discovery surface ---
	TSharedPtr<FJsonObject> ListParams = MonolithAtmosphereTestUtils::MakeParams();
	FMonolithActionResult All = MonolithAtmosphereTestUtils::Exec(TEXT("list_atmosphere_presets"), ListParams);
	if (TestTrue(FString::Printf(TEXT("list_atmosphere_presets succeeded (%s)"), *All.ErrorMessage), All.bSuccess))
	{
		const double TotalCount = All.Result->GetNumberField(TEXT("total_count"));
		TestTrue(TEXT("presets were listed"), TotalCount > 0.0);
		TestEqual(TEXT("unfiltered count equals total"), All.Result->GetNumberField(TEXT("count")), TotalCount);
		TestEqual(TEXT("atmosphere_types is advertised (3 spawnable + lumen)"),
			All.Result->GetArrayField(TEXT("atmosphere_types")).Num(), 4);

		TSharedPtr<FJsonObject> Filtered = MonolithAtmosphereTestUtils::MakeParams();
		Filtered->SetStringField(TEXT("type"), TEXT("lumen"));
		FMonolithActionResult Lumen = MonolithAtmosphereTestUtils::Exec(TEXT("list_atmosphere_presets"), Filtered);
		if (TestTrue(FString::Printf(TEXT("filtered list succeeded (%s)"), *Lumen.ErrorMessage), Lumen.bSuccess))
		{
			const TArray<TSharedPtr<FJsonValue>>& Arr = Lumen.Result->GetArrayField(TEXT("presets"));
			TestTrue(TEXT("at least one lumen preset ships"), Arr.Num() > 0);
			TestTrue(TEXT("filter narrowed the list"), Arr.Num() < static_cast<int32>(TotalCount));
			for (const TSharedPtr<FJsonValue>& V : Arr)
			{
				TestEqual(TEXT("every filtered preset is a lumen preset"),
					V->AsObject()->GetStringField(TEXT("type")), FString(TEXT("lumen")));
			}
		}
	}

	TSharedPtr<FJsonObject> BadFilter = MonolithAtmosphereTestUtils::MakeParams();
	BadFilter->SetStringField(TEXT("type"), TEXT("banana"));
	TestFalse(TEXT("an unknown type filter is refused"),
		MonolithAtmosphereTestUtils::Exec(TEXT("list_atmosphere_presets"), BadFilter).bSuccess);

	TSharedPtr<FJsonObject> ProjectFilter = MonolithAtmosphereTestUtils::MakeParams();
	ProjectFilter->SetStringField(TEXT("type"), TEXT("project"));
	TestFalse(TEXT("the read-only project section is not a preset filter"),
		MonolithAtmosphereTestUtils::Exec(TEXT("list_atmosphere_presets"), ProjectFilter).bSuccess);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
