#include "MonolithMeshAtmosphereActions.h"

#include "MonolithMeshUtils.h"
#include "MonolithMeshLightActions.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "Reflection/MonolithReflectionReader.h"

#include "Engine/World.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/Scene.h"
#include "Engine/RendererSettings.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/ActorComponent.h"
#include "Components/BrushComponent.h"
#include "Builders/CubeBuilder.h"
#include "ActorFactories/ActorFactory.h"
#include "GameFramework/Actor.h"
#include "EngineUtils.h"
#include "Editor.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"

// ============================================================================
// Type table
//
// Token -> actor/component class is the ONE mapping that cannot live in data:
// a JSON file cannot name a C++ UClass without a string lookup that is strictly
// worse than this table (no compile-time checking, no IWYU include). Everything
// else — presets, defaults, which properties to report — is data.
// ============================================================================

namespace MonolithAtmosphereTypes
{
	struct FEntry
	{
		const TCHAR* Token;
		UClass* ActorClass;
		/** null for post_process: its settings are a struct member, not a component. */
		UClass* ComponentClass;
	};

	static const TArray<FEntry>& Table()
	{
		// Built once, after UClasses exist. Static-local init is thread-safe and
		// deferred, which matters because this file's statics are constructed before
		// StaticClass() is safe at module-load time.
		static const TArray<FEntry> Entries = {
			{ TEXT("post_process"),   APostProcessVolume::StaticClass(),    nullptr                                     },
			{ TEXT("height_fog"),     AExponentialHeightFog::StaticClass(), UExponentialHeightFogComponent::StaticClass() },
			{ TEXT("sky_atmosphere"), ASkyAtmosphere::StaticClass(),        USkyAtmosphereComponent::StaticClass()       },
		};
		return Entries;
	}

	/** Alias -> canonical token. Kept next to the class table for the same reason. */
	struct FAlias { const TCHAR* Alias; const TCHAR* Token; };

	static const TArray<FAlias>& Aliases()
	{
		static const TArray<FAlias> Entries = {
			{ TEXT("postprocess"),                 TEXT("post_process")   },
			{ TEXT("postprocessvolume"),           TEXT("post_process")   },
			{ TEXT("apostprocessvolume"),          TEXT("post_process")   },
			{ TEXT("ppv"),                         TEXT("post_process")   },
			{ TEXT("heightfog"),                   TEXT("height_fog")     },
			{ TEXT("fog"),                         TEXT("height_fog")     },
			{ TEXT("exponentialheightfog"),        TEXT("height_fog")     },
			{ TEXT("aexponentialheightfog"),       TEXT("height_fog")     },
			{ TEXT("exponentialheightfogcomponent"),TEXT("height_fog")    },
			{ TEXT("skyatmosphere"),               TEXT("sky_atmosphere") },
			{ TEXT("askyatmosphere"),              TEXT("sky_atmosphere") },
			{ TEXT("skyatmospherecomponent"),      TEXT("sky_atmosphere") },
			{ TEXT("atmosphere"),                  TEXT("sky_atmosphere") },
		};
		return Entries;
	}

	/** Lowercase, drop separators; then map through the alias table. */
	static FString Normalise(const FString& In)
	{
		FString S = In.TrimStartAndEnd().ToLower();
		S.ReplaceInline(TEXT(" "), TEXT(""));
		S.ReplaceInline(TEXT("_"), TEXT(""));
		S.ReplaceInline(TEXT("-"), TEXT(""));

		for (const FEntry& E : Table())
		{
			FString Canonical(E.Token);
			Canonical.ReplaceInline(TEXT("_"), TEXT(""));
			if (S == Canonical)
			{
				return E.Token;
			}
		}
		for (const FAlias& A : Aliases())
		{
			if (S == A.Alias)
			{
				return A.Token;
			}
		}
		return S;
	}
}

const TArray<FString>& FMonolithMeshAtmosphereActions::GetAtmosphereTokens()
{
	static TArray<FString> Tokens;
	if (Tokens.Num() == 0)
	{
		for (const MonolithAtmosphereTypes::FEntry& E : MonolithAtmosphereTypes::Table())
		{
			Tokens.Add(E.Token);
		}
	}
	return Tokens;
}

const TArray<FString>& FMonolithMeshAtmosphereActions::GetSectionTokens()
{
	static TArray<FString> Sections;
	if (Sections.Num() == 0)
	{
		Sections = GetAtmosphereTokens();
		Sections.Add(TEXT("lumen"));
		Sections.Add(TEXT("project"));
	}
	return Sections;
}

UClass* FMonolithMeshAtmosphereActions::ResolveAtmosphereActorClass(const FString& Token, FString& OutError)
{
	OutError.Reset();
	if (Token.TrimStartAndEnd().IsEmpty())
	{
		OutError = FString::Printf(TEXT("Missing atmosphere type. Valid types: %s."),
			*FString::Join(GetAtmosphereTokens(), TEXT(", ")));
		return nullptr;
	}

	const FString Norm = MonolithAtmosphereTypes::Normalise(Token);
	for (const MonolithAtmosphereTypes::FEntry& E : MonolithAtmosphereTypes::Table())
	{
		if (Norm == E.Token)
		{
			return E.ActorClass;
		}
	}

	OutError = FString::Printf(TEXT("Unknown atmosphere type '%s'. Valid types: %s."),
		*Token, *FString::Join(GetAtmosphereTokens(), TEXT(", ")));
	return nullptr;
}

UStruct* FMonolithMeshAtmosphereActions::ResolveSectionStruct(const FString& Token)
{
	const FString Norm = MonolithAtmosphereTypes::Normalise(Token);

	// post_process and lumen are two curated views of the SAME struct.
	if (Norm == TEXT("post_process") || Norm == TEXT("lumen"))
	{
		return FPostProcessSettings::StaticStruct();
	}
	if (Norm == TEXT("project"))
	{
		return URendererSettings::StaticClass();
	}
	for (const MonolithAtmosphereTypes::FEntry& E : MonolithAtmosphereTypes::Table())
	{
		if (Norm == E.Token)
		{
			return E.ComponentClass;
		}
	}
	return nullptr;
}

FString FMonolithMeshAtmosphereActions::TokenForActor(const AActor* Actor)
{
	if (!Actor)
	{
		return FString();
	}
	for (const MonolithAtmosphereTypes::FEntry& E : MonolithAtmosphereTypes::Table())
	{
		if (Actor->IsA(E.ActorClass))
		{
			return E.Token;
		}
	}
	return FString();
}

bool FMonolithMeshAtmosphereActions::RequireEditorWorld(const UWorld* World, FString& OutError)
{
	if (World)
	{
		OutError.Reset();
		return true;
	}
	OutError = TEXT("No open level — the editor has no world. Open or create a level "
					"(editor.open_level / editor.new_level) before spawning or editing atmosphere actors.");
	return false;
}

bool FMonolithMeshAtmosphereActions::ResolveTarget(AActor* Actor, FTarget& OutTarget, FString& OutError)
{
	OutError.Reset();
	OutTarget = FTarget();

	if (!Actor)
	{
		OutError = TEXT("No actor supplied.");
		return false;
	}

	const FString Token = TokenForActor(Actor);
	if (Token.IsEmpty())
	{
		OutError = FString::Printf(
			TEXT("Actor '%s' (%s) is not an atmosphere actor. Supported types: %s. "
				 "Spawn one with mesh.spawn_atmosphere."),
			*Actor->GetActorNameOrLabel(), *Actor->GetClass()->GetName(),
			*FString::Join(GetAtmosphereTokens(), TEXT(", ")));
		return false;
	}

	OutTarget.Token = Token;
	OutTarget.Actor = Actor;

	if (APostProcessVolume* PPVol = Cast<APostProcessVolume>(Actor))
	{
		// The settings are a struct MEMBER of the actor, not a component.
		FProperty* SettingsProp = FMonolithReflectionWalker::FindPropertyForwarding(
			APostProcessVolume::StaticClass(), TEXT("Settings"));
		if (!SettingsProp)
		{
			OutError = TEXT("APostProcessVolume has no reflected 'Settings' property on this engine build — "
							"the post-process surface cannot be used. This is an engine-version mismatch.");
			return false;
		}
		OutTarget.CradleObject = PPVol;
		OutTarget.Struct = FPostProcessSettings::StaticStruct();
		OutTarget.Container = &PPVol->Settings;
		OutTarget.OuterStructProp = SettingsProp;
		OutTarget.bOverrideAware = true;
		return true;
	}

	UClass* CompClass = nullptr;
	for (const MonolithAtmosphereTypes::FEntry& E : MonolithAtmosphereTypes::Table())
	{
		if (Token == E.Token) { CompClass = E.ComponentClass; break; }
	}
	if (!CompClass)
	{
		OutError = FString::Printf(TEXT("Internal: no component class registered for atmosphere type '%s'."), *Token);
		return false;
	}

	UActorComponent* Comp = Actor->FindComponentByClass(CompClass);
	if (!Comp)
	{
		OutError = FString::Printf(
			TEXT("Actor '%s' (%s) has no %s. It looks like a %s actor but its component is missing or was removed."),
			*Actor->GetActorNameOrLabel(), *Actor->GetClass()->GetName(), *CompClass->GetName(), *Token);
		return false;
	}

	OutTarget.CradleObject = Comp;
	OutTarget.Struct = Comp->GetClass();
	OutTarget.Container = Comp;
	return true;
}

// ============================================================================
// Data-driven preset library
// ============================================================================

const FMonolithMeshJsonPresets& FMonolithMeshAtmosphereActions::Presets()
{
	static const FMonolithMeshJsonPresets Library(
		TEXT("MonolithAtmospherePresets.json"), TEXT("AtmospherePresets"), TEXT("atmosphere preset"));
	return Library;
}

TArray<FString> FMonolithMeshAtmosphereActions::LoadReadbackKeys(const FString& SectionToken, TArray<FString>& OutWarnings)
{
	return Presets().LoadReadbackKeys(MonolithAtmosphereTypes::Normalise(SectionToken), OutWarnings);
}

bool FMonolithMeshAtmosphereActions::IsPresetCompatible(const FString& PresetToken, const FString& TargetToken)
{
	if (PresetToken.IsEmpty())
	{
		return true; // untyped preset applies anywhere
	}
	const FString P = MonolithAtmosphereTypes::Normalise(PresetToken);
	const FString T = MonolithAtmosphereTypes::Normalise(TargetToken);
	if (P == T)
	{
		return true;
	}
	// Lumen presets write FPostProcessSettings, so they are post-process presets.
	return P == TEXT("lumen") && T == TEXT("post_process");
}

// ============================================================================
// Shared helpers
// ============================================================================

namespace MonolithAtmosphereHelpers
{
	/** Scoped undo transaction — same shape as the other mesh action files. */
	struct FScopedMeshTransaction
	{
		bool bOwnsTransaction;

		explicit FScopedMeshTransaction(const FText& Description)
			: bOwnsTransaction(true)
		{
			if (GEditor)
			{
				GEditor->BeginTransaction(Description);
			}
		}

		~FScopedMeshTransaction()
		{
			if (bOwnsTransaction && GEditor)
			{
				GEditor->EndTransaction();
			}
		}

		void Cancel()
		{
			if (bOwnsTransaction && GEditor)
			{
				GEditor->CancelTransaction(0);
				bOwnsTransaction = false;
			}
		}
	};

	static TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}

	static TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FString& S : In) { Arr.Add(MakeShared<FJsonValueString>(S)); }
		return Arr;
	}

	/** Name of the override bit that gates a FPostProcessSettings field. */
	static FString OverrideNameFor(const FString& Key)
	{
		return FString::Printf(TEXT("bOverride_%s"), *Key);
	}

	/** Read a set of property names off a container into a JSON object. */
	static void ReadKeys(
		UStruct* Struct, const void* Container, const UObject* Owner,
		const TArray<FString>& Names,
		const TSharedPtr<FJsonObject>& OutProps,
		TArray<FString>& OutReported,
		TArray<FString>& OutMissing)
	{
		for (const FString& Name : Names)
		{
			FProperty* Prop = FMonolithReflectionWalker::FindPropertyForwarding(Struct, Name);
			if (!Prop)
			{
				OutMissing.Add(Name);
				continue;
			}
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Container);
			OutProps->SetField(Prop->GetName(), FMonolithReflectionReader::PropertyToJsonValue(Prop, ValuePtr, Owner));
			OutReported.Add(Prop->GetName());
		}
	}

	/** For each reported key that has a bOverride_ sibling, report that bit's state. */
	static TSharedPtr<FJsonObject> ReadOverrides(
		UStruct* Struct, const void* Container, const UObject* Owner, const TArray<FString>& Names)
	{
		auto Obj = MakeShared<FJsonObject>();
		for (const FString& Name : Names)
		{
			const FString OverrideName = OverrideNameFor(Name);
			FProperty* Prop = FMonolithReflectionWalker::FindPropertyForwarding(Struct, OverrideName);
			if (!Prop)
			{
				continue;
			}
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Container);
			Obj->SetField(Prop->GetName(), FMonolithReflectionReader::PropertyToJsonValue(Prop, ValuePtr, Owner));
		}
		return Obj;
	}
}

// ============================================================================
// Reflection write path
// ============================================================================

bool FMonolithMeshAtmosphereActions::ApplyPropertyTree(
	const FTarget& Target,
	const TSharedPtr<FJsonObject>& Params,
	const FString& PresetField,
	const FString& PropertiesField,
	const TArray<FString>& RestrictToKeys,
	TArray<FString>& OutApplied,
	TArray<FString>& OutOverrides,
	FString& OutError)
{
	OutApplied.Reset();
	OutOverrides.Reset();
	OutError.Reset();

	if (!Target.Struct || !Target.Container || !Target.CradleObject)
	{
		OutError = TEXT("No atmosphere target supplied.");
		return false;
	}
	if (!Params.IsValid())
	{
		return true;
	}

	// --- Merge preset (if any) then the explicit properties object over it. ---
	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();

	FString PresetName;
	if (Params->TryGetStringField(PresetField, PresetName) && !PresetName.TrimStartAndEnd().IsEmpty())
	{
		FMonolithJsonPreset Preset;
		if (!Presets().ResolvePreset(PresetName.TrimStartAndEnd(), Preset, OutError))
		{
			OutError += TEXT(" Run mesh.list_atmosphere_presets to see them with their types and property sets.");
			return false;
		}
		if (!IsPresetCompatible(Preset.TypeToken, Target.Token))
		{
			OutError = FString::Printf(
				TEXT("Preset '%s' targets '%s' but '%s' is a %s actor. "
					 "Pick a preset of the right type (mesh.list_atmosphere_presets type=%s)."),
				*Preset.Name, *Preset.TypeToken, *Target.Actor->GetActorNameOrLabel(),
				*Target.Token, *Target.Token);
			return false;
		}
		if (Preset.Properties.IsValid())
		{
			Tree->Values = Preset.Properties->Values;
		}
	}

	const TSharedPtr<FJsonObject>* Explicit = nullptr;
	if (Params->TryGetObjectField(PropertiesField, Explicit) && Explicit && (*Explicit).IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Explicit)->Values)
		{
			Tree->Values.Add(Pair.Key, Pair.Value);
		}
	}
	else if (Params->HasField(PropertiesField) && !Params->HasTypedField<EJson::Object>(PropertiesField))
	{
		OutError = FString::Printf(
			TEXT("Param '%s' must be a JSON object of {UPROPERTY name: value}, e.g. {\"FogDensity\": 0.02}."),
			*PropertiesField);
		return false;
	}

	if (Tree->Values.Num() == 0)
	{
		return true; // nothing asked for — not an error
	}

	// --- Optional key restriction (set_lumen_settings). Runs BEFORE anything is
	//     written and before the override bits are injected, so the caller sees a
	//     message about the key they actually typed. ---
	if (RestrictToKeys.Num() > 0)
	{
		TArray<FString> Refused;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Tree->Values)
		{
			FString Bare = Pair.Key;
			if (Bare.StartsWith(TEXT("bOverride_"), ESearchCase::IgnoreCase))
			{
				Bare = Bare.RightChop(10);
			}
			const bool bAllowed = RestrictToKeys.ContainsByPredicate(
				[&Bare](const FString& K) { return K.Equals(Bare, ESearchCase::IgnoreCase); });
			if (!bAllowed)
			{
				Refused.Add(Pair.Key);
			}
		}
		if (Refused.Num() > 0)
		{
			OutError = FString::Printf(
				TEXT("%d propert%s outside the Lumen set — nothing was applied: %s. "
					 "Lumen settings are: %s. Use mesh.set_atmosphere_properties for any other "
					 "post-process setting on the same volume."),
				Refused.Num(), Refused.Num() == 1 ? TEXT("y is") : TEXT("ies are"),
				*FString::Join(Refused, TEXT(", ")), *FString::Join(RestrictToKeys, TEXT(", ")));
			return false;
		}
	}

	// --- Override bits. A FPostProcessSettings field is INERT unless its
	//     bOverride_<Field> bit is set, so a plain reflection write "succeeds" and
	//     changes nothing on screen. Inject the bits the caller did not name
	//     explicitly, then let the SAME validated walk write them. ---
	TArray<FString> UserKeys;
	Tree->Values.GetKeys(UserKeys);
	TArray<FString> InjectedOverrides;
	if (Target.bOverrideAware)
	{
		for (const FString& Key : UserKeys)
		{
			if (Key.StartsWith(TEXT("bOverride_"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			const FString OverrideName = MonolithAtmosphereHelpers::OverrideNameFor(Key);
			if (Tree->Values.Contains(OverrideName))
			{
				continue; // caller decided for themselves
			}
			FProperty* OverrideProp = FMonolithReflectionWalker::FindPropertyForwarding(Target.Struct, OverrideName);
			if (!OverrideProp)
			{
				continue; // not every field is override-gated
			}
			Tree->Values.Add(OverrideProp->GetName(), MakeShared<FJsonValueBoolean>(true));
			InjectedOverrides.Add(OverrideProp->GetName());
		}
	}

	// --- Validate BEFORE mutating. InspectTree runs the exact same coercion the
	//     real write does, against scratch buffers, so a single bad key cannot leave
	//     the volume half-configured. ---
	FBulkFillSpec Spec;
	Spec.TargetNamespace = TEXT("mesh");
	Spec.Tree = Tree;
	Spec.bStrict = true;

	FDryRunReport DryRun = FMonolithReflectionWalker::InspectTree(Tree, Target.Struct, Target.Container, Spec);
	if (DryRun.Errors > 0)
	{
		TArray<FString> Problems;
		for (const FBulkFillFieldWrite& W : DryRun.FieldWrites)
		{
			if (W.bOk)
			{
				continue;
			}
			FString Line = FString::Printf(TEXT("'%s': %s"), *W.Path, *W.Reason);
			const TArray<FString> Hints = FMonolithMeshLightActions::SuggestPropertyNames(Target.Struct, W.Path);
			if (W.Reason.Contains(TEXT("unknown field")) && Hints.Num() > 0)
			{
				Line += FString::Printf(TEXT(" (did you mean: %s?)"), *FString::Join(Hints, TEXT(", ")));
			}
			Problems.Add(Line);
		}
		OutError = FString::Printf(
			TEXT("%d atmosphere property write(s) rejected on %s — nothing was applied. %s. "
				 "Use mesh.get_atmosphere_properties with all=true to see the exact UPROPERTY names "
				 "this target supports."),
			Problems.Num(), *Target.Struct->GetName(), *FString::Join(Problems, TEXT("; ")));
		return false;
	}

	// --- Commit, inside the edit cradle. For a struct-member target the cradle
	//     fires once on the OWNING property (the inner FProperties belong to the
	//     struct, not to the actor's class, so they cannot be passed to
	//     PreEditChange on the actor). ---
	UObject* Cradle = Target.CradleObject;
	Cradle->Modify();

	TArray<FProperty*> Touched;
	if (Target.OuterStructProp)
	{
#if WITH_EDITOR
		Cradle->PreEditChange(Target.OuterStructProp);
#endif
	}
	else
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Tree->Values)
		{
			if (FProperty* Prop = FMonolithReflectionWalker::FindPropertyForwarding(Target.Struct, Pair.Key))
			{
				Touched.Add(Prop);
#if WITH_EDITOR
				Cradle->PreEditChange(Prop);
#endif
			}
		}
	}

	FDryRunReport Report = FMonolithReflectionWalker::WriteTree(Tree, Target.Struct, Target.Container, Cradle, Spec);

#if WITH_EDITOR
	if (Target.OuterStructProp)
	{
		FPropertyChangedEvent Event(Target.OuterStructProp);
		Cradle->PostEditChangeProperty(Event);
	}
	else
	{
		for (FProperty* Prop : Touched)
		{
			FPropertyChangedEvent Event(Prop);
			Cradle->PostEditChangeProperty(Event);
		}
	}
#endif
	if (UActorComponent* AsComponent = Cast<UActorComponent>(Cradle))
	{
		AsComponent->MarkRenderStateDirty();
	}
	if (Target.Actor)
	{
		Target.Actor->MarkPackageDirty();
	}

	if (Report.Errors > 0)
	{
		// The dry run passed, so the live write disagreed with the scratch write —
		// surface it rather than silently reporting success.
		TArray<FString> Problems;
		for (const FBulkFillFieldWrite& W : Report.FieldWrites)
		{
			if (!W.bOk)
			{
				Problems.Add(FString::Printf(TEXT("'%s': %s"), *W.Path, *W.Reason));
			}
		}
		OutError = FString::Printf(
			TEXT("%d atmosphere property write(s) failed after validation passed on %s: %s"),
			Problems.Num(), *Target.Struct->GetName(), *FString::Join(Problems, TEXT("; ")));
		return false;
	}

	for (const FBulkFillFieldWrite& W : Report.FieldWrites)
	{
		if (!W.bOk || W.Path.Contains(TEXT(".")) || W.Path.Contains(TEXT("[")))
		{
			continue;
		}
		if (InjectedOverrides.Contains(W.Path))
		{
			OutOverrides.AddUnique(W.Path);
		}
		else
		{
			OutApplied.AddUnique(W.Path);
		}
	}
	return true;
}

bool FMonolithMeshAtmosphereActions::ApplyActorPropertyTree(
	AActor* Actor,
	const TSharedPtr<FJsonObject>& Params,
	TArray<FString>& OutApplied,
	FString& OutError)
{
	OutApplied.Reset();
	OutError.Reset();

	if (!Actor || !Params.IsValid())
	{
		return true;
	}

	const TSharedPtr<FJsonObject>* Explicit = nullptr;
	if (!Params->TryGetObjectField(TEXT("actor_properties"), Explicit) || !Explicit || !(*Explicit).IsValid())
	{
		if (Params->HasField(TEXT("actor_properties")) && !Params->HasTypedField<EJson::Object>(TEXT("actor_properties")))
		{
			OutError = TEXT("Param 'actor_properties' must be a JSON object of {UPROPERTY name: value}, "
							"e.g. {\"bUnbound\": true, \"Priority\": 1}.");
			return false;
		}
		return true;
	}

	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	Tree->Values = (*Explicit)->Values;
	if (Tree->Values.Num() == 0)
	{
		return true;
	}

	UClass* ActorClass = Actor->GetClass();
	FBulkFillSpec Spec;
	Spec.TargetNamespace = TEXT("mesh");
	Spec.Tree = Tree;
	Spec.bStrict = true;

	FDryRunReport DryRun = FMonolithReflectionWalker::InspectTree(Tree, ActorClass, Actor, Spec);
	if (DryRun.Errors > 0)
	{
		TArray<FString> Problems;
		for (const FBulkFillFieldWrite& W : DryRun.FieldWrites)
		{
			if (W.bOk) { continue; }
			FString Line = FString::Printf(TEXT("'%s': %s"), *W.Path, *W.Reason);
			const TArray<FString> Hints = FMonolithMeshLightActions::SuggestPropertyNames(ActorClass, W.Path);
			if (W.Reason.Contains(TEXT("unknown field")) && Hints.Num() > 0)
			{
				Line += FString::Printf(TEXT(" (did you mean: %s?)"), *FString::Join(Hints, TEXT(", ")));
			}
			Problems.Add(Line);
		}
		OutError = FString::Printf(
			TEXT("%d actor property write(s) rejected on %s — nothing was applied. %s."),
			Problems.Num(), *ActorClass->GetName(), *FString::Join(Problems, TEXT("; ")));
		return false;
	}

	Actor->Modify();
	TArray<FProperty*> Touched;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Tree->Values)
	{
		if (FProperty* Prop = FMonolithReflectionWalker::FindPropertyForwarding(ActorClass, Pair.Key))
		{
			Touched.Add(Prop);
#if WITH_EDITOR
			Actor->PreEditChange(Prop);
#endif
		}
	}

	FDryRunReport Report = FMonolithReflectionWalker::WriteTree(Tree, ActorClass, Actor, Actor, Spec);

#if WITH_EDITOR
	for (FProperty* Prop : Touched)
	{
		FPropertyChangedEvent Event(Prop);
		Actor->PostEditChangeProperty(Event);
	}
#endif
	Actor->MarkPackageDirty();

	if (Report.Errors > 0)
	{
		TArray<FString> Problems;
		for (const FBulkFillFieldWrite& W : Report.FieldWrites)
		{
			if (!W.bOk) { Problems.Add(FString::Printf(TEXT("'%s': %s"), *W.Path, *W.Reason)); }
		}
		OutError = FString::Printf(TEXT("%d actor property write(s) failed after validation passed on %s: %s"),
			Problems.Num(), *ActorClass->GetName(), *FString::Join(Problems, TEXT("; ")));
		return false;
	}

	for (const FBulkFillFieldWrite& W : Report.FieldWrites)
	{
		if (W.bOk && !W.Path.Contains(TEXT(".")) && !W.Path.Contains(TEXT("[")))
		{
			OutApplied.AddUnique(W.Path);
		}
	}
	return true;
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshAtmosphereActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("mesh"), TEXT("spawn_atmosphere"),
		TEXT("Spawn and configure an atmosphere actor: post_process (APostProcessVolume), "
			 "height_fog (AExponentialHeightFog) or sky_atmosphere (ASkyAtmosphere). "
			 "'preset' applies a data-driven recipe (mesh.list_atmosphere_presets); 'properties' sets ANY "
			 "UPROPERTY on the target settings by name via reflection. For a post-process volume the target is "
			 "APostProcessVolume::Settings and each written key also enables its bOverride_ bit, which is what "
			 "makes the change actually visible."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAtmosphereActions::SpawnAtmosphere),
		FParamSchemaBuilder()
			.Required(TEXT("type"), TEXT("string"), TEXT("Atmosphere type: post_process, height_fog, sky_atmosphere"))
			.Optional(TEXT("location"), TEXT("array"), TEXT("World location [x, y, z]"), TEXT("[0,0,0]"))
			.Optional(TEXT("rotation"), TEXT("array"), TEXT("Rotation [pitch, yaw, roll]"), TEXT("[0,0,0]"))
			.Optional(TEXT("extent"), TEXT("array"), TEXT("post_process only — brush half-extents [x, y, z]"), TEXT("[500,500,300]"))
			.Optional(TEXT("preset"), TEXT("string"), TEXT("Named data-driven preset applied before 'properties' (see mesh.list_atmosphere_presets)"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("UPROPERTY name -> value on the settings target, e.g. {\"FogDensity\": 0.02}. Unknown/mistyped names fail the whole call with no partial write."))
			.Optional(TEXT("actor_properties"), TEXT("object"), TEXT("UPROPERTY name -> value on the ACTOR itself, e.g. {\"bUnbound\": true, \"Priority\": 1, \"BlendRadius\": 100} for a post-process volume."))
			.Optional(TEXT("name"), TEXT("string"), TEXT("Actor label"))
			.OptionalAssetPath(TEXT("folder"), TEXT("Outliner folder path"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("set_atmosphere_properties"),
		TEXT("Modify an existing post_process / height_fog / sky_atmosphere actor. 'preset' applies a "
			 "data-driven recipe; 'properties' sets ANY UPROPERTY on its settings target by name via "
			 "reflection; 'actor_properties' targets the actor's own UPROPERTYs. On a post-process volume "
			 "the matching bOverride_ bits are enabled automatically."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAtmosphereActions::SetAtmosphereProperties),
		FParamSchemaBuilder()
			.Required(TEXT("actor_name"), TEXT("string"), TEXT("Atmosphere actor name or label"))
			.Optional(TEXT("preset"), TEXT("string"), TEXT("Named data-driven preset applied before 'properties'"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("UPROPERTY name -> value on the settings target. Unknown/mistyped names fail the whole call with no partial write."))
			.Optional(TEXT("actor_properties"), TEXT("object"), TEXT("UPROPERTY name -> value on the actor itself"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("get_atmosphere_properties"),
		TEXT("Read an atmosphere actor's settings back. Returns the data-driven default set for its type, "
			 "or exactly the properties you name. For a post-process volume the bOverride_ state of every "
			 "reported key comes back in 'overrides' — a value with its override off is inert. "
			 "Unknown property names are an error with did-you-mean candidates, not a silent omission."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAtmosphereActions::GetAtmosphereProperties),
		FParamSchemaBuilder()
			.Required(TEXT("actor_name"), TEXT("string"), TEXT("Atmosphere actor name or label"))
			.Optional(TEXT("properties"), TEXT("array"), TEXT("Exact UPROPERTY names to read (default: the data-driven set for this type)"))
			.Optional(TEXT("all"), TEXT("boolean"), TEXT("Read every editable/visible UPROPERTY on the settings target instead of the default set"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("list_atmosphere_presets"),
		TEXT("List the data-driven atmosphere presets available to mesh.spawn_atmosphere / "
			 "mesh.set_atmosphere_properties / mesh.set_lumen_settings. Built-ins ship in "
			 "Plugins/Monolith/Config/MonolithAtmospherePresets.json; drop your own JSON in "
			 "Plugins/Monolith/Saved/Monolith/AtmospherePresets/ to add or override presets without rebuilding."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAtmosphereActions::ListAtmospherePresets),
		FParamSchemaBuilder()
			.Optional(TEXT("type"), TEXT("string"), TEXT("Filter by type: post_process, height_fog, sky_atmosphere, lumen"))
			.Optional(TEXT("include_properties"), TEXT("boolean"), TEXT("Include each preset's full property set"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("get_lumen_settings"),
		TEXT("Read the Lumen / global-illumination / reflection settings of a post-process volume, together "
			 "with their bOverride_ state. Also returns a READ-ONLY 'project' block from the project renderer "
			 "settings (GI method, reflection method, hardware ray tracing, mesh distance fields) — that block "
			 "is usually the answer to 'why does Lumen do nothing here'. With no actor_name the unbound volume "
			 "is used, or the only volume in the level."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAtmosphereActions::GetLumenSettings),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Post-process volume name or label (default: the unbound volume, or the only one)"))
			.Optional(TEXT("include_project"), TEXT("boolean"), TEXT("Include the read-only project renderer settings block"), TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("set_lumen_settings"),
		TEXT("Write Lumen / global-illumination / reflection settings onto a post-process volume, enabling "
			 "each key's bOverride_ bit so the change takes effect. Only Lumen keys are accepted — anything "
			 "else is refused with the full list (use mesh.set_atmosphere_properties for other post-process "
			 "settings). Project-level renderer settings are NEVER written by this action."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshAtmosphereActions::SetLumenSettings),
		FParamSchemaBuilder()
			.Optional(TEXT("actor_name"), TEXT("string"), TEXT("Post-process volume name or label (default: the unbound volume, or the only one)"))
			.Optional(TEXT("preset"), TEXT("string"), TEXT("Named lumen preset (mesh.list_atmosphere_presets type=lumen)"))
			.Optional(TEXT("properties"), TEXT("object"), TEXT("Lumen UPROPERTY name -> value, e.g. {\"LumenSceneDetail\": 2.0, \"DynamicGlobalIlluminationMethod\": \"Lumen\"}"))
			.Build());
}

// ============================================================================
// mesh.spawn_atmosphere
// ============================================================================

FMonolithActionResult FMonolithMeshAtmosphereActions::SpawnAtmosphere(const TSharedPtr<FJsonObject>& Params)
{
	FString TypeStr;
	if (!Params->TryGetStringField(TEXT("type"), TypeStr))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Missing required param: type. Valid types: %s."),
			*FString::Join(GetAtmosphereTokens(), TEXT(", "))));
	}

	FString TypeError;
	UClass* ActorClass = ResolveAtmosphereActorClass(TypeStr, TypeError);
	if (!ActorClass)
	{
		return FMonolithActionResult::Error(TypeError);
	}
	const FString Token = MonolithAtmosphereTypes::Normalise(TypeStr);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	FString WorldError;
	if (!RequireEditorWorld(World, WorldError))
	{
		return FMonolithActionResult::Error(WorldError);
	}

	FVector Location = FVector::ZeroVector;
	MonolithMeshUtils::ParseVector(Params, TEXT("location"), Location);
	FRotator Rotation = FRotator::ZeroRotator;
	MonolithMeshUtils::ParseRotator(Params, TEXT("rotation"), Rotation);

	FVector Extent(500.0, 500.0, 300.0);
	MonolithMeshUtils::ParseVector(Params, TEXT("extent"), Extent);
	if (Extent.X <= 0 || Extent.Y <= 0 || Extent.Z <= 0)
	{
		return FMonolithActionResult::Error(TEXT("All extent values must be positive."));
	}

	MonolithAtmosphereHelpers::FScopedMeshTransaction Transaction(
		FText::FromString(TEXT("Monolith: Spawn Atmosphere")));

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* Actor = World->SpawnActor(ActorClass, &Location, &Rotation, SpawnParams);
	if (!Actor)
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(FString::Printf(TEXT("Failed to spawn %s"), *ActorClass->GetName()));
	}

	// A post-process volume is an AVolume: it needs brush geometry or it has no bounds.
	bool bBrushValid = true;
	if (APostProcessVolume* PPVol = Cast<APostProcessVolume>(Actor))
	{
		UCubeBuilder* CubeBuilder = NewObject<UCubeBuilder>();
		CubeBuilder->X = Extent.X * 2.0;
		CubeBuilder->Y = Extent.Y * 2.0;
		CubeBuilder->Z = Extent.Z * 2.0;
		UActorFactory::CreateBrushForVolumeActor(PPVol, CubeBuilder);

		UBrushComponent* BrushComp = PPVol->GetBrushComponent();
		bBrushValid = BrushComp && BrushComp->Brush != nullptr;
	}

	FTarget Target;
	FString TargetError;
	if (!ResolveTarget(Actor, Target, TargetError))
	{
		World->DestroyActor(Actor);
		Transaction.Cancel();
		return FMonolithActionResult::Error(TargetError);
	}

	TArray<FString> Applied, Overrides;
	FString ApplyError;
	if (!ApplyPropertyTree(Target, Params, TEXT("preset"), TEXT("properties"),
			TArray<FString>(), Applied, Overrides, ApplyError))
	{
		// Roll the spawn back — an actor that only half-matches the request is worse
		// than no actor: the caller cannot tell which half landed.
		World->DestroyActor(Actor);
		Transaction.Cancel();
		return FMonolithActionResult::Error(ApplyError);
	}

	TArray<FString> ActorApplied;
	if (!ApplyActorPropertyTree(Actor, Params, ActorApplied, ApplyError))
	{
		World->DestroyActor(Actor);
		Transaction.Cancel();
		return FMonolithActionResult::Error(ApplyError);
	}

	FString OptionalName;
	if (Params->TryGetStringField(TEXT("name"), OptionalName) && !OptionalName.IsEmpty())
	{
		Actor->SetActorLabel(OptionalName);
	}

	FString Folder;
	Params->TryGetStringField(TEXT("folder"), Folder);
	Actor->SetFolderPath(FName(Folder.IsEmpty() ? TEXT("Atmosphere") : *Folder));

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	Result->SetStringField(TEXT("class"), ActorClass->GetName());
	Result->SetStringField(TEXT("atmosphere_type"), Token);
	Result->SetStringField(TEXT("target_struct"), Target.Struct->GetName());
	Result->SetArrayField(TEXT("location"), MonolithAtmosphereHelpers::VectorToJsonArray(Actor->GetActorLocation()));
	if (Target.bOverrideAware)
	{
		Result->SetArrayField(TEXT("extent"), MonolithAtmosphereHelpers::VectorToJsonArray(Extent));
		Result->SetBoolField(TEXT("brush_valid"), bBrushValid);
	}
	Result->SetArrayField(TEXT("properties_set"), MonolithAtmosphereHelpers::StringsToJson(Applied));
	Result->SetArrayField(TEXT("overrides_enabled"), MonolithAtmosphereHelpers::StringsToJson(Overrides));
	Result->SetArrayField(TEXT("actor_properties_set"), MonolithAtmosphereHelpers::StringsToJson(ActorApplied));
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// mesh.set_atmosphere_properties
// ============================================================================

FMonolithActionResult FMonolithMeshAtmosphereActions::SetAtmosphereProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: actor_name"));
	}

	FString Error;
	if (!RequireEditorWorld(MonolithMeshUtils::GetEditorWorld(), Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	AActor* Actor = MonolithMeshUtils::FindActorByName(ActorName, Error);
	if (!Actor)
	{
		return FMonolithActionResult::Error(Error);
	}

	FTarget Target;
	if (!ResolveTarget(Actor, Target, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	MonolithAtmosphereHelpers::FScopedMeshTransaction Transaction(
		FText::FromString(TEXT("Monolith: Set Atmosphere Properties")));

	TArray<FString> Applied, Overrides;
	if (!ApplyPropertyTree(Target, Params, TEXT("preset"), TEXT("properties"),
			TArray<FString>(), Applied, Overrides, Error))
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(Error);
	}

	TArray<FString> ActorApplied;
	if (!ApplyActorPropertyTree(Actor, Params, ActorApplied, Error))
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(Error);
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	Result->SetStringField(TEXT("atmosphere_type"), Target.Token);
	Result->SetStringField(TEXT("target_struct"), Target.Struct->GetName());
	Result->SetArrayField(TEXT("properties_set"), MonolithAtmosphereHelpers::StringsToJson(Applied));
	Result->SetArrayField(TEXT("overrides_enabled"), MonolithAtmosphereHelpers::StringsToJson(Overrides));
	Result->SetArrayField(TEXT("actor_properties_set"), MonolithAtmosphereHelpers::StringsToJson(ActorApplied));
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// mesh.get_atmosphere_properties
// ============================================================================

FMonolithActionResult FMonolithMeshAtmosphereActions::GetAtmosphereProperties(const TSharedPtr<FJsonObject>& Params)
{
	FString ActorName;
	if (!Params->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: actor_name"));
	}

	FString Error;
	if (!RequireEditorWorld(MonolithMeshUtils::GetEditorWorld(), Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	AActor* Actor = MonolithMeshUtils::FindActorByName(ActorName, Error);
	if (!Actor)
	{
		return FMonolithActionResult::Error(Error);
	}

	FTarget Target;
	if (!ResolveTarget(Actor, Target, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<FString> Wanted;
	bool bExplicit = false;
	const TArray<TSharedPtr<FJsonValue>>* NameArr = nullptr;
	if (Params->TryGetArrayField(TEXT("properties"), NameArr) && NameArr && NameArr->Num() > 0)
	{
		bExplicit = true;
		for (const TSharedPtr<FJsonValue>& V : *NameArr)
		{
			const FString N = V.IsValid() ? V->AsString() : FString();
			if (!N.IsEmpty()) { Wanted.AddUnique(N); }
		}
	}

	bool bAll = false;
	Params->TryGetBoolField(TEXT("all"), bAll);

	auto PropsObj = MakeShared<FJsonObject>();
	TArray<FString> Reported;
	TArray<FString> Missing;

	if (bExplicit)
	{
		TArray<FString> Unknown;
		for (const FString& Name : Wanted)
		{
			FProperty* Prop = FMonolithReflectionWalker::FindPropertyForwarding(Target.Struct, Name);
			if (!Prop)
			{
				const TArray<FString> Hints = FMonolithMeshLightActions::SuggestPropertyNames(Target.Struct, Name);
				Unknown.Add(Hints.Num() > 0
					? FString::Printf(TEXT("'%s' (did you mean: %s?)"), *Name, *FString::Join(Hints, TEXT(", ")))
					: FString::Printf(TEXT("'%s'"), *Name));
				continue;
			}
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Target.Container);
			PropsObj->SetField(Prop->GetName(),
				FMonolithReflectionReader::PropertyToJsonValue(Prop, ValuePtr, Target.CradleObject));
			Reported.Add(Prop->GetName());
		}
		if (Unknown.Num() > 0)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("%d unknown propert%s on %s (atmosphere actor '%s'): %s. "
					 "Call mesh.get_atmosphere_properties with all=true to list every readable name."),
				Unknown.Num(), Unknown.Num() == 1 ? TEXT("y") : TEXT("ies"),
				*Target.Struct->GetName(), *Actor->GetActorNameOrLabel(),
				*FString::Join(Unknown, TEXT(", "))));
		}
	}
	else if (bAll)
	{
		for (TFieldIterator<FProperty> It(Target.Struct); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
			{
				continue;
			}
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Target.Container);
			PropsObj->SetField(Prop->GetName(),
				FMonolithReflectionReader::PropertyToJsonValue(Prop, ValuePtr, Target.CradleObject));
			Reported.Add(Prop->GetName());
		}
	}
	else
	{
		TArray<FString> Warnings;
		const TArray<FString> Keys = LoadReadbackKeys(Target.Token, Warnings);
		MonolithAtmosphereHelpers::ReadKeys(Target.Struct, Target.Container, Target.CradleObject,
			Keys, PropsObj, Reported, Missing);
		if (Missing.Num() > 0)
		{
			// A default-set name that no longer exists on this engine version.
			// Reported, not fatal — the read-back set is data, not a contract.
			PropsObj->SetField(TEXT("__unresolved"),
				MakeShared<FJsonValueArray>(MonolithAtmosphereHelpers::StringsToJson(Missing)));
		}
	}

	const FVector Loc = Actor->GetActorLocation();
	const FRotator Rot = Actor->GetActorRotation();
	TArray<TSharedPtr<FJsonValue>> RotArr{
		MakeShared<FJsonValueNumber>(Rot.Pitch), MakeShared<FJsonValueNumber>(Rot.Yaw),
		MakeShared<FJsonValueNumber>(Rot.Roll) };

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	Result->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
	Result->SetStringField(TEXT("atmosphere_type"), Target.Token);
	Result->SetStringField(TEXT("target_struct"), Target.Struct->GetName());
	Result->SetArrayField(TEXT("location"), MonolithAtmosphereHelpers::VectorToJsonArray(Loc));
	Result->SetArrayField(TEXT("rotation"), RotArr);
	Result->SetNumberField(TEXT("property_count"), Reported.Num());
	Result->SetObjectField(TEXT("properties"), PropsObj);
	if (Target.bOverrideAware)
	{
		Result->SetObjectField(TEXT("overrides"),
			MonolithAtmosphereHelpers::ReadOverrides(Target.Struct, Target.Container, Target.CradleObject, Reported));
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// mesh.list_atmosphere_presets
// ============================================================================

FMonolithActionResult FMonolithMeshAtmosphereActions::ListAtmospherePresets(const TSharedPtr<FJsonObject>& Params)
{
	FString TypeFilter;
	Params->TryGetStringField(TEXT("type"), TypeFilter);
	if (!TypeFilter.IsEmpty())
	{
		const FString Norm = MonolithAtmosphereTypes::Normalise(TypeFilter);
		if (!GetSectionTokens().Contains(Norm) || Norm == TEXT("project"))
		{
			TArray<FString> Valid = GetAtmosphereTokens();
			Valid.Add(TEXT("lumen"));
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Unknown preset type filter '%s'. Valid filters: %s."),
				*TypeFilter, *FString::Join(Valid, TEXT(", "))));
		}
		TypeFilter = Norm;
	}

	bool bIncludeProperties = true;
	Params->TryGetBoolField(TEXT("include_properties"), bIncludeProperties);

	TArray<FString> Warnings;
	const TMap<FString, FMonolithJsonPreset> Loaded = Presets().LoadPresets(Warnings);

	TArray<FString> Names;
	Loaded.GetKeys(Names);
	Names.Sort();

	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FString& Name : Names)
	{
		const FMonolithJsonPreset& P = Loaded[Name];
		if (!TypeFilter.IsEmpty() && !P.TypeToken.Equals(TypeFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), P.Name);
		Obj->SetStringField(TEXT("type"), P.TypeToken);
		Obj->SetStringField(TEXT("description"), P.Description);
		Obj->SetStringField(TEXT("source_file"), P.SourceFile);
		if (bIncludeProperties && P.Properties.IsValid())
		{
			Obj->SetObjectField(TEXT("properties"), P.Properties);
		}
		Out.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TArray<FString> AllTypes = GetAtmosphereTokens();
	AllTypes.Add(TEXT("lumen"));

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("presets"), Out);
	Result->SetNumberField(TEXT("count"), Out.Num());
	Result->SetNumberField(TEXT("total_count"), Loaded.Num());
	Result->SetStringField(TEXT("builtin_file"), Presets().GetBuiltinFile());
	Result->SetStringField(TEXT("user_directory"), Presets().GetUserDirectory());
	Result->SetArrayField(TEXT("atmosphere_types"), MonolithAtmosphereHelpers::StringsToJson(AllTypes));
	if (Warnings.Num() > 0)
	{
		Result->SetArrayField(TEXT("warnings"), MonolithAtmosphereHelpers::StringsToJson(Warnings));
	}
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// Lumen
// ============================================================================

namespace MonolithLumenHelpers
{
	/**
	 * Which post-process volume a Lumen call means when the caller did not name one.
	 * Prefers the unbound (whole-level) volume, then falls back to the only volume in
	 * the level. Ambiguity is an error, never a guess.
	 */
	static APostProcessVolume* ResolveVolume(
		UWorld* World, const FString& ActorName, bool& bOutNoneFound, FString& OutError)
	{
		bOutNoneFound = false;
		OutError.Reset();

		if (!ActorName.IsEmpty())
		{
			AActor* Actor = MonolithMeshUtils::FindActorByName(ActorName, OutError);
			if (!Actor)
			{
				return nullptr;
			}
			APostProcessVolume* PPVol = Cast<APostProcessVolume>(Actor);
			if (!PPVol)
			{
				OutError = FString::Printf(
					TEXT("Actor '%s' is a %s, not a post-process volume. Lumen settings live on "
						 "APostProcessVolume::Settings — spawn one with "
						 "mesh.spawn_atmosphere type=post_process."),
					*Actor->GetActorNameOrLabel(), *Actor->GetClass()->GetName());
				return nullptr;
			}
			return PPVol;
		}

		TArray<APostProcessVolume*> Found;
		APostProcessVolume* Unbound = nullptr;
		for (TActorIterator<APostProcessVolume> It(World); It; ++It)
		{
			Found.Add(*It);
			if (It->bUnbound && !Unbound)
			{
				Unbound = *It;
			}
		}

		if (Unbound)
		{
			return Unbound;
		}
		if (Found.Num() == 1)
		{
			return Found[0];
		}
		if (Found.Num() == 0)
		{
			bOutNoneFound = true;
			OutError = TEXT("This level has no post-process volume, so it carries no per-level Lumen "
							"settings. Create one with mesh.spawn_atmosphere type=post_process "
							"actor_properties={\"bUnbound\": true}.");
			return nullptr;
		}

		TArray<FString> Labels;
		for (APostProcessVolume* V : Found) { Labels.Add(V->GetActorNameOrLabel()); }
		Labels.Sort();
		OutError = FString::Printf(
			TEXT("This level has %d post-process volumes and none of them is unbound, so there is no "
				 "single obvious target. Pass actor_name explicitly. Volumes: %s."),
			Found.Num(), *FString::Join(Labels, TEXT(", ")));
		return nullptr;
	}

	/** Read the project renderer settings block. READ ONLY — never written by Monolith. */
	static TSharedPtr<FJsonObject> ReadProjectBlock(TArray<FString>& OutMissing)
	{
		auto Obj = MakeShared<FJsonObject>();
		const URendererSettings* Settings = GetDefault<URendererSettings>();
		if (!Settings)
		{
			return Obj;
		}
		TArray<FString> Warnings;
		const TArray<FString> Keys =
			FMonolithMeshAtmosphereActions::LoadReadbackKeys(TEXT("project"), Warnings);
		TArray<FString> Reported;
		MonolithAtmosphereHelpers::ReadKeys(
			URendererSettings::StaticClass(), Settings, Settings, Keys, Obj, Reported, OutMissing);
		return Obj;
	}
}

FMonolithActionResult FMonolithMeshAtmosphereActions::GetLumenSettings(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!RequireEditorWorld(World, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString ActorName;
	Params->TryGetStringField(TEXT("actor_name"), ActorName);

	bool bIncludeProject = true;
	Params->TryGetBoolField(TEXT("include_project"), bIncludeProject);

	bool bNoneFound = false;
	APostProcessVolume* PPVol = MonolithLumenHelpers::ResolveVolume(World, ActorName, bNoneFound, Error);
	if (!PPVol && !bNoneFound)
	{
		return FMonolithActionResult::Error(Error);
	}

	auto Result = MakeShared<FJsonObject>();
	TArray<FString> Warnings;
	const TArray<FString> LumenKeys = LoadReadbackKeys(TEXT("lumen"), Warnings);

	if (PPVol)
	{
		FTarget Target;
		if (!ResolveTarget(PPVol, Target, Error))
		{
			return FMonolithActionResult::Error(Error);
		}

		auto LumenObj = MakeShared<FJsonObject>();
		TArray<FString> Reported, Missing;
		MonolithAtmosphereHelpers::ReadKeys(Target.Struct, Target.Container, Target.CradleObject,
			LumenKeys, LumenObj, Reported, Missing);

		auto VolumeObj = MakeShared<FJsonObject>();
		VolumeObj->SetStringField(TEXT("actor_name"), PPVol->GetActorNameOrLabel());
		VolumeObj->SetStringField(TEXT("class"), PPVol->GetClass()->GetName());
		VolumeObj->SetBoolField(TEXT("unbound"), PPVol->bUnbound != 0);
		VolumeObj->SetBoolField(TEXT("enabled"), PPVol->bEnabled != 0);
		VolumeObj->SetNumberField(TEXT("priority"), PPVol->Priority);
		VolumeObj->SetNumberField(TEXT("blend_weight"), PPVol->BlendWeight);

		Result->SetObjectField(TEXT("volume"), VolumeObj);
		Result->SetObjectField(TEXT("lumen"), LumenObj);
		Result->SetObjectField(TEXT("overrides"),
			MonolithAtmosphereHelpers::ReadOverrides(Target.Struct, Target.Container, Target.CradleObject, Reported));
		Result->SetNumberField(TEXT("property_count"), Reported.Num());
		if (Missing.Num() > 0)
		{
			Result->SetArrayField(TEXT("unresolved"), MonolithAtmosphereHelpers::StringsToJson(Missing));
		}
	}
	else
	{
		Result->SetField(TEXT("volume"), MakeShared<FJsonValueNull>());
		Result->SetObjectField(TEXT("lumen"), MakeShared<FJsonObject>());
		Result->SetNumberField(TEXT("property_count"), 0);
		Result->SetStringField(TEXT("note"), Error);
	}

	if (bIncludeProject)
	{
		TArray<FString> ProjectMissing;
		Result->SetObjectField(TEXT("project"), MonolithLumenHelpers::ReadProjectBlock(ProjectMissing));
		Result->SetBoolField(TEXT("project_is_read_only"), true);
		if (ProjectMissing.Num() > 0)
		{
			Result->SetArrayField(TEXT("project_unresolved"),
				MonolithAtmosphereHelpers::StringsToJson(ProjectMissing));
		}
	}

	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithMeshAtmosphereActions::SetLumenSettings(const TSharedPtr<FJsonObject>& Params)
{
	FString Error;
	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!RequireEditorWorld(World, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString ActorName;
	Params->TryGetStringField(TEXT("actor_name"), ActorName);

	bool bNoneFound = false;
	APostProcessVolume* PPVol = MonolithLumenHelpers::ResolveVolume(World, ActorName, bNoneFound, Error);
	if (!PPVol)
	{
		return FMonolithActionResult::Error(Error);
	}

	FTarget Target;
	if (!ResolveTarget(PPVol, Target, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<FString> Warnings;
	const TArray<FString> LumenKeys = LoadReadbackKeys(TEXT("lumen"), Warnings);
	if (LumenKeys.Num() == 0)
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("No Lumen key set is available — the built-in preset data could not be read. %s"),
			*FString::Join(Warnings, TEXT("; "))));
	}

	MonolithAtmosphereHelpers::FScopedMeshTransaction Transaction(
		FText::FromString(TEXT("Monolith: Set Lumen Settings")));

	TArray<FString> Applied, Overrides;
	if (!ApplyPropertyTree(Target, Params, TEXT("preset"), TEXT("properties"),
			LumenKeys, Applied, Overrides, Error))
	{
		Transaction.Cancel();
		return FMonolithActionResult::Error(Error);
	}

	auto LumenObj = MakeShared<FJsonObject>();
	TArray<FString> Reported, Missing;
	MonolithAtmosphereHelpers::ReadKeys(Target.Struct, Target.Container, Target.CradleObject,
		LumenKeys, LumenObj, Reported, Missing);

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), PPVol->GetActorNameOrLabel());
	Result->SetStringField(TEXT("atmosphere_type"), Target.Token);
	Result->SetStringField(TEXT("target_struct"), Target.Struct->GetName());
	Result->SetArrayField(TEXT("properties_set"), MonolithAtmosphereHelpers::StringsToJson(Applied));
	Result->SetArrayField(TEXT("overrides_enabled"), MonolithAtmosphereHelpers::StringsToJson(Overrides));
	Result->SetObjectField(TEXT("lumen"), LumenObj);
	Result->SetObjectField(TEXT("overrides"),
		MonolithAtmosphereHelpers::ReadOverrides(Target.Struct, Target.Container, Target.CradleObject, Reported));
	return FMonolithActionResult::Success(Result);
}
