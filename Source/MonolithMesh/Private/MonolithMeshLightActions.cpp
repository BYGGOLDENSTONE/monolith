#include "MonolithMeshLightActions.h"

#include "MonolithMeshUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"
#include "MonolithBulkFillTypes.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "Reflection/MonolithReflectionReader.h"

#include "Engine/DirectionalLight.h"
#include "Engine/PointLight.h"
#include "Engine/RectLight.h"
#include "Engine/SkyLight.h"
#include "Engine/SpotLight.h"
#include "Engine/World.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/LightComponentBase.h"
#include "Components/PointLightComponent.h"
#include "Components/RectLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SpotLightComponent.h"
#include "GameFramework/Actor.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/UnrealType.h"

// ============================================================================
// Light type table
//
// Token -> actor class is the ONE mapping that cannot live in data (a JSON file
// cannot name a C++ UClass without a string lookup that is strictly worse than
// this table: no compile-time checking, no IWYU include). Everything else about
// a light — presets, defaults, which properties to report — is data.
// ============================================================================

namespace MonolithLightTypes
{
	struct FEntry
	{
		const TCHAR* Token;
		UClass* ActorClass;
		UClass* ComponentClass;
	};

	static const TArray<FEntry>& Table()
	{
		// Built once, after UClasses exist. Static-local init is thread-safe and
		// deferred, which matters because this file's statics are constructed
		// before StaticClass() is safe at module-load time.
		static const TArray<FEntry> Entries = {
			{ TEXT("directional"), ADirectionalLight::StaticClass(), UDirectionalLightComponent::StaticClass() },
			{ TEXT("point"),       APointLight::StaticClass(),       UPointLightComponent::StaticClass()       },
			{ TEXT("spot"),        ASpotLight::StaticClass(),        USpotLightComponent::StaticClass()        },
			{ TEXT("rect"),        ARectLight::StaticClass(),        URectLightComponent::StaticClass()        },
			{ TEXT("sky"),         ASkyLight::StaticClass(),         USkyLightComponent::StaticClass()         },
		};
		return Entries;
	}

	/** Normalise an incoming token: lowercase, strip a leading 'A'/'U' and a trailing "light". */
	static FString Normalise(const FString& In)
	{
		FString S = In.TrimStartAndEnd().ToLower();
		S.ReplaceInline(TEXT(" "), TEXT(""));
		S.ReplaceInline(TEXT("_"), TEXT(""));
		if (S.EndsWith(TEXT("component"))) { S.LeftChopInline(9); }
		if (S.EndsWith(TEXT("light")) && S.Len() > 5) { S.LeftChopInline(5); }
		if (S.Len() > 1 && (S[0] == TEXT('a') || S[0] == TEXT('u')))
		{
			// "apointlight" -> "apoint" -> "point"; only strip when the remainder matches a token.
			const FString Chopped = S.RightChop(1);
			for (const FEntry& E : Table())
			{
				if (Chopped == E.Token) { return Chopped; }
			}
		}
		return S;
	}
}

const TArray<FString>& FMonolithMeshLightActions::GetLightTypeTokens()
{
	static TArray<FString> Tokens;
	if (Tokens.Num() == 0)
	{
		for (const MonolithLightTypes::FEntry& E : MonolithLightTypes::Table())
		{
			Tokens.Add(E.Token);
		}
	}
	return Tokens;
}

UClass* FMonolithMeshLightActions::ResolveLightActorClass(const FString& Token, FString& OutError)
{
	OutError.Reset();
	if (Token.TrimStartAndEnd().IsEmpty())
	{
		OutError = FString::Printf(TEXT("Missing light type. Valid types: %s."),
			*FString::Join(GetLightTypeTokens(), TEXT(", ")));
		return nullptr;
	}

	const FString Norm = MonolithLightTypes::Normalise(Token);
	for (const MonolithLightTypes::FEntry& E : MonolithLightTypes::Table())
	{
		if (Norm == E.Token)
		{
			return E.ActorClass;
		}
	}

	OutError = FString::Printf(TEXT("Unknown light type '%s'. Valid types: %s."),
		*Token, *FString::Join(GetLightTypeTokens(), TEXT(", ")));
	return nullptr;
}

ULightComponentBase* FMonolithMeshLightActions::ResolveLightComponent(AActor* Actor, FString& OutError)
{
	OutError.Reset();
	if (!Actor)
	{
		OutError = TEXT("No actor supplied.");
		return nullptr;
	}

	// ULightComponentBase — NOT ULightComponent. USkyLightComponent derives from
	// the base only, which is why every pre-existing light path missed sky lights.
	ULightComponentBase* Comp = Actor->FindComponentByClass<ULightComponentBase>();
	if (!Comp)
	{
		OutError = FString::Printf(
			TEXT("Actor '%s' (%s) is not a light — it has no ULightComponentBase. "
				 "Spawn one with mesh.place_light (types: %s)."),
			*Actor->GetActorNameOrLabel(), *Actor->GetClass()->GetName(),
			*FString::Join(GetLightTypeTokens(), TEXT(", ")));
		return nullptr;
	}
	return Comp;
}

FString FMonolithMeshLightActions::TokenForLightComponent(const ULightComponentBase* Comp)
{
	if (!Comp)
	{
		return FString();
	}
	// Most-derived first: USpotLightComponent derives from UPointLightComponent.
	const UClass* CompClass = Comp->GetClass();
	const MonolithLightTypes::FEntry* Best = nullptr;
	int32 BestDepth = -1;
	for (const MonolithLightTypes::FEntry& E : MonolithLightTypes::Table())
	{
		if (!CompClass->IsChildOf(E.ComponentClass))
		{
			continue;
		}
		int32 Depth = 0;
		for (const UClass* C = E.ComponentClass; C; C = C->GetSuperClass()) { ++Depth; }
		if (Depth > BestDepth)
		{
			BestDepth = Depth;
			Best = &E;
		}
	}
	return Best ? FString(Best->Token) : FString();
}

bool FMonolithMeshLightActions::RequireEditorWorld(const UWorld* World, FString& OutError)
{
	if (World)
	{
		OutError.Reset();
		return true;
	}
	OutError = TEXT("No open level — the editor has no world. Open or create a level "
					"(editor.open_level / editor.new_level) before placing or editing lights.");
	return false;
}

// ============================================================================
// Data-driven preset library
//
// Built-in file ships with the plugin; user files override by name. Same layout
// as FMonolithMeshPresetActions (built-in + Saved/Monolith/<Kind>/), which is the
// preset mechanism this module already uses.
// ============================================================================

FString FMonolithMeshLightActions::GetBuiltinPresetFile()
{
	return FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Config") / TEXT("MonolithLightPresets.json");
}

FString FMonolithMeshLightActions::GetUserPresetDirectory()
{
	return FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Saved") / TEXT("Monolith") / TEXT("LightPresets");
}

namespace MonolithLightPresetIO
{
	/** Read one JSON document. Returns null + reason on failure. */
	static TSharedPtr<FJsonObject> ReadDocument(const FString& FilePath, FString& OutError)
	{
		FString Raw;
		if (!FFileHelper::LoadFileToString(Raw, *FilePath))
		{
			OutError = FString::Printf(TEXT("could not read '%s'"), *FilePath);
			return nullptr;
		}
		TSharedPtr<FJsonObject> Doc;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		if (!FJsonSerializer::Deserialize(Reader, Doc) || !Doc.IsValid())
		{
			OutError = FString::Printf(TEXT("'%s' is not valid JSON"), *FilePath);
			return nullptr;
		}
		return Doc;
	}

	/** Built-in document first, then every user *.json (sorted for determinism). */
	static void CollectDocuments(TArray<TPair<FString, TSharedPtr<FJsonObject>>>& Out, TArray<FString>& OutWarnings)
	{
		const FString Builtin = FMonolithMeshLightActions::GetBuiltinPresetFile();
		if (IFileManager::Get().FileExists(*Builtin))
		{
			FString Err;
			if (TSharedPtr<FJsonObject> Doc = ReadDocument(Builtin, Err))
			{
				Out.Emplace(Builtin, Doc);
			}
			else
			{
				OutWarnings.Add(Err);
			}
		}
		else
		{
			OutWarnings.Add(FString::Printf(
				TEXT("built-in light preset file missing: %s (plugin install is incomplete)"), *Builtin));
		}

		const FString UserDir = FMonolithMeshLightActions::GetUserPresetDirectory();
		TArray<FString> UserFiles;
		IFileManager::Get().FindFiles(UserFiles, *(UserDir / TEXT("*.json")), true, false);
		UserFiles.Sort();
		for (const FString& Leaf : UserFiles)
		{
			const FString Full = UserDir / Leaf;
			FString Err;
			if (TSharedPtr<FJsonObject> Doc = ReadDocument(Full, Err))
			{
				Out.Emplace(Full, Doc);
			}
			else
			{
				OutWarnings.Add(Err);
			}
		}
	}
}

TMap<FString, FMonolithMeshLightActions::FPreset> FMonolithMeshLightActions::LoadPresets(TArray<FString>& OutWarnings)
{
	TMap<FString, FPreset> Presets;

	TArray<TPair<FString, TSharedPtr<FJsonObject>>> Docs;
	MonolithLightPresetIO::CollectDocuments(Docs, OutWarnings);

	for (const TPair<FString, TSharedPtr<FJsonObject>>& Doc : Docs)
	{
		const TSharedPtr<FJsonObject>* PresetsObj = nullptr;
		if (!Doc.Value->TryGetObjectField(TEXT("presets"), PresetsObj) || !PresetsObj || !(*PresetsObj).IsValid())
		{
			continue;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PresetsObj)->Values)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(Entry) || !Entry || !(*Entry).IsValid())
			{
				OutWarnings.Add(FString::Printf(TEXT("preset '%s' in '%s' is not an object — skipped"),
					*Pair.Key, *Doc.Key));
				continue;
			}

			FPreset P;
			P.Name = Pair.Key;
			P.SourceFile = Doc.Key;
			(*Entry)->TryGetStringField(TEXT("type"), P.TypeToken);
			(*Entry)->TryGetStringField(TEXT("description"), P.Description);

			const TSharedPtr<FJsonObject>* Props = nullptr;
			if ((*Entry)->TryGetObjectField(TEXT("properties"), Props) && Props && (*Props).IsValid())
			{
				P.Properties = MakeShared<FJsonObject>();
				P.Properties->Values = (*Props)->Values;
			}
			else
			{
				OutWarnings.Add(FString::Printf(TEXT("preset '%s' in '%s' has no 'properties' object — skipped"),
					*Pair.Key, *Doc.Key));
				continue;
			}

			// Later document wins — user files override built-ins by name.
			Presets.Add(P.Name, MoveTemp(P));
		}
	}

	return Presets;
}

TArray<FString> FMonolithMeshLightActions::LoadReadbackKeys(const FString& TypeToken)
{
	TArray<FString> Keys;
	TArray<FString> Warnings;

	TArray<TPair<FString, TSharedPtr<FJsonObject>>> Docs;
	MonolithLightPresetIO::CollectDocuments(Docs, Warnings);

	auto Append = [&Keys](const TSharedPtr<FJsonObject>& Obj, const FString& Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj->TryGetArrayField(Field, Arr) || !Arr)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const FString Name = V.IsValid() ? V->AsString() : FString();
			if (!Name.IsEmpty())
			{
				Keys.AddUnique(Name);
			}
		}
	};

	for (const TPair<FString, TSharedPtr<FJsonObject>>& Doc : Docs)
	{
		const TSharedPtr<FJsonObject>* Readback = nullptr;
		if (!Doc.Value->TryGetObjectField(TEXT("readback"), Readback) || !Readback || !(*Readback).IsValid())
		{
			continue;
		}
		Append(*Readback, TEXT("common"));
		if (!TypeToken.IsEmpty())
		{
			Append(*Readback, TypeToken);
		}
	}

	return Keys;
}

bool FMonolithMeshLightActions::ResolvePreset(const FString& Name, FPreset& OutPreset, FString& OutError)
{
	TArray<FString> Warnings;
	const TMap<FString, FPreset> Presets = LoadPresets(Warnings);

	if (const FPreset* Found = Presets.Find(Name))
	{
		OutPreset = *Found;
		OutError.Reset();
		return true;
	}

	// Case-insensitive second pass — preset names are authored by hand.
	for (const TPair<FString, FPreset>& Pair : Presets)
	{
		if (Pair.Key.Equals(Name, ESearchCase::IgnoreCase))
		{
			OutPreset = Pair.Value;
			OutError.Reset();
			return true;
		}
	}

	TArray<FString> Available;
	Presets.GetKeys(Available);
	Available.Sort();
	OutError = FString::Printf(
		TEXT("Unknown light preset '%s'. Available presets (%d): %s. "
			 "Run mesh.list_light_presets for their types and property sets, or add your own JSON to %s."),
		*Name, Available.Num(),
		Available.Num() > 0 ? *FString::Join(Available, TEXT(", ")) : TEXT("<none loaded>"),
		*GetUserPresetDirectory());
	if (Warnings.Num() > 0)
	{
		OutError += FString::Printf(TEXT(" Preset load warnings: %s"), *FString::Join(Warnings, TEXT("; ")));
	}
	return false;
}

// ============================================================================
// Reflection write path
// ============================================================================

namespace MonolithLightSuggest
{
	/** Classic Levenshtein distance, two-row variant. Both inputs are lowercased. */
	static int32 EditDistance(const FString& A, const FString& B)
	{
		const int32 LenA = A.Len();
		const int32 LenB = B.Len();
		if (LenA == 0) { return LenB; }
		if (LenB == 0) { return LenA; }

		TArray<int32> Prev, Curr;
		Prev.SetNumUninitialized(LenB + 1);
		Curr.SetNumUninitialized(LenB + 1);
		for (int32 j = 0; j <= LenB; ++j) { Prev[j] = j; }

		for (int32 i = 1; i <= LenA; ++i)
		{
			Curr[0] = i;
			for (int32 j = 1; j <= LenB; ++j)
			{
				const int32 Cost = (A[i - 1] == B[j - 1]) ? 0 : 1;
				Curr[j] = FMath::Min3(Curr[j - 1] + 1, Prev[j] + 1, Prev[j - 1] + Cost);
			}
			Swap(Prev, Curr);
		}
		return Prev[LenB];
	}
}

TArray<FString> FMonolithMeshLightActions::SuggestPropertyNames(const UStruct* Struct, const FString& Query, int32 MaxResults)
{
	TArray<FString> Out;
	if (!Struct || Query.IsEmpty())
	{
		return Out;
	}

	// Rank by (score, distance): substring/prefix hits first, then near-misses by
	// edit distance — a single-character typo ("Intensitty") must still suggest
	// "Intensity", which a pure substring match cannot do.
	struct FCandidate { FString Name; int32 Score; int32 Distance; };
	TArray<FCandidate> Candidates;

	const FString Lower = Query.ToLower();
	const int32 Threshold = FMath::Max(2, Lower.Len() / 4);

	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		const FString PropName = It->GetName();
		const FString PropLower = PropName.ToLower();

		int32 Score = 3;
		if (PropLower == Lower)                                        { Score = 0; }
		else if (PropLower.StartsWith(Lower) || Lower.StartsWith(PropLower)) { Score = 1; }
		else if (PropLower.Contains(Lower) || Lower.Contains(PropLower))     { Score = 2; }

		const int32 Distance = MonolithLightSuggest::EditDistance(Lower, PropLower);
		if (Score == 3 && Distance > Threshold)
		{
			continue;
		}
		Candidates.Add({ PropName, Score, Distance });
	}

	Candidates.Sort([](const FCandidate& A, const FCandidate& B)
	{
		if (A.Score != B.Score) { return A.Score < B.Score; }
		if (A.Distance != B.Distance) { return A.Distance < B.Distance; }
		return A.Name < B.Name;
	});

	for (const FCandidate& C : Candidates)
	{
		if (Out.Num() >= MaxResults) { break; }
		Out.Add(C.Name);
	}
	return Out;
}

bool FMonolithMeshLightActions::ApplyPropertyTree(
	ULightComponentBase* Comp,
	const TSharedPtr<FJsonObject>& Params,
	TArray<FString>& OutApplied,
	FString& OutError)
{
	OutApplied.Reset();
	OutError.Reset();

	if (!Comp)
	{
		OutError = TEXT("No light component supplied.");
		return false;
	}
	if (!Params.IsValid())
	{
		return true;
	}

	// --- Merge preset (if any) then the explicit `properties` object over it. ---
	TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
	const FString ActualToken = TokenForLightComponent(Comp);

	FString PresetName;
	if (Params->TryGetStringField(TEXT("preset"), PresetName) && !PresetName.TrimStartAndEnd().IsEmpty())
	{
		FPreset Preset;
		if (!ResolvePreset(PresetName.TrimStartAndEnd(), Preset, OutError))
		{
			return false;
		}
		if (!Preset.TypeToken.IsEmpty() && !ActualToken.IsEmpty()
			&& !Preset.TypeToken.Equals(ActualToken, ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(
				TEXT("Preset '%s' targets '%s' lights but the target is a %s ('%s'). "
					 "Pick a preset of the right type (mesh.list_light_presets type=%s)."),
				*Preset.Name, *Preset.TypeToken, *Comp->GetClass()->GetName(), *ActualToken, *ActualToken);
			return false;
		}
		if (Preset.Properties.IsValid())
		{
			Tree->Values = Preset.Properties->Values;
		}
	}

	const TSharedPtr<FJsonObject>* Explicit = nullptr;
	if (Params->TryGetObjectField(TEXT("properties"), Explicit) && Explicit && (*Explicit).IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Explicit)->Values)
		{
			Tree->Values.Add(Pair.Key, Pair.Value);
		}
	}
	else if (Params->HasField(TEXT("properties")) && !Params->HasTypedField<EJson::Object>(TEXT("properties")))
	{
		OutError = TEXT("Param 'properties' must be a JSON object of {UPROPERTY name: value}, "
						"e.g. {\"Intensity\": 1700, \"AttenuationRadius\": 600}.");
		return false;
	}

	if (Tree->Values.Num() == 0)
	{
		return true; // nothing asked for — not an error
	}

	// --- Validate BEFORE mutating. InspectTree runs the exact same coercion the
	//     real write does, against a scratch buffer, so a single bad key cannot
	//     leave the light half-configured. ---
	UClass* CompClass = Comp->GetClass();
	FBulkFillSpec Spec;
	Spec.TargetNamespace = TEXT("mesh");
	Spec.Tree = Tree;
	Spec.bStrict = true;

	FDryRunReport DryRun = FMonolithReflectionWalker::InspectTree(Tree, CompClass, Comp, Spec);
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
			const TArray<FString> Hints = SuggestPropertyNames(CompClass, W.Path);
			if (W.Reason.Contains(TEXT("unknown field")) && Hints.Num() > 0)
			{
				Line += FString::Printf(TEXT(" (did you mean: %s?)"), *FString::Join(Hints, TEXT(", ")));
			}
			Problems.Add(Line);
		}
		OutError = FString::Printf(
			TEXT("%d light property write(s) rejected on %s — nothing was applied. %s. "
				 "Use mesh.get_light_properties to see the exact UPROPERTY names this light supports."),
			Problems.Num(), *CompClass->GetName(), *FString::Join(Problems, TEXT("; ")));
		return false;
	}

	// --- Commit. Edit cradle around the whole tree: Modify + PreEditChange per
	//     key, WriteTree, PostEditChangeProperty per key, then a render-state
	//     refresh so the viewport reflects the change immediately. ---
	Comp->Modify();

	TArray<FProperty*> Touched;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Tree->Values)
	{
		if (FProperty* Prop = FMonolithReflectionWalker::FindPropertyForwarding(CompClass, Pair.Key))
		{
			Touched.Add(Prop);
#if WITH_EDITOR
			Comp->PreEditChange(Prop);
#endif
		}
	}

	FDryRunReport Report = FMonolithReflectionWalker::WriteTree(Tree, CompClass, Comp, Comp, Spec);

#if WITH_EDITOR
	for (FProperty* Prop : Touched)
	{
		FPropertyChangedEvent Event(Prop);
		Comp->PostEditChangeProperty(Event);
	}
#endif
	Comp->MarkRenderStateDirty();
	if (AActor* Owner = Comp->GetOwner())
	{
		Owner->MarkPackageDirty();
	}

	if (Report.Errors > 0)
	{
		// The dry run passed, so this means the live write disagreed with the
		// scratch write — surface it rather than silently reporting success.
		TArray<FString> Problems;
		for (const FBulkFillFieldWrite& W : Report.FieldWrites)
		{
			if (!W.bOk)
			{
				Problems.Add(FString::Printf(TEXT("'%s': %s"), *W.Path, *W.Reason));
			}
		}
		OutError = FString::Printf(
			TEXT("%d light property write(s) failed after validation passed on %s: %s"),
			Problems.Num(), *CompClass->GetName(), *FString::Join(Problems, TEXT("; ")));
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

void FMonolithMeshLightActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("mesh"), TEXT("get_light_properties"),
		TEXT("Read a light actor's settings back (all five light types incl. SkyLight). "
			 "Returns the data-driven default set for the light's type, or exactly the properties you name. "
			 "Unknown property names are an error with did-you-mean candidates, not a silent omission."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshLightActions::GetLightProperties),
		FParamSchemaBuilder()
			.Required(TEXT("actor_name"), TEXT("string"), TEXT("Light actor name or label"))
			.Optional(TEXT("properties"), TEXT("array"), TEXT("Exact UPROPERTY names to read (default: the data-driven set for this light type)"))
			.Optional(TEXT("all"), TEXT("boolean"), TEXT("Read every editable/visible UPROPERTY on the light component instead of the default set"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("list_light_presets"),
		TEXT("List the data-driven light presets available to mesh.place_light / mesh.set_light_properties. "
			 "Built-ins ship in Plugins/Monolith/Config/MonolithLightPresets.json; drop your own JSON in "
			 "Plugins/Monolith/Saved/Monolith/LightPresets/ to add or override presets without rebuilding."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshLightActions::ListLightPresets),
		FParamSchemaBuilder()
			.Optional(TEXT("type"), TEXT("string"), TEXT("Filter by light type: directional, point, spot, rect, sky"))
			.Optional(TEXT("include_properties"), TEXT("boolean"), TEXT("Include each preset's full property set"), TEXT("true"))
			.Build());
}

// ============================================================================
// mesh.get_light_properties
// ============================================================================

FMonolithActionResult FMonolithMeshLightActions::GetLightProperties(const TSharedPtr<FJsonObject>& Params)
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

	ULightComponentBase* Comp = ResolveLightComponent(Actor, Error);
	if (!Comp)
	{
		return FMonolithActionResult::Error(Error);
	}

	UClass* CompClass = Comp->GetClass();
	const FString Token = TokenForLightComponent(Comp);

	// Which names to report: explicit list > all > data-driven default set.
	TArray<FString> Wanted;
	bool bExplicit = false;
	const TArray<TSharedPtr<FJsonValue>>* NameArr = nullptr;
	if (Params->TryGetArrayField(TEXT("properties"), NameArr) && NameArr && NameArr->Num() > 0)
	{
		bExplicit = true;
		for (const TSharedPtr<FJsonValue>& V : *NameArr)
		{
			const FString N = V.IsValid() ? V->AsString() : FString();
			if (!N.IsEmpty())
			{
				Wanted.AddUnique(N);
			}
		}
	}

	bool bAll = false;
	Params->TryGetBoolField(TEXT("all"), bAll);

	auto PropsObj = MakeShared<FJsonObject>();
	TArray<FString> Reported;

	if (bExplicit)
	{
		TArray<FString> Unknown;
		for (const FString& Name : Wanted)
		{
			FProperty* Prop = FMonolithReflectionWalker::FindPropertyForwarding(CompClass, Name);
			if (!Prop)
			{
				const TArray<FString> Hints = SuggestPropertyNames(CompClass, Name);
				Unknown.Add(Hints.Num() > 0
					? FString::Printf(TEXT("'%s' (did you mean: %s?)"), *Name, *FString::Join(Hints, TEXT(", ")))
					: FString::Printf(TEXT("'%s'"), *Name));
				continue;
			}
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Comp);
			PropsObj->SetField(Prop->GetName(), FMonolithReflectionReader::PropertyToJsonValue(Prop, ValuePtr, Comp));
			Reported.Add(Prop->GetName());
		}
		if (Unknown.Num() > 0)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("%d unknown propert%s on %s (light '%s'): %s. "
					 "Call mesh.get_light_properties with all=true to list every readable name."),
				Unknown.Num(), Unknown.Num() == 1 ? TEXT("y") : TEXT("ies"),
				*CompClass->GetName(), *Actor->GetActorNameOrLabel(),
				*FString::Join(Unknown, TEXT(", "))));
		}
	}
	else if (bAll)
	{
		for (TFieldIterator<FProperty> It(CompClass); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible))
			{
				continue;
			}
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Comp);
			PropsObj->SetField(Prop->GetName(), FMonolithReflectionReader::PropertyToJsonValue(Prop, ValuePtr, Comp));
			Reported.Add(Prop->GetName());
		}
	}
	else
	{
		TArray<FString> Missing;
		for (const FString& Name : LoadReadbackKeys(Token))
		{
			FProperty* Prop = FMonolithReflectionWalker::FindPropertyForwarding(CompClass, Name);
			if (!Prop)
			{
				// A default-set name that no longer exists on this engine version.
				// Reported, not fatal — the read-back set is data, not a contract.
				Missing.Add(Name);
				continue;
			}
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Comp);
			PropsObj->SetField(Prop->GetName(), FMonolithReflectionReader::PropertyToJsonValue(Prop, ValuePtr, Comp));
			Reported.Add(Prop->GetName());
		}
		if (Missing.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> MissingArr;
			for (const FString& M : Missing) { MissingArr.Add(MakeShared<FJsonValueString>(M)); }
			PropsObj->SetField(TEXT("__unresolved"), MakeShared<FJsonValueArray>(MissingArr));
		}
	}

	const FVector Loc = Actor->GetActorLocation();
	const FRotator Rot = Actor->GetActorRotation();

	auto LocArr = TArray<TSharedPtr<FJsonValue>>{
		MakeShared<FJsonValueNumber>(Loc.X), MakeShared<FJsonValueNumber>(Loc.Y), MakeShared<FJsonValueNumber>(Loc.Z) };
	auto RotArr = TArray<TSharedPtr<FJsonValue>>{
		MakeShared<FJsonValueNumber>(Rot.Pitch), MakeShared<FJsonValueNumber>(Rot.Yaw), MakeShared<FJsonValueNumber>(Rot.Roll) };

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
	Result->SetStringField(TEXT("actor_class"), Actor->GetClass()->GetName());
	Result->SetStringField(TEXT("light_type"), Token);
	Result->SetStringField(TEXT("component_name"), Comp->GetFName().ToString());
	Result->SetStringField(TEXT("component_class"), CompClass->GetName());
	Result->SetArrayField(TEXT("location"), LocArr);
	Result->SetArrayField(TEXT("rotation"), RotArr);
	Result->SetNumberField(TEXT("property_count"), Reported.Num());
	Result->SetObjectField(TEXT("properties"), PropsObj);
	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// mesh.list_light_presets
// ============================================================================

FMonolithActionResult FMonolithMeshLightActions::ListLightPresets(const TSharedPtr<FJsonObject>& Params)
{
	FString TypeFilter;
	Params->TryGetStringField(TEXT("type"), TypeFilter);
	if (!TypeFilter.IsEmpty())
	{
		FString TypeError;
		if (!ResolveLightActorClass(TypeFilter, TypeError))
		{
			return FMonolithActionResult::Error(TypeError);
		}
		TypeFilter = MonolithLightTypes::Normalise(TypeFilter);
	}

	bool bIncludeProperties = true;
	Params->TryGetBoolField(TEXT("include_properties"), bIncludeProperties);

	TArray<FString> Warnings;
	const TMap<FString, FPreset> Presets = LoadPresets(Warnings);

	TArray<FString> Names;
	Presets.GetKeys(Names);
	Names.Sort();

	TArray<TSharedPtr<FJsonValue>> Out;
	for (const FString& Name : Names)
	{
		const FPreset& P = Presets[Name];
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

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("presets"), Out);
	Result->SetNumberField(TEXT("count"), Out.Num());
	Result->SetNumberField(TEXT("total_count"), Presets.Num());
	Result->SetStringField(TEXT("builtin_file"), GetBuiltinPresetFile());
	Result->SetStringField(TEXT("user_directory"), GetUserPresetDirectory());
	TArray<TSharedPtr<FJsonValue>> TypeArr;
	for (const FString& T : GetLightTypeTokens()) { TypeArr.Add(MakeShared<FJsonValueString>(T)); }
	Result->SetArrayField(TEXT("light_types"), TypeArr);
	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Warnings) { WarnArr.Add(MakeShared<FJsonValueString>(W)); }
		Result->SetArrayField(TEXT("warnings"), WarnArr);
	}
	return FMonolithActionResult::Success(Result);
}
