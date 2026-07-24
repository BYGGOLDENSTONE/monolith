#include "MonolithMeshLayoutActions.h"

#include "MonolithMeshAtmosphereActions.h"
#include "MonolithMeshLightActions.h"
#include "MonolithMeshSceneActions.h"
#include "MonolithMeshVolumeActions.h"
#include "MonolithMeshUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithToolRegistry.h"

#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// ============================================================================
// Kind table
//
// Token -> which EXISTING action places this kind. This is the one mapping that
// cannot live in data: a JSON file naming a C++ handler would be a string lookup
// with no compile-time checking. Everything else about an entry is data and is
// forwarded to the named action untouched.
// ============================================================================

namespace MonolithLayoutInternal
{
	/** Entry keys the layout system owns and therefore never forwards. */
	static const TCHAR* ReservedKeys[] = {
		TEXT("id"),       // identity
		TEXT("kind"),     // dispatch
		TEXT("name"),     // label — assigned by the layout after placement
		TEXT("label"),    // alias of name
		TEXT("comment"),  // author note
	};

	/** Entry keys rewritten before forwarding: layout spelling -> action parameter. */
	struct FKeyRewrite { const TCHAR* From; const TCHAR* To; };
	static const FKeyRewrite KeyRewrites[] = {
		{ TEXT("class"), TEXT("class_or_mesh") },
	};

	/** Keys that must be a 3+ number array when present. */
	static const TCHAR* VectorKeys[] = {
		TEXT("location"), TEXT("rotation"), TEXT("scale"), TEXT("extent"),
	};

	static bool IsReserved(const FString& Key)
	{
		for (const TCHAR* K : ReservedKeys)
		{
			if (Key.Equals(K, ESearchCase::IgnoreCase)) { return true; }
		}
		return false;
	}

	/** Ids name actors, tags, folders and files — keep them boring on purpose. */
	static bool IsValidIdentifier(const FString& Id)
	{
		if (Id.IsEmpty() || Id.Len() > 96) { return false; }
		for (const TCHAR C : Id)
		{
			const bool bOk = FChar::IsAlnum(C) || C == TEXT('_') || C == TEXT('-') || C == TEXT('.');
			if (!bOk) { return false; }
		}
		return true;
	}

	static TSharedPtr<FJsonObject> GetActionSchema(const FString& Namespace, const FString& Action)
	{
		for (const FMonolithActionInfo& Info : FMonolithToolRegistry::Get().GetActions(Namespace))
		{
			if (Info.Action == Action)
			{
				return Info.ParamSchema;
			}
		}
		return nullptr;
	}

	/** Schema-declared required params that Params does not supply (canonical or alias). */
	static TArray<FString> FindMissingRequired(
		const TSharedPtr<FJsonObject>& Schema, const TSharedPtr<FJsonObject>& Params)
	{
		TArray<FString> Missing;
		if (!Schema.IsValid() || !Params.IsValid())
		{
			return Missing;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Schema->Values)
		{
			const TSharedPtr<FJsonObject>* Def = nullptr;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(Def) || !Def) { continue; }

			bool bRequired = false;
			(*Def)->TryGetBoolField(TEXT("required"), bRequired);
			if (!bRequired) { continue; }

			if (Params->HasField(Pair.Key)) { continue; }

			bool bAliasPresent = false;
			const TArray<TSharedPtr<FJsonValue>>* Aliases = nullptr;
			if ((*Def)->TryGetArrayField(TEXT("aliases"), Aliases) && Aliases)
			{
				for (const TSharedPtr<FJsonValue>& A : *Aliases)
				{
					if (A.IsValid() && Params->HasField(A->AsString())) { bAliasPresent = true; break; }
				}
			}
			if (!bAliasPresent) { Missing.Add(Pair.Key); }
		}
		Missing.Sort();
		return Missing;
	}

	static TArray<FString> SchemaKeys(const TSharedPtr<FJsonObject>& Schema)
	{
		TArray<FString> Keys;
		if (Schema.IsValid())
		{
			Schema->Values.GetKeys(Keys);
			Keys.Sort();
		}
		return Keys;
	}

	static TArray<TSharedPtr<FJsonValue>> VectorToJson(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}

	/** Read an optional [x,y,z] (or {x,y,z}) field. Returns false only when present-but-malformed. */
	static bool ReadOptionalVector(
		const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& Out, bool& bOutPresent)
	{
		bOutPresent = Obj.IsValid() && Obj->HasField(Key);
		if (!bOutPresent) { return true; }
		return MonolithMeshUtils::ParseVector(Obj, Key, Out);
	}

	/** Canonical atmosphere token for a user-supplied type string. Empty when unresolvable. */
	static FString CanonicalAtmosphereToken(const FString& TypeStr)
	{
		FString Err;
		UClass* Wanted = FMonolithMeshAtmosphereActions::ResolveAtmosphereActorClass(TypeStr, Err);
		if (!Wanted) { return FString(); }
		for (const FString& Token : FMonolithMeshAtmosphereActions::GetAtmosphereTokens())
		{
			FString TokenErr;
			if (FMonolithMeshAtmosphereActions::ResolveAtmosphereActorClass(Token, TokenErr) == Wanted)
			{
				return Token;
			}
		}
		return FString();
	}

	/**
	 * Records the actors spawned while it is alive.
	 *
	 * The delegating actions return an `actor_name`, but a name lookup can in
	 * principle collide with a pre-existing actor that happens to carry the same
	 * label. Listening to the world's spawn delegate identifies the actor a child
	 * action just created EXACTLY, which is what the layout tags depend on.
	 */
	struct FSpawnWatcher
	{
		UWorld* World = nullptr;
		FDelegateHandle Handle;
		TArray<AActor*> Spawned;

		explicit FSpawnWatcher(UWorld* InWorld)
			: World(InWorld)
		{
			if (World)
			{
				Handle = World->AddOnActorSpawnedHandler(
					FOnActorSpawned::FDelegate::CreateRaw(this, &FSpawnWatcher::OnSpawned));
			}
		}

		~FSpawnWatcher()
		{
			if (World && Handle.IsValid())
			{
				World->RemoveOnActorSpawnedHandler(Handle);
			}
		}

		void OnSpawned(AActor* Actor) { Spawned.Add(Actor); }
	};

	/** Restores FMonolithMeshSceneActions::bBatchTransactionActive on scope exit. */
	struct FScopedBatchFlag
	{
		bool bPrevious;
		explicit FScopedBatchFlag(bool bNew)
			: bPrevious(FMonolithMeshSceneActions::bBatchTransactionActive)
		{
			FMonolithMeshSceneActions::bBatchTransactionActive = bNew;
		}
		~FScopedBatchFlag()
		{
			FMonolithMeshSceneActions::bBatchTransactionActive = bPrevious;
		}
	};
}

const TCHAR* FMonolithMeshLayoutActions::LayoutTagPrefix() { return TEXT("Monolith.Layout:"); }
const TCHAR* FMonolithMeshLayoutActions::EntryTagPrefix()  { return TEXT("Monolith.LayoutEntry:"); }

FName FMonolithMeshLayoutActions::MakeLayoutTag(const FString& LayoutId)
{
	return FName(*(FString(LayoutTagPrefix()) + LayoutId));
}

FName FMonolithMeshLayoutActions::MakeEntryTag(const FString& EntryId)
{
	return FName(*(FString(EntryTagPrefix()) + EntryId));
}

TArray<AActor*> FMonolithMeshLayoutActions::FindLayoutActors(UWorld* World, const FString& LayoutId)
{
	TArray<AActor*> Found;
	if (!World || LayoutId.IsEmpty())
	{
		return Found;
	}
	const FName Wanted = MakeLayoutTag(LayoutId);
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!IsValid(Actor)) { continue; }
		for (const FName& Tag : Actor->Tags)
		{
			if (MonolithMeshUtils::MatchTag(Tag, Wanted))
			{
				Found.Add(Actor);
				break;
			}
		}
	}
	return Found;
}

FString FMonolithMeshLayoutActions::GetEntryId(const AActor* Actor)
{
	if (!Actor) { return FString(); }
	const FString Prefix = EntryTagPrefix();
	for (const FName& Tag : Actor->Tags)
	{
		const FString S = Tag.ToString();
		if (S.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			return S.RightChop(Prefix.Len());
		}
	}
	return FString();
}

const TArray<FMonolithMeshLayoutActions::FKind>& FMonolithMeshLayoutActions::GetKinds()
{
	static const TArray<FKind> Kinds = {
		{ TEXT("actor"),      TEXT("mesh"), TEXT("spawn_actor")      },
		{ TEXT("light"),      TEXT("mesh"), TEXT("place_light")      },
		{ TEXT("atmosphere"), TEXT("mesh"), TEXT("spawn_atmosphere") },
		{ TEXT("volume"),     TEXT("mesh"), TEXT("spawn_volume")     },
	};
	return Kinds;
}

const FMonolithMeshLayoutActions::FKind* FMonolithMeshLayoutActions::FindKind(const FString& Token)
{
	const FString Norm = Token.TrimStartAndEnd().ToLower();
	for (const FKind& K : GetKinds())
	{
		if (Norm == K.Token) { return &K; }
	}
	return nullptr;
}

const FMonolithMeshJsonPresets& FMonolithMeshLayoutActions::Library()
{
	static const FMonolithMeshJsonPresets Instance(
		TEXT("MonolithLevelLayouts.json"), TEXT("LevelLayouts"), TEXT("level layout"));
	return Instance;
}

TMap<FString, FMonolithNamedJsonObject> FMonolithMeshLayoutActions::LoadLayouts(TArray<FString>& OutWarnings)
{
	return Library().LoadNamedObjects(TEXT("layouts"), OutWarnings);
}

bool FMonolithMeshLayoutActions::RequireEditorWorld(const UWorld* World, FString& OutError)
{
	if (World) { OutError.Reset(); return true; }
	OutError = TEXT("No level is open in the editor, so there is nothing to place a layout into. "
					"Open or create a level first (editor.open_level), then re-run this action.");
	return false;
}

// ============================================================================
// Phase 1 — resolve + validate, without touching the world
// ============================================================================

bool FMonolithMeshLayoutActions::ResolveLayout(
	const FString& LayoutId,
	const TSharedPtr<FJsonObject>& Body,
	const FVector& Origin,
	const FString& FolderOverride,
	TArray<FResolvedEntry>& OutEntries,
	TArray<FString>& OutProblems)
{
	using namespace MonolithLayoutInternal;

	OutEntries.Reset();
	OutProblems.Reset();

	if (!IsValidIdentifier(LayoutId))
	{
		OutProblems.Add(FString::Printf(
			TEXT("layout id '%s' is not usable — ids may only contain letters, digits, '_', '-' and '.' "
				 "(they name actor tags, an outliner folder and a file)."), *LayoutId));
		return false;
	}

	if (!Body.IsValid())
	{
		OutProblems.Add(TEXT("layout body is missing or is not a JSON object."));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* EntriesArr = nullptr;
	if (!Body->TryGetArrayField(TEXT("entries"), EntriesArr) || !EntriesArr)
	{
		if (Body->HasField(TEXT("layouts")))
		{
			OutProblems.Add(TEXT("this looks like a whole layout FILE, not one layout. Pass the object under "
								 "\"layouts\".<id> as layout_json, or name the layout with the `layout` param."));
		}
		else
		{
			OutProblems.Add(TEXT("layout has no \"entries\" array. A layout is { \"entries\": [ {id, kind, ...}, ... ] }."));
		}
		return false;
	}

	if (EntriesArr->Num() == 0)
	{
		OutProblems.Add(TEXT("layout \"entries\" is empty — nothing to place."));
		return false;
	}

	const FString Folder = !FolderOverride.IsEmpty()
		? FolderOverride
		: FString::Printf(TEXT("Monolith/Layouts/%s"), *LayoutId);

	TSet<FString> SeenIds;

	for (int32 Index = 0; Index < EntriesArr->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject>* EntryObjPtr = nullptr;
		if (!(*EntriesArr)[Index].IsValid() || !(*EntriesArr)[Index]->TryGetObject(EntryObjPtr) || !EntryObjPtr)
		{
			OutProblems.Add(FString::Printf(TEXT("entry #%d: not a JSON object."), Index));
			continue;
		}
		const TSharedPtr<FJsonObject>& Entry = *EntryObjPtr;

		// --- id ---------------------------------------------------------
		FString EntryId;
		Entry->TryGetStringField(TEXT("id"), EntryId);
		EntryId = EntryId.TrimStartAndEnd();
		if (!IsValidIdentifier(EntryId))
		{
			OutProblems.Add(FString::Printf(
				TEXT("entry #%d: missing or invalid \"id\" ('%s'). Every entry needs a stable id — it is what "
					 "makes re-applying the layout update that entry instead of duplicating it. Allowed "
					 "characters: letters, digits, '_', '-', '.'."), Index, *EntryId));
			continue;
		}
		if (SeenIds.Contains(EntryId))
		{
			OutProblems.Add(FString::Printf(
				TEXT("entry #%d: duplicate id '%s' — ids must be unique inside a layout."), Index, *EntryId));
			continue;
		}
		SeenIds.Add(EntryId);

		// --- kind -------------------------------------------------------
		FString KindToken;
		Entry->TryGetStringField(TEXT("kind"), KindToken);
		const FKind* Kind = FindKind(KindToken);
		if (!Kind)
		{
			TArray<FString> Tokens;
			for (const FKind& K : GetKinds()) { Tokens.Add(K.Token); }
			OutProblems.Add(FString::Printf(
				TEXT("'%s': unknown kind '%s'. Valid kinds: %s."),
				*EntryId, *KindToken, *FString::Join(Tokens, TEXT(", "))));
			continue;
		}

		const TSharedPtr<FJsonObject> Schema = GetActionSchema(Kind->Namespace, Kind->Action);
		if (!Schema.IsValid())
		{
			OutProblems.Add(FString::Printf(
				TEXT("'%s': kind '%s' places through %s.%s, which is not registered in this editor "
					 "(is the Mesh module enabled?)."),
				*EntryId, *Kind->Token, *Kind->Namespace, *Kind->Action));
			continue;
		}

		// --- build the forwarded params ---------------------------------
		TSharedPtr<FJsonObject> Forward = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Entry->Values)
		{
			if (IsReserved(Pair.Key)) { continue; }
			FString Key = Pair.Key;
			for (const FKeyRewrite& R : KeyRewrites)
			{
				if (Key.Equals(R.From, ESearchCase::IgnoreCase)) { Key = R.To; break; }
			}
			Forward->SetField(Key, Pair.Value);
		}

		// --- vector shapes ----------------------------------------------
		bool bVectorProblem = false;
		for (const TCHAR* VKey : VectorKeys)
		{
			FVector Dummy;
			bool bPresent = false;
			if (!ReadOptionalVector(Forward, VKey, Dummy, bPresent))
			{
				OutProblems.Add(FString::Printf(
					TEXT("'%s': \"%s\" must be [x, y, z] numbers."), *EntryId, VKey));
				bVectorProblem = true;
			}
		}
		if (bVectorProblem) { continue; }

		// --- required + unknown params of the target action -------------
		const TArray<FString> Missing = FindMissingRequired(Schema, Forward);
		if (Missing.Num() > 0)
		{
			OutProblems.Add(FString::Printf(
				TEXT("'%s': missing required field(s) [%s] for kind '%s' (%s.%s)."),
				*EntryId, *FString::Join(Missing, TEXT(", ")), *Kind->Token, *Kind->Namespace, *Kind->Action));
			continue;
		}

		const TArray<FString> Unknown = FMonolithParamSchema::FindUnknownKeys(Schema, Forward);
		if (Unknown.Num() > 0)
		{
			OutProblems.Add(FString::Printf(
				TEXT("'%s': unknown field(s) [%s] for kind '%s'. %s.%s accepts: %s (plus the layout's own "
					 "id, kind, name/label, comment; 'class' is accepted as a spelling of 'class_or_mesh')."),
				*EntryId, *FString::Join(Unknown, TEXT(", ")), *Kind->Token,
				*Kind->Namespace, *Kind->Action, *FString::Join(SchemaKeys(Schema), TEXT(", "))));
			continue;
		}

		// --- kind-specific semantics ------------------------------------
		FString KindProblem;
		if (Kind->Token == TEXT("actor"))
		{
			FString ClassOrMesh;
			Forward->TryGetStringField(TEXT("class_or_mesh"), ClassOrMesh);
			UClass* Cls = nullptr;
			UStaticMesh* Mesh = nullptr;
			FString ResolveError;
			if (!FMonolithMeshSceneActions::ResolveSpawnTarget(ClassOrMesh, Cls, Mesh, ResolveError))
			{
				KindProblem = ResolveError;
			}
			else
			{
				// The free-form `properties` / `component_properties` bags are checked
				// against the class default object here, in phase 1. Without this a
				// mistyped UPROPERTY name would only be caught mid-placement, and
				// all-or-nothing would then have to unwind a half-built layout instead
				// of never starting one.
				UClass* EffectiveClass = Mesh ? AStaticMeshActor::StaticClass() : Cls;
				FString PropertyError;
				if (!FMonolithMeshSceneActions::ValidateSpawnProperties(Forward, EffectiveClass, PropertyError))
				{
					KindProblem = PropertyError;
				}
			}
		}
		else if (Kind->Token == TEXT("volume"))
		{
			FString TypeStr;
			Forward->TryGetStringField(TEXT("type"), TypeStr);
			FString TypeError;
			UClass* VolumeClass = FMonolithMeshVolumeActions::ResolveVolumeClass(TypeStr, TypeError);
			if (!VolumeClass)
			{
				KindProblem = TypeError;
			}
			else
			{
				// spawn_volume's `properties` are curated aliases, not UPROPERTY names,
				// so they are checked against its honoured-key table rather than through
				// the reflection walker. Same phase-1 guarantee either way.
				FString PropertyError;
				if (!FMonolithMeshVolumeActions::ValidateVolumeProperties(VolumeClass, Forward, PropertyError))
				{
					KindProblem = PropertyError;
				}
			}
		}
		else if (Kind->Token == TEXT("light"))
		{
			FString TypeStr;
			Forward->TryGetStringField(TEXT("type"), TypeStr);
			FString TypeError;
			UClass* LightClass = FMonolithMeshLightActions::ResolveLightActorClass(TypeStr, TypeError);
			if (!LightClass)
			{
				KindProblem = TypeError;
			}
			else
			{
				FString PresetName;
				if (Forward->TryGetStringField(TEXT("preset"), PresetName) && !PresetName.IsEmpty())
				{
					FMonolithMeshLightActions::FPreset Preset;
					FString PresetError;
					if (!FMonolithMeshLightActions::ResolvePreset(PresetName, Preset, PresetError))
					{
						KindProblem = PresetError;
					}
					else if (!Preset.TypeToken.IsEmpty())
					{
						FString PresetTypeError;
						UClass* PresetClass =
							FMonolithMeshLightActions::ResolveLightActorClass(Preset.TypeToken, PresetTypeError);
						if (PresetClass && PresetClass != LightClass)
						{
							KindProblem = FString::Printf(
								TEXT("preset '%s' targets light type '%s', but this entry is type '%s'."),
								*PresetName, *Preset.TypeToken, *TypeStr);
						}
					}
				}
			}
		}
		else if (Kind->Token == TEXT("atmosphere"))
		{
			FString TypeStr;
			Forward->TryGetStringField(TEXT("type"), TypeStr);
			FString TypeError;
			if (!FMonolithMeshAtmosphereActions::ResolveAtmosphereActorClass(TypeStr, TypeError))
			{
				KindProblem = TypeError;
			}
			else
			{
				FString PresetName;
				if (Forward->TryGetStringField(TEXT("preset"), PresetName) && !PresetName.IsEmpty())
				{
					FMonolithJsonPreset Preset;
					FString PresetError;
					if (!FMonolithMeshAtmosphereActions::Presets().ResolvePreset(PresetName, Preset, PresetError))
					{
						KindProblem = PresetError;
					}
					else
					{
						const FString Canonical = CanonicalAtmosphereToken(TypeStr);
						if (!Preset.TypeToken.IsEmpty() && !Canonical.IsEmpty() &&
							!FMonolithMeshAtmosphereActions::IsPresetCompatible(Preset.TypeToken, Canonical))
						{
							KindProblem = FString::Printf(
								TEXT("preset '%s' targets '%s', but this entry is type '%s'."),
								*PresetName, *Preset.TypeToken, *Canonical);
						}
					}
				}
			}
		}

		if (!KindProblem.IsEmpty())
		{
			OutProblems.Add(FString::Printf(TEXT("'%s': %s"), *EntryId, *KindProblem));
			continue;
		}

		// --- injections: world offset + outliner folder ------------------
		{
			FVector Location = FVector::ZeroVector;
			bool bHasLocation = false;
			ReadOptionalVector(Forward, TEXT("location"), Location, bHasLocation);
			if (bHasLocation || !Origin.IsNearlyZero())
			{
				if (Schema->HasField(TEXT("location")))
				{
					Forward->SetArrayField(TEXT("location"), VectorToJson(Location + Origin));
				}
			}
		}
		if (!Forward->HasField(TEXT("folder")) && Schema->HasField(TEXT("folder")))
		{
			Forward->SetStringField(TEXT("folder"), Folder);
		}

		// --- label -------------------------------------------------------
		FString Label;
		if (!Entry->TryGetStringField(TEXT("name"), Label) || Label.IsEmpty())
		{
			Entry->TryGetStringField(TEXT("label"), Label);
		}
		if (Label.IsEmpty())
		{
			Label = FString::Printf(TEXT("%s.%s"), *LayoutId, *EntryId);
		}

		FResolvedEntry Resolved;
		Resolved.EntryId = EntryId;
		Resolved.KindToken = Kind->Token;
		Resolved.Namespace = Kind->Namespace;
		Resolved.Action = Kind->Action;
		Resolved.Params = Forward;
		Resolved.Label = Label;
		OutEntries.Add(MoveTemp(Resolved));
	}

	return OutProblems.Num() == 0;
}

// ============================================================================
// Shared parameter plumbing for the actions
// ============================================================================

namespace MonolithLayoutInternal
{
	struct FRequest
	{
		FString LayoutId;
		FString Source;          // "builtin/user file path" or "inline"
		TSharedPtr<FJsonObject> Body;
		FVector Origin = FVector::ZeroVector;
		FString Folder;
	};

	/**
	 * Resolve `layout` / `layout_json` / `origin` / `folder` into a request.
	 * Returns false + an actionable OutError.
	 */
	static bool ReadRequest(const TSharedPtr<FJsonObject>& Params, FRequest& Out, FString& OutError)
	{
		Params->TryGetStringField(TEXT("layout"), Out.LayoutId);
		Out.LayoutId = Out.LayoutId.TrimStartAndEnd();

		const TSharedPtr<FJsonObject>* InlinePtr = nullptr;
		if (Params->TryGetObjectField(TEXT("layout_json"), InlinePtr) && InlinePtr && (*InlinePtr).IsValid())
		{
			Out.Body = *InlinePtr;
			Out.Source = TEXT("inline");
			if (Out.LayoutId.IsEmpty())
			{
				Out.Body->TryGetStringField(TEXT("id"), Out.LayoutId);
				Out.LayoutId = Out.LayoutId.TrimStartAndEnd();
			}
			if (Out.LayoutId.IsEmpty())
			{
				OutError = TEXT("layout_json needs an id: pass the `layout` param, or set \"id\" inside "
								"layout_json. The id is the identity every placed actor is tagged with, so "
								"apply/remove/re-apply can find its own actors.");
				return false;
			}
		}
		else
		{
			if (Out.LayoutId.IsEmpty())
			{
				OutError = TEXT("Pass either `layout` (a named layout — see mesh.list_level_layouts) or "
								"`layout_json` (a layout document body, for a layout you just generated).");
				return false;
			}

			TArray<FString> Warnings;
			const TMap<FString, FMonolithNamedJsonObject> Layouts = FMonolithMeshLayoutActions::LoadLayouts(Warnings);
			const FMonolithNamedJsonObject* Found = Layouts.Find(Out.LayoutId);
			if (!Found)
			{
				for (const TPair<FString, FMonolithNamedJsonObject>& Pair : Layouts)
				{
					if (Pair.Key.Equals(Out.LayoutId, ESearchCase::IgnoreCase)) { Found = &Pair.Value; break; }
				}
			}
			if (!Found)
			{
				TArray<FString> Available;
				Layouts.GetKeys(Available);
				Available.Sort();
				OutError = FString::Printf(
					TEXT("Unknown level layout '%s'. Available (%d): %s. Add your own JSON to %s, or pass the "
						 "document inline with `layout_json`."),
					*Out.LayoutId, Available.Num(),
					Available.Num() > 0 ? *FString::Join(Available, TEXT(", ")) : TEXT("<none loaded>"),
					*FMonolithMeshLayoutActions::Library().GetUserDirectory());
				if (Warnings.Num() > 0)
				{
					OutError += FString::Printf(TEXT(" Load warnings: %s"), *FString::Join(Warnings, TEXT("; ")));
				}
				return false;
			}
			Out.LayoutId = Found->Name;
			Out.Body = Found->Object;
			Out.Source = Found->SourceFile;
		}

		// Origin: document origin + caller origin (both optional, they add).
		FVector DocOrigin = FVector::ZeroVector;
		bool bPresent = false;
		if (!ReadOptionalVector(Out.Body, TEXT("origin"), DocOrigin, bPresent))
		{
			OutError = TEXT("Layout \"origin\" must be [x, y, z] numbers.");
			return false;
		}
		FVector CallOrigin = FVector::ZeroVector;
		if (!ReadOptionalVector(Params, TEXT("origin"), CallOrigin, bPresent))
		{
			OutError = TEXT("Param `origin` must be [x, y, z] numbers.");
			return false;
		}
		Out.Origin = DocOrigin + CallOrigin;

		Params->TryGetStringField(TEXT("folder"), Out.Folder);
		if (Out.Folder.IsEmpty())
		{
			Out.Body->TryGetStringField(TEXT("folder"), Out.Folder);
		}

		return true;
	}

	static FString JoinProblems(const TArray<FString>& Problems)
	{
		return FString::Join(Problems, TEXT("; "));
	}
}

// ============================================================================
// Registration
// ============================================================================

void FMonolithMeshLayoutActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("mesh"), TEXT("list_level_layouts"),
		TEXT("List the data-driven level layouts available to mesh.apply_level_layout. Built-ins ship in "
			 "Plugins/Monolith/Config/MonolithLevelLayouts.json; drop your own JSON in "
			 "Plugins/Monolith/Saved/Monolith/LevelLayouts/ (or write one with mesh.save_level_layout) to add "
			 "or override layouts without rebuilding. Reports how many of each layout's actors are currently "
			 "in the open level."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshLayoutActions::ListLevelLayouts),
		FParamSchemaBuilder()
			.Optional(TEXT("validate"), TEXT("boolean"),
				TEXT("Resolve every entry of every layout against the live engine and report problem counts"),
				TEXT("true"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("describe_level_layout"),
		TEXT("Resolve one level layout WITHOUT touching the level: per-entry target action, resolved class/type, "
			 "and any problem (unknown class, missing asset, bad preset, unknown field). Also reports which "
			 "entries are already in the open level and which tagged actors are orphans (in the level but no "
			 "longer in the document). This is the dry run for mesh.apply_level_layout."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshLayoutActions::DescribeLevelLayout),
		FParamSchemaBuilder()
			.Optional(TEXT("layout"), TEXT("string"), TEXT("Layout id (see mesh.list_level_layouts)"))
			.Optional(TEXT("layout_json"), TEXT("object"),
				TEXT("Layout body {description?, folder?, origin?, entries:[...]} — for a layout you generated but have not saved"))
			.Optional(TEXT("origin"), TEXT("array"), TEXT("Extra world offset [x, y, z] added to the layout's own origin"), TEXT("[0,0,0]"))
			.OptionalAssetPath(TEXT("folder"), TEXT("Outliner folder override (default Monolith/Layouts/<layout>)"))
			.Optional(TEXT("include_params"), TEXT("boolean"), TEXT("Include the exact parameters each entry would be placed with"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("apply_level_layout"),
		TEXT("Place a data-driven level layout into the open level. ALL-OR-NOTHING: every entry is resolved "
			 "first and a single bad entry fails the call with the level untouched and EVERY problem listed. "
			 "Idempotent: each placed actor is tagged 'Monolith.Layout:<layout>' + 'Monolith.LayoutEntry:<id>', "
			 "and the default on_existing='replace' removes the previous instance of that layout (only after "
			 "the new one is fully built), so applying twice never duplicates. Entries reference light and "
			 "atmosphere PRESETS by name rather than restating property values."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshLayoutActions::ApplyLevelLayout),
		FParamSchemaBuilder()
			.Optional(TEXT("layout"), TEXT("string"), TEXT("Layout id (see mesh.list_level_layouts)"))
			.Optional(TEXT("layout_json"), TEXT("object"),
				TEXT("Layout body {id?, description?, folder?, origin?, entries:[...]} — apply a layout you just generated"))
			.Optional(TEXT("origin"), TEXT("array"), TEXT("Extra world offset [x, y, z] added to the layout's own origin"), TEXT("[0,0,0]"))
			.OptionalAssetPath(TEXT("folder"), TEXT("Outliner folder override (default Monolith/Layouts/<layout>)"))
			.Optional(TEXT("on_existing"), TEXT("string"),
				TEXT("'replace' (default: remove the previous instance of this layout, then place the document) or "
					 "'skip' (place only entries that have no tagged actor yet; leave everything else alone)"),
				TEXT("replace"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Validate and report what would happen; place nothing"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("remove_level_layout"),
		TEXT("Delete every actor tagged as belonging to a layout. Works purely from the actor tags, so it "
			 "removes a previously applied layout even if its document was changed or deleted. This is the "
			 "undo for mesh.apply_level_layout."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshLayoutActions::RemoveLevelLayout),
		FParamSchemaBuilder()
			.Required(TEXT("layout"), TEXT("string"), TEXT("Layout id whose actors should be removed"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("Report what would be removed; delete nothing"), TEXT("false"))
			.Build());

	Registry.RegisterAction(TEXT("mesh"), TEXT("save_level_layout"),
		TEXT("Validate a generated layout and write it to Plugins/Monolith/Saved/Monolith/LevelLayouts/<layout>.json "
			 "so it becomes a named layout for mesh.apply_level_layout. The layout is resolved against the live "
			 "engine first — an invalid document is never written."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshLayoutActions::SaveLevelLayout),
		FParamSchemaBuilder()
			.Required(TEXT("layout"), TEXT("string"), TEXT("Layout id — also the file name"))
			.Required(TEXT("layout_json"), TEXT("object"), TEXT("Layout body {description?, folder?, origin?, entries:[...]}"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("Overwrite an existing user layout file with this id"), TEXT("false"))
			.Build());

	// The reverse direction (level -> document). Lives in MonolithMeshLayoutCapture.cpp.
	RegisterCaptureAction(Registry);
}

// ============================================================================
// 1. list_level_layouts
// ============================================================================

FMonolithActionResult FMonolithMeshLayoutActions::ListLevelLayouts(const TSharedPtr<FJsonObject>& Params)
{
	bool bValidate = true;
	Params->TryGetBoolField(TEXT("validate"), bValidate);

	TArray<FString> Warnings;
	const TMap<FString, FMonolithNamedJsonObject> Layouts = LoadLayouts(Warnings);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();

	TArray<FString> Ids;
	Layouts.GetKeys(Ids);
	Ids.Sort();

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FString& Id : Ids)
	{
		const FMonolithNamedJsonObject& Named = Layouts[Id];
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("layout"), Id);

		FString Description;
		Named.Object->TryGetStringField(TEXT("description"), Description);
		Obj->SetStringField(TEXT("description"), Description);
		Obj->SetStringField(TEXT("source_file"), Named.SourceFile);

		int32 EntryCount = 0;
		TMap<FString, int32> KindCounts;
		const TArray<TSharedPtr<FJsonValue>>* EntriesArr = nullptr;
		if (Named.Object->TryGetArrayField(TEXT("entries"), EntriesArr) && EntriesArr)
		{
			EntryCount = EntriesArr->Num();
			for (const TSharedPtr<FJsonValue>& V : *EntriesArr)
			{
				const TSharedPtr<FJsonObject>* E = nullptr;
				if (!V.IsValid() || !V->TryGetObject(E) || !E) { continue; }
				FString KindToken;
				(*E)->TryGetStringField(TEXT("kind"), KindToken);
				KindCounts.FindOrAdd(KindToken.ToLower())++;
			}
		}
		Obj->SetNumberField(TEXT("entry_count"), EntryCount);

		auto KindsObj = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Pair : KindCounts)
		{
			KindsObj->SetNumberField(Pair.Key, Pair.Value);
		}
		Obj->SetObjectField(TEXT("kinds"), KindsObj);

		if (bValidate)
		{
			TArray<FResolvedEntry> Resolved;
			TArray<FString> Problems;
			const bool bOk = ResolveLayout(Id, Named.Object, FVector::ZeroVector, FString(), Resolved, Problems);
			Obj->SetBoolField(TEXT("valid"), bOk);
			Obj->SetNumberField(TEXT("problem_count"), Problems.Num());
			if (!bOk)
			{
				TArray<TSharedPtr<FJsonValue>> ProblemArr;
				for (const FString& P : Problems) { ProblemArr.Add(MakeShared<FJsonValueString>(P)); }
				Obj->SetArrayField(TEXT("problems"), ProblemArr);
			}
		}

		if (World)
		{
			Obj->SetNumberField(TEXT("actors_in_level"), FindLayoutActors(World, Id).Num());
		}

		Arr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("layouts"), Arr);
	Result->SetNumberField(TEXT("count"), Arr.Num());
	Result->SetStringField(TEXT("builtin_file"), Library().GetBuiltinFile());
	Result->SetStringField(TEXT("user_directory"), Library().GetUserDirectory());
	Result->SetBoolField(TEXT("level_open"), World != nullptr);

	if (Warnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarnArr;
		for (const FString& W : Warnings) { WarnArr.Add(MakeShared<FJsonValueString>(W)); }
		Result->SetArrayField(TEXT("warnings"), WarnArr);
	}

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 2. describe_level_layout
// ============================================================================

FMonolithActionResult FMonolithMeshLayoutActions::DescribeLevelLayout(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithLayoutInternal;

	FRequest Request;
	FString Error;
	if (!ReadRequest(Params, Request, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	bool bIncludeParams = false;
	Params->TryGetBoolField(TEXT("include_params"), bIncludeParams);

	TArray<FResolvedEntry> Resolved;
	TArray<FString> Problems;
	const bool bValid = ResolveLayout(
		Request.LayoutId, Request.Body, Request.Origin, Request.Folder, Resolved, Problems);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();

	// Which entry ids are already in the level, and which tagged actors are orphans.
	TMap<FString, FString> PresentEntryToActor;
	TArray<TSharedPtr<FJsonValue>> OrphanArr;
	if (World)
	{
		TSet<FString> DocumentIds;
		for (const FResolvedEntry& E : Resolved) { DocumentIds.Add(E.EntryId); }

		for (AActor* Actor : FindLayoutActors(World, Request.LayoutId))
		{
			const FString EntryId = GetEntryId(Actor);
			if (EntryId.IsEmpty() || !DocumentIds.Contains(EntryId))
			{
				auto Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
				Obj->SetStringField(TEXT("entry"), EntryId);
				OrphanArr.Add(MakeShared<FJsonValueObject>(Obj));
			}
			else
			{
				PresentEntryToActor.Add(EntryId, Actor->GetActorNameOrLabel());
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> EntryArr;
	for (const FResolvedEntry& E : Resolved)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), E.EntryId);
		Obj->SetStringField(TEXT("kind"), E.KindToken);
		Obj->SetStringField(TEXT("places_via"), FString::Printf(TEXT("%s.%s"), *E.Namespace, *E.Action));
		Obj->SetStringField(TEXT("label"), E.Label);

		FString Detail;
		if (E.Params->TryGetStringField(TEXT("class_or_mesh"), Detail) ||
			E.Params->TryGetStringField(TEXT("type"), Detail))
		{
			Obj->SetStringField(TEXT("target"), Detail);
		}
		FString PresetName;
		if (E.Params->TryGetStringField(TEXT("preset"), PresetName))
		{
			Obj->SetStringField(TEXT("preset"), PresetName);
		}
		if (const FString* ActorName = PresentEntryToActor.Find(E.EntryId))
		{
			Obj->SetBoolField(TEXT("in_level"), true);
			Obj->SetStringField(TEXT("actor_name"), *ActorName);
		}
		else
		{
			Obj->SetBoolField(TEXT("in_level"), false);
		}
		if (bIncludeParams)
		{
			Obj->SetObjectField(TEXT("params"), E.Params);
		}
		EntryArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TArray<TSharedPtr<FJsonValue>> ProblemArr;
	for (const FString& P : Problems) { ProblemArr.Add(MakeShared<FJsonValueString>(P)); }

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("layout"), Request.LayoutId);
	Result->SetStringField(TEXT("source"), Request.Source);
	Result->SetBoolField(TEXT("valid"), bValid);
	Result->SetNumberField(TEXT("entry_count"), Resolved.Num());
	Result->SetArrayField(TEXT("entries"), EntryArr);
	Result->SetArrayField(TEXT("problems"), ProblemArr);
	Result->SetArrayField(TEXT("origin"), VectorToJson(Request.Origin));
	Result->SetStringField(TEXT("folder"), !Request.Folder.IsEmpty()
		? Request.Folder
		: FString::Printf(TEXT("Monolith/Layouts/%s"), *Request.LayoutId));
	Result->SetBoolField(TEXT("level_open"), World != nullptr);
	Result->SetNumberField(TEXT("entries_in_level"), PresentEntryToActor.Num());
	Result->SetArrayField(TEXT("orphans"), OrphanArr);

	FString Description;
	Request.Body->TryGetStringField(TEXT("description"), Description);
	Result->SetStringField(TEXT("description"), Description);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 3. apply_level_layout
// ============================================================================

FMonolithActionResult FMonolithMeshLayoutActions::ApplyLevelLayout(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithLayoutInternal;

	const double StartSeconds = FPlatformTime::Seconds();

	FRequest Request;
	FString Error;
	if (!ReadRequest(Params, Request, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FString OnExisting = TEXT("replace");
	Params->TryGetStringField(TEXT("on_existing"), OnExisting);
	OnExisting = OnExisting.TrimStartAndEnd().ToLower();
	if (OnExisting.IsEmpty()) { OnExisting = TEXT("replace"); }
	if (OnExisting != TEXT("replace") && OnExisting != TEXT("skip"))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown on_existing '%s'. Use 'replace' (remove the previous instance of this layout, then "
				 "place the document — the default) or 'skip' (place only entries that are not in the level yet)."),
			*OnExisting));
	}

	bool bDryRun = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	// --- Phase 1: resolve everything. Nothing is touched yet. ---
	TArray<FResolvedEntry> Resolved;
	TArray<FString> Problems;
	if (!ResolveLayout(Request.LayoutId, Request.Body, Request.Origin, Request.Folder, Resolved, Problems))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Layout '%s' has %d unresolved entry problem(s) — NOTHING was placed and the level is "
				 "unchanged. %s. Fix the document and re-apply; mesh.describe_level_layout reports the same "
				 "list without touching the level."),
			*Request.LayoutId, Problems.Num(), *JoinProblems(Problems)));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!bDryRun && !RequireEditorWorld(World, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	const FString Folder = !Request.Folder.IsEmpty()
		? Request.Folder
		: FString::Printf(TEXT("Monolith/Layouts/%s"), *Request.LayoutId);

	// Existing instance of this layout, by tag.
	TArray<AActor*> Existing = FindLayoutActors(World, Request.LayoutId);
	TSet<FString> ExistingEntryIds;
	for (AActor* Actor : Existing)
	{
		const FString EntryId = GetEntryId(Actor);
		if (!EntryId.IsEmpty()) { ExistingEntryIds.Add(EntryId); }
	}

	// Which entries this call will actually place.
	TArray<const FResolvedEntry*> ToPlace;
	TArray<FString> SkippedIds;
	for (const FResolvedEntry& E : Resolved)
	{
		if (OnExisting == TEXT("skip") && ExistingEntryIds.Contains(E.EntryId))
		{
			SkippedIds.Add(E.EntryId);
			continue;
		}
		ToPlace.Add(&E);
	}

	if (bDryRun)
	{
		auto DryResult = MakeShared<FJsonObject>();
		DryResult->SetStringField(TEXT("layout"), Request.LayoutId);
		DryResult->SetStringField(TEXT("source"), Request.Source);
		DryResult->SetBoolField(TEXT("dry_run"), true);
		DryResult->SetStringField(TEXT("on_existing"), OnExisting);
		DryResult->SetStringField(TEXT("folder"), Folder);
		DryResult->SetNumberField(TEXT("entry_count"), Resolved.Num());
		DryResult->SetNumberField(TEXT("would_place"), ToPlace.Num());
		DryResult->SetNumberField(TEXT("would_skip"), SkippedIds.Num());
		DryResult->SetNumberField(TEXT("would_remove_previous"),
			OnExisting == TEXT("replace") ? Existing.Num() : 0);
		DryResult->SetBoolField(TEXT("level_open"), World != nullptr);
		DryResult->SetNumberField(TEXT("elapsed_ms"), (FPlatformTime::Seconds() - StartSeconds) * 1000.0);
		return FMonolithActionResult::Success(DryResult);
	}

	// --- Phase 2: place. One undo transaction for the whole layout. ---
	FScopedBatchFlag BatchFlag(true);
	if (GEditor)
	{
		GEditor->BeginTransaction(FText::FromString(
			FString::Printf(TEXT("Monolith: Apply Level Layout '%s'"), *Request.LayoutId)));
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const FName LayoutTag = MakeLayoutTag(Request.LayoutId);

	TArray<AActor*> Created;
	TArray<const FResolvedEntry*> CreatedFor;
	FString FailureEntryId;
	FString FailureReason;

	FSpawnWatcher Watcher(World);

	for (const FResolvedEntry* Entry : ToPlace)
	{
		Watcher.Spawned.Reset();

		const FMonolithActionResult Sub = Registry.ExecuteAction(Entry->Namespace, Entry->Action, Entry->Params);
		if (!Sub.bSuccess)
		{
			FailureEntryId = Entry->EntryId;
			FailureReason = Sub.ErrorMessage;
			break;
		}

		FString ActorName;
		if (Sub.Result.IsValid())
		{
			Sub.Result->TryGetStringField(TEXT("actor_name"), ActorName);
		}

		// Prefer the actor the world actually reported spawning; fall back to the
		// action's own actor_name only if the delegate saw nothing.
		AActor* Actor = nullptr;
		for (int32 i = Watcher.Spawned.Num() - 1; i >= 0; --i)
		{
			AActor* Candidate = Watcher.Spawned[i];
			if (!IsValid(Candidate)) { continue; }
			if (!ActorName.IsEmpty() && Candidate->GetActorNameOrLabel() == ActorName)
			{
				Actor = Candidate;
				break;
			}
			if (!Actor) { Actor = Candidate; }
		}
		if (!Actor && !ActorName.IsEmpty())
		{
			FString FindError;
			Actor = MonolithMeshUtils::FindActorByName(ActorName, FindError);
		}
		if (!Actor)
		{
			FailureEntryId = Entry->EntryId;
			FailureReason = FString::Printf(
				TEXT("%s.%s reported success but no spawned actor could be identified, so it cannot be tagged "
					 "as part of the layout."),
				*Entry->Namespace, *Entry->Action);
			break;
		}

		Actor->Modify();
		Actor->Tags.AddUnique(LayoutTag);
		Actor->Tags.AddUnique(MakeEntryTag(Entry->EntryId));

		Created.Add(Actor);
		CreatedFor.Add(Entry);
	}

	// --- All-or-nothing: undo everything this call created. ---
	if (!FailureEntryId.IsEmpty())
	{
		int32 Removed = 0;
		for (AActor* Actor : Created)
		{
			if (IsValid(Actor) && World)
			{
				World->EditorDestroyActor(Actor, true);
				++Removed;
			}
		}
		if (GEditor && GEditor->IsTransactionActive())
		{
			GEditor->CancelTransaction(0);
		}

		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Layout '%s' entry '%s' failed to place: %s. The layout was NOT applied — the %d actor(s) this "
				 "call had already created were removed, and the previous instance of this layout (%d actor(s)) "
				 "is untouched. This entry passed validation, so the refusal came from the engine at spawn time."),
			*Request.LayoutId, *FailureEntryId, *FailureReason, Removed, Existing.Num()));
	}

	// --- Success: retire the previous instance, then name the new actors. ---
	int32 RemovedPrevious = 0;
	if (OnExisting == TEXT("replace"))
	{
		const TSet<AActor*> Fresh(Created);
		for (AActor* Actor : Existing)
		{
			if (IsValid(Actor) && !Fresh.Contains(Actor) && World)
			{
				World->EditorDestroyActor(Actor, true);
				++RemovedPrevious;
			}
		}
	}

	// Labels are assigned only after the old instance is gone, so re-applying a
	// layout does not drift labels ("sun" -> "sun2" -> "sun3").
	TArray<TSharedPtr<FJsonValue>> ActorArr;
	for (int32 i = 0; i < Created.Num(); ++i)
	{
		AActor* Actor = Created[i];
		const FResolvedEntry* Entry = CreatedFor[i];
		if (!IsValid(Actor)) { continue; }
		Actor->SetActorLabel(Entry->Label);
		Actor->SetFolderPath(FName(*Folder));

		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Entry->EntryId);
		Obj->SetStringField(TEXT("kind"), Entry->KindToken);
		Obj->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
		Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
		ActorArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	if (GEditor && GEditor->IsTransactionActive())
	{
		GEditor->EndTransaction();
	}

	TArray<TSharedPtr<FJsonValue>> SkippedArr;
	for (const FString& Id : SkippedIds) { SkippedArr.Add(MakeShared<FJsonValueString>(Id)); }

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("layout"), Request.LayoutId);
	Result->SetStringField(TEXT("source"), Request.Source);
	Result->SetStringField(TEXT("on_existing"), OnExisting);
	Result->SetStringField(TEXT("folder"), Folder);
	Result->SetStringField(TEXT("layout_tag"), LayoutTag.ToString());
	Result->SetNumberField(TEXT("entry_count"), Resolved.Num());
	Result->SetNumberField(TEXT("placed"), ActorArr.Num());
	Result->SetNumberField(TEXT("skipped"), SkippedIds.Num());
	Result->SetNumberField(TEXT("removed_previous"), RemovedPrevious);
	Result->SetArrayField(TEXT("skipped_entries"), SkippedArr);
	Result->SetArrayField(TEXT("actors"), ActorArr);
	Result->SetArrayField(TEXT("origin"), VectorToJson(Request.Origin));
	Result->SetNumberField(TEXT("elapsed_ms"), (FPlatformTime::Seconds() - StartSeconds) * 1000.0);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 4. remove_level_layout
// ============================================================================

FMonolithActionResult FMonolithMeshLayoutActions::RemoveLevelLayout(const TSharedPtr<FJsonObject>& Params)
{
	FString LayoutId;
	if (!Params->TryGetStringField(TEXT("layout"), LayoutId) || LayoutId.TrimStartAndEnd().IsEmpty())
	{
		return FMonolithActionResult::Error(
			TEXT("Missing required param: layout (the layout id its actors are tagged with — "
				 "see mesh.list_level_layouts)."));
	}
	LayoutId = LayoutId.TrimStartAndEnd();

	bool bDryRun = false;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	FString Error;
	if (!RequireEditorWorld(World, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	TArray<AActor*> Actors = FindLayoutActors(World, LayoutId);

	TArray<TSharedPtr<FJsonValue>> ActorArr;
	for (AActor* Actor : Actors)
	{
		auto Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("actor_name"), Actor->GetActorNameOrLabel());
		Obj->SetStringField(TEXT("entry"), GetEntryId(Actor));
		Obj->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
		ActorArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	int32 Removed = 0;
	if (!bDryRun && Actors.Num() > 0)
	{
		MonolithLayoutInternal::FScopedBatchFlag BatchFlag(true);
		if (GEditor)
		{
			GEditor->BeginTransaction(FText::FromString(
				FString::Printf(TEXT("Monolith: Remove Level Layout '%s'"), *LayoutId)));
		}
		for (AActor* Actor : Actors)
		{
			if (IsValid(Actor))
			{
				World->EditorDestroyActor(Actor, true);
				++Removed;
			}
		}
		if (GEditor && GEditor->IsTransactionActive())
		{
			GEditor->EndTransaction();
		}
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("layout"), LayoutId);
	Result->SetStringField(TEXT("layout_tag"), MakeLayoutTag(LayoutId).ToString());
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetNumberField(TEXT("found"), Actors.Num());
	Result->SetNumberField(TEXT("removed"), Removed);
	Result->SetArrayField(TEXT("actors"), ActorArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 5. save_level_layout
// ============================================================================

FMonolithActionResult FMonolithMeshLayoutActions::SaveLevelLayout(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithLayoutInternal;

	FString LayoutId;
	if (!Params->TryGetStringField(TEXT("layout"), LayoutId) || LayoutId.TrimStartAndEnd().IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Missing required param: layout (the layout id — also the file name)."));
	}
	LayoutId = LayoutId.TrimStartAndEnd();
	if (!IsValidIdentifier(LayoutId))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Layout id '%s' is not usable as a file name — use only letters, digits, '_', '-' and '.'."),
			*LayoutId));
	}

	const TSharedPtr<FJsonObject>* BodyPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("layout_json"), BodyPtr) || !BodyPtr || !(*BodyPtr).IsValid())
	{
		return FMonolithActionResult::Error(
			TEXT("Missing required param: layout_json (the layout body: {description?, folder?, origin?, entries:[...]})."));
	}
	const TSharedPtr<FJsonObject> Body = *BodyPtr;

	bool bOverwrite = false;
	Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);

	// Never write a document that would not apply.
	TArray<FResolvedEntry> Resolved;
	TArray<FString> Problems;
	if (!ResolveLayout(LayoutId, Body, FVector::ZeroVector, FString(), Resolved, Problems))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Layout '%s' has %d problem(s) and was NOT written: %s."),
			*LayoutId, Problems.Num(), *JoinProblems(Problems)));
	}

	const FString Dir = Library().GetUserDirectory();
	const FString FilePath = Dir / (LayoutId + TEXT(".json"));

	if (!bOverwrite && IFileManager::Get().FileExists(*FilePath))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("A layout file already exists at %s. Pass overwrite=true to replace it."), *FilePath));
	}

	IFileManager::Get().MakeDirectory(*Dir, true);

	auto Layouts = MakeShared<FJsonObject>();
	Layouts->SetObjectField(LayoutId, Body);
	auto Document = MakeShared<FJsonObject>();
	Document->SetObjectField(TEXT("layouts"), Layouts);

	FString Serialized;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Serialized);
	if (!FJsonSerializer::Serialize(Document, Writer))
	{
		return FMonolithActionResult::Error(TEXT("Failed to serialize the layout document."));
	}

	if (!FFileHelper::SaveStringToFile(Serialized, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Failed to write %s — check that the directory is writable."), *FilePath));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("layout"), LayoutId);
	Result->SetStringField(TEXT("file"), FilePath);
	Result->SetNumberField(TEXT("entry_count"), Resolved.Num());
	Result->SetBoolField(TEXT("overwrote"), bOverwrite);
	return FMonolithActionResult::Success(Result);
}
