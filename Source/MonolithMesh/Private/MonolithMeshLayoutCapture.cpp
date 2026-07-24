// Copyright tumourlove. All Rights Reserved.

// =============================================================================
// MonolithMeshLayoutCapture.cpp — mesh.capture_level_layout
//
// The REVERSE direction of the data-driven level layout system: read actors that
// are already in the open level back into a layout document.
//
// The design (sources, the two property filters, the FPostProcessSettings
// exception, exact-match presets, and every place the round trip is lossy) is
// documented in the CAPTURE section of MonolithMeshLayoutActions.h. This file is
// the implementation of exactly that contract.
//
// It is READ-ONLY with respect to the world: no spawn, no property write, no
// transaction, no package dirtying. The only side effect the action can have is
// writing a file, and it does that by calling mesh.save_level_layout — the
// existing writer, which validates before it writes.
// =============================================================================

#include "MonolithMeshLayoutActions.h"

#include "MonolithMeshAtmosphereActions.h"
#include "MonolithMeshLightActions.h"
#include "MonolithMeshSceneActions.h"
#include "MonolithMeshVolumeActions.h"
#include "MonolithMeshUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithToolRegistry.h"

#include "Reflection/MonolithReflectionReader.h"
#include "Reflection/MonolithReflectionWalker.h"

#include "Builders/CubeBuilder.h"
#include "Components/LightComponentBase.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/Brush.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Selection.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace MonolithLayoutCaptureInternal
{
	/** Section tokens of the capture allowlists in Config/MonolithLevelLayouts.json. */
	static const TCHAR* SectionActor()          { return TEXT("actor"); }
	static const TCHAR* SectionActorComponent() { return TEXT("actor_component"); }
	static const TCHAR* SectionAtmoActor()      { return TEXT("atmosphere_actor"); }

	/**
	 * Decimal places kept for TRANSFORM numbers (location / rotation / scale /
	 * extent) written into a document.
	 *
	 * A rotation read back off an actor has been through FRotator -> FQuat ->
	 * FRotator, so an authored [-35, 150, 0] comes back as
	 * [-34.999999999999993, 150.00000000000003, 4.07e-14]. Writing that verbatim
	 * makes a document that is technically exact and practically unreadable and
	 * undiffable, which defeats the reason the format is text in the first place.
	 *
	 * This is the ONE deliberate approximation in the whole capture path, it is
	 * bounded at 5e-7 units / 5e-7 degrees (below anything the engine, a renderer or
	 * a human distinguishes), and it applies ONLY to transform numbers — never to a
	 * captured property value, where exactness is the contract.
	 */
	static constexpr int32 TransformDecimals = 6;

	static double RoundTransform(double V)
	{
		const double Scale = FMath::Pow(10.0, static_cast<double>(TransformDecimals));
		return FMath::RoundToDouble(V * Scale) / Scale;
	}

	static TArray<TSharedPtr<FJsonValue>> VectorToJson(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(RoundTransform(V.X)));
		Arr.Add(MakeShared<FJsonValueNumber>(RoundTransform(V.Y)));
		Arr.Add(MakeShared<FJsonValueNumber>(RoundTransform(V.Z)));
		return Arr;
	}

	static TArray<TSharedPtr<FJsonValue>> RotatorToJson(const FRotator& R)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(RoundTransform(R.Pitch)));
		Arr.Add(MakeShared<FJsonValueNumber>(RoundTransform(R.Yaw)));
		Arr.Add(MakeShared<FJsonValueNumber>(RoundTransform(R.Roll)));
		return Arr;
	}

	static TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FString& S : In) { Arr.Add(MakeShared<FJsonValueString>(S)); }
		return Arr;
	}

	/**
	 * An actor label turned into something ResolveLayout will accept as an entry id
	 * (letters, digits, '_', '-', '.'; at most 96 characters). Runs of anything else
	 * collapse to a single underscore.
	 */
	static FString SanitiseId(const FString& In)
	{
		FString Out;
		for (const TCHAR C : In)
		{
			if (FChar::IsAlnum(C) || C == TEXT('_') || C == TEXT('-') || C == TEXT('.'))
			{
				Out.AppendChar(FChar::ToLower(C));
			}
			else if (Out.Len() > 0 && Out[Out.Len() - 1] != TEXT('_'))
			{
				Out.AppendChar(TEXT('_'));
			}
		}
		while (Out.EndsWith(TEXT("_"))) { Out.LeftChopInline(1); }
		if (Out.Len() > 96) { Out.LeftInline(96); }
		return Out;
	}

	/** Make Id unique inside Used, then remember it. */
	static FString MakeUniqueId(const FString& Id, TSet<FString>& Used)
	{
		FString Base = Id.IsEmpty() ? TEXT("actor") : Id;
		FString Candidate = Base;
		int32 Suffix = 2;
		while (Used.Contains(Candidate))
		{
			Candidate = FString::Printf(TEXT("%s_%d"), *Base, Suffix++);
		}
		Used.Add(Candidate);
		return Candidate;
	}

	/**
	 * Would the write path accept this captured value back?
	 *
	 * Runs the real coercion (FMonolithReflectionWalker::InspectTree) against
	 * scratch buffers — nothing is mutated. A key that fails here is a value the
	 * reader can produce but the writer cannot consume; capturing it anyway would
	 * produce a document that fails to apply, so it is dropped WITH a warning.
	 */
	static bool CanWriteBack(
		UStruct* Struct, const void* Container, const FString& Key,
		const TSharedPtr<FJsonValue>& Value, FString& OutReason)
	{
		OutReason.Reset();
		if (!Struct || !Container || !Value.IsValid())
		{
			OutReason = TEXT("no target");
			return false;
		}

		TSharedPtr<FJsonObject> Tree = MakeShared<FJsonObject>();
		Tree->SetField(Key, Value);

		FBulkFillSpec Spec;
		Spec.TargetNamespace = TEXT("mesh");
		Spec.Tree = Tree;
		Spec.bStrict = true;

		const FDryRunReport Report = FMonolithReflectionWalker::InspectTree(Tree, Struct, Container, Spec);
		if (Report.Errors <= 0)
		{
			return true;
		}
		for (const FBulkFillFieldWrite& W : Report.FieldWrites)
		{
			if (!W.bOk)
			{
				OutReason = W.Reason.IsEmpty() ? TEXT("rejected by the write path") : W.Reason;
				break;
			}
		}
		if (OutReason.IsEmpty()) { OutReason = TEXT("rejected by the write path"); }
		return false;
	}

	/**
	 * Would applying PresetValue to this property land on exactly the live value?
	 *
	 * The comparison runs the preset's JSON through the REAL write path into a
	 * scratch buffer and then asks FProperty::Identical. That is what makes preset
	 * matching exact rather than approximate: comparing JSON numbers would report a
	 * false MISS for every float preset value (0.5357 in JSON is not equal to
	 * (double)0.5357f read back off a float property).
	 */
	static bool PresetValueMatches(
		FProperty* Prop, const void* LiveValuePtr, const TSharedPtr<FJsonValue>& PresetValue, UObject* Owner)
	{
		if (!Prop || !LiveValuePtr || !PresetValue.IsValid())
		{
			return false;
		}

		void* Scratch = FMemory::Malloc(Prop->GetSize(), Prop->GetMinAlignment());
		Prop->InitializeValue(Scratch);

		FBulkFillSpec Spec;
		Spec.TargetNamespace = TEXT("mesh");
		Spec.bStrict = true;
		FDryRunReport Report;

		const FBulkFillFieldWrite Write =
			FMonolithReflectionWalker::WriteLeaf(Prop, Scratch, PresetValue, Owner, Spec, Report, Prop->GetName());

		const bool bMatch = Write.bOk && Prop->Identical(Scratch, LiveValuePtr, PPF_None);

		Prop->DestroyValue(Scratch);
		FMemory::Free(Scratch);
		return bMatch;
	}

	/** Name of the FPostProcessSettings bit that gates a field. */
	static FString OverrideNameFor(const FString& Key)
	{
		return FString::Printf(TEXT("bOverride_%s"), *Key);
	}

	/** True when Struct has a bOverride_<Key> bit and it is currently set on Container. */
	static bool OverrideBitIsSet(UStruct* Struct, const void* Container, const FString& Key)
	{
		FProperty* Bit = FMonolithReflectionWalker::FindPropertyForwarding(Struct, OverrideNameFor(Key));
		const FBoolProperty* BoolBit = CastField<FBoolProperty>(Bit);
		if (!BoolBit)
		{
			return false;
		}
		return BoolBit->GetPropertyValue(BoolBit->ContainerPtrToValuePtr<void>(Container));
	}

	/** One preset offered to the matcher, regardless of which library it came from. */
	struct FPresetCandidate
	{
		FString Name;
		TSharedPtr<FJsonObject> Properties;
	};

	/**
	 * Pick the preset that reproduces the live values exactly, preferring the one
	 * that explains the most keys (ties broken by name so the result is stable).
	 * OutCovered receives the canonical property names the winning preset accounts
	 * for, so the caller can drop them from the raw bag WITHOUT losing anything:
	 * they were proved equal, not assumed equal.
	 */
	static FString ChooseMatchingPreset(
		const TArray<FPresetCandidate>& Candidates,
		UStruct* Struct, const void* Container, UObject* Owner,
		bool bOverrideAware,
		TArray<FString>& OutCovered)
	{
		OutCovered.Reset();

		FString BestName;
		TArray<FString> BestCovered;

		for (const FPresetCandidate& Candidate : Candidates)
		{
			if (!Candidate.Properties.IsValid() || Candidate.Properties->Values.Num() == 0)
			{
				continue;
			}

			bool bAllMatch = true;
			TArray<FString> Covered;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Candidate.Properties->Values)
			{
				FProperty* Prop = FMonolithReflectionWalker::FindPropertyForwarding(Struct, Pair.Key);
				if (!Prop)
				{
					bAllMatch = false;
					break;
				}
				// On an override-aware struct a value that matches but whose override
				// bit is OFF is NOT in effect. Referencing the preset would turn the
				// bit on, which is a behaviour change — so that is not a match.
				if (bOverrideAware && !OverrideBitIsSet(Struct, Container, Prop->GetName()))
				{
					bAllMatch = false;
					break;
				}
				const void* LivePtr = Prop->ContainerPtrToValuePtr<void>(Container);
				if (!PresetValueMatches(Prop, LivePtr, Pair.Value, Owner))
				{
					bAllMatch = false;
					break;
				}
				Covered.Add(Prop->GetName());
			}

			if (!bAllMatch)
			{
				continue;
			}
			const bool bBetter = BestName.IsEmpty()
				|| Covered.Num() > BestCovered.Num()
				|| (Covered.Num() == BestCovered.Num() && Candidate.Name < BestName);
			if (bBetter)
			{
				BestName = Candidate.Name;
				BestCovered = Covered;
			}
		}

		OutCovered = BestCovered;
		return BestName;
	}

	/** Everything one allowlist read needs to know. */
	struct FReadRequest
	{
		UStruct* Struct = nullptr;
		const void* Container = nullptr;
		/** Default values to compare against; nullptr disables the "differs from default" filter. */
		const void* Archetype = nullptr;
		UObject* Owner = nullptr;
		/** true = a missing name means engine drift and is worth a warning. */
		bool bStrictNames = false;
		/** Human word used in warning text ("light", "root component", ...). */
		FString WhatLabel;
	};

	/**
	 * Read allowlisted properties of one object/struct into OutBag, applying the
	 * "only what differs from the archetype" filter and the write-back check.
	 * Insertion order follows the allowlist, so the document is stable and diffable.
	 */
	static void ReadAllowlisted(
		const FReadRequest& Request,
		const TArray<FString>& Keys,
		bool bIncludeDefaults,
		const FString& EntryId,
		const TSharedPtr<FJsonObject>& OutBag,
		TArray<FString>& OutWarnings)
	{
		if (!Request.Struct || !Request.Container)
		{
			return;
		}

		for (const FString& Key : Keys)
		{
			FProperty* Prop = FMonolithReflectionWalker::FindPropertyForwarding(Request.Struct, Key);
			if (!Prop)
			{
				if (Request.bStrictNames)
				{
					OutWarnings.Add(FString::Printf(
						TEXT("'%s': read-back property '%s' does not exist on %s in this engine build, so it was "
							 "not captured. The read-back set is data — fix the name in the preset file."),
						*EntryId, *Key, *Request.Struct->GetName()));
				}
				continue;
			}

			const void* LivePtr = Prop->ContainerPtrToValuePtr<void>(Request.Container);
			if (!bIncludeDefaults && Request.Archetype)
			{
				const void* DefaultPtr = Prop->ContainerPtrToValuePtr<void>(Request.Archetype);
				if (Prop->Identical(LivePtr, DefaultPtr, PPF_None))
				{
					continue;
				}
			}

			const TSharedPtr<FJsonValue> Value =
				FMonolithReflectionReader::PropertyToJsonValue(Prop, LivePtr, Request.Owner);

			FString Reason;
			if (!CanWriteBack(Request.Struct, Request.Container, Prop->GetName(), Value, Reason))
			{
				OutWarnings.Add(FString::Printf(
					TEXT("'%s': %s property '%s' differs from its default but could not be captured — the write "
						 "path would refuse the value back (%s). Set it by hand in the editor after applying."),
					*EntryId, *Request.WhatLabel, *Prop->GetName(), *Reason));
				continue;
			}

			OutBag->SetField(Prop->GetName(), Value);
		}
	}

	/**
	 * Half-extents of a volume brush, recovered from the box builder those spawn
	 * actions leave on the actor. Returns false (with a reason) for any brush this
	 * cannot reproduce — an extent guessed from a bounding box would be a silent
	 * approximation, which is exactly what this system must not do.
	 */
	static bool ReadBoxExtent(AActor* Actor, FVector& OutExtent, FString& OutWhy)
	{
		OutWhy.Reset();
#if WITH_EDITORONLY_DATA
		ABrush* Brush = Cast<ABrush>(Actor);
		if (!Brush)
		{
			OutWhy = TEXT("actor is not a brush");
			return false;
		}
		UCubeBuilder* Cube = Cast<UCubeBuilder>(Brush->BrushBuilder);
		if (!Cube)
		{
			OutWhy = Brush->BrushBuilder
				? FString::Printf(TEXT("its brush was built by %s, not the box builder"),
					*Brush->BrushBuilder->GetClass()->GetName())
				: FString(TEXT("it carries no brush builder (hand-edited or imported geometry)"));
			return false;
		}
		OutExtent = FVector(Cube->X * 0.5, Cube->Y * 0.5, Cube->Z * 0.5);
		return true;
#else
		OutWhy = TEXT("brush builders are editor-only data");
		return false;
#endif
	}

	/** Reverse of FMonolithMeshVolumeActions::ResolveVolumeClass: class -> token. */
	static FString TokenForVolumeActor(const AActor* Actor)
	{
		if (!Actor) { return FString(); }
		UClass* ActorClass = Actor->GetClass();
		for (const FString& Token : FMonolithMeshVolumeActions::GetVolumeTokens())
		{
			FString Unused;
			// Exact class match only: a SUBCLASS of ATriggerVolume would spawn back as
			// the base class, which is not the same actor. Those fall through to the
			// generic `actor` kind, which does preserve the class.
			if (FMonolithMeshVolumeActions::ResolveVolumeClass(Token, Unused) == ActorClass)
			{
				return Token;
			}
		}
		return FString();
	}
}

// ============================================================================
// Data — capture allowlists
// ============================================================================

TArray<FString> FMonolithMeshLayoutActions::LoadCaptureKeys(
	const FString& SectionToken, TArray<FString>& OutWarnings)
{
	return Library().LoadReadbackKeys(SectionToken, OutWarnings);
}

// ============================================================================
// Capture core
// ============================================================================

bool FMonolithMeshLayoutActions::CaptureActors(
	const TArray<AActor*>& Actors,
	const FCaptureOptions& Options,
	FCaptureResult& OutResult,
	FString& OutError)
{
	using namespace MonolithLayoutCaptureInternal;

	OutError.Reset();
	OutResult = FCaptureResult();

	if (Actors.Num() == 0)
	{
		OutError = TEXT("No actors to capture.");
		return false;
	}

	TArray<FString> DataWarnings;
	const TArray<FString> ActorKeys     = LoadCaptureKeys(SectionActor(), DataWarnings);
	const TArray<FString> ComponentKeys = LoadCaptureKeys(SectionActorComponent(), DataWarnings);
	const TArray<FString> AtmoActorKeys = LoadCaptureKeys(SectionAtmoActor(), DataWarnings);
	OutResult.Warnings.Append(DataWarnings);

	TArray<FString> PresetWarnings;
	const TMap<FString, FMonolithMeshLightActions::FPreset> LightPresets =
		FMonolithMeshLightActions::LoadPresets(PresetWarnings);
	const TMap<FString, FMonolithJsonPreset> AtmoPresets =
		FMonolithMeshAtmosphereActions::Presets().LoadPresets(PresetWarnings);

	TSet<FString> UsedIds;
	TArray<TSharedPtr<FJsonObject>> Entries;
	TArray<FString> EntryIds;
	TSet<FString> Folders;

	for (AActor* Actor : Actors)
	{
		if (!IsValid(Actor))
		{
			continue;
		}

		const FString ActorName = Actor->GetActorNameOrLabel();
		const FString ActorClassName = Actor->GetClass()->GetName();

		// --- entry id: the tag it was placed under, else its label ---------
		FString EntryId = GetEntryId(Actor);
		if (EntryId.IsEmpty())
		{
			EntryId = SanitiseId(ActorName);
		}
		EntryId = MakeUniqueId(EntryId, UsedIds);

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("id"), EntryId);

		const FVector Location = Actor->GetActorLocation() - Options.Origin;
		const FRotator Rotation = Actor->GetActorRotation();
		const FVector Scale = Actor->GetActorScale3D();

		// --- classify -------------------------------------------------------
		const FString AtmoToken = FMonolithMeshAtmosphereActions::TokenForActor(Actor);
		FString LightError;
		ULightComponentBase* LightComp =
			AtmoToken.IsEmpty() ? FMonolithMeshLightActions::ResolveLightComponent(Actor, LightError) : nullptr;
		const FString LightToken =
			LightComp ? FMonolithMeshLightActions::TokenForLightComponent(LightComp) : FString();
		const FString VolumeToken =
			(AtmoToken.IsEmpty() && LightToken.IsEmpty()) ? TokenForVolumeActor(Actor) : FString();

		bool bScaleMatters = false;

		if (!AtmoToken.IsEmpty())
		{
			// ---------------- atmosphere ----------------
			FMonolithMeshAtmosphereActions::FTarget Target;
			FString TargetError;
			if (!FMonolithMeshAtmosphereActions::ResolveTarget(Actor, Target, TargetError))
			{
				OutResult.Skipped.Add({ ActorName, ActorClassName, TargetError });
				UsedIds.Remove(EntryId);
				continue;
			}

			Entry->SetStringField(TEXT("kind"), TEXT("atmosphere"));
			Entry->SetStringField(TEXT("type"), Target.Token);

			TSharedPtr<FJsonObject> Bag = MakeShared<FJsonObject>();

			if (Target.bOverrideAware)
			{
				// FPostProcessSettings: the override bits ARE the truth. Walk them
				// rather than a read-back list, so nothing actually in effect is lost
				// and nothing inert is invented.
				for (TFieldIterator<FProperty> It(Target.Struct); It; ++It)
				{
					FProperty* Bit = *It;
					const FString BitName = Bit->GetName();
					if (!BitName.StartsWith(TEXT("bOverride_"), ESearchCase::CaseSensitive))
					{
						continue;
					}
					const FBoolProperty* BoolBit = CastField<FBoolProperty>(Bit);
					if (!BoolBit || !BoolBit->GetPropertyValue(BoolBit->ContainerPtrToValuePtr<void>(Target.Container)))
					{
						continue;
					}
					const FString FieldName = BitName.RightChop(10);
					FProperty* Field = FMonolithReflectionWalker::FindPropertyForwarding(Target.Struct, FieldName);
					if (!Field)
					{
						OutResult.Warnings.Add(FString::Printf(
							TEXT("'%s': override bit '%s' is set but there is no matching field on %s, so it was "
								 "not captured."), *EntryId, *BitName, *Target.Struct->GetName()));
						continue;
					}
					const void* LivePtr = Field->ContainerPtrToValuePtr<void>(Target.Container);
					const TSharedPtr<FJsonValue> Value =
						FMonolithReflectionReader::PropertyToJsonValue(Field, LivePtr, Target.CradleObject);
					FString Reason;
					if (!CanWriteBack(Target.Struct, Target.Container, Field->GetName(), Value, Reason))
					{
						OutResult.Warnings.Add(FString::Printf(
							TEXT("'%s': post-process setting '%s' is overridden in the level but could not be "
								 "captured — the write path would refuse the value back (%s)."),
							*EntryId, *Field->GetName(), *Reason));
						continue;
					}
					Bag->SetField(Field->GetName(), Value);
				}

				// The volume's own knobs (bUnbound, Priority, ...) are a separate bag.
				TSharedPtr<FJsonObject> ActorBag = MakeShared<FJsonObject>();
				FReadRequest ActorRead;
				ActorRead.Struct = Actor->GetClass();
				ActorRead.Container = Actor;
				ActorRead.Archetype = Actor->GetArchetype();
				ActorRead.Owner = Actor;
				ActorRead.WhatLabel = TEXT("post-process volume");
				ReadAllowlisted(ActorRead, AtmoActorKeys, Options.bIncludeDefaults, EntryId, ActorBag, OutResult.Warnings);
				if (ActorBag->Values.Num() > 0)
				{
					Entry->SetObjectField(TEXT("actor_properties"), ActorBag);
				}

				FVector Extent;
				FString Why;
				if (ReadBoxExtent(Actor, Extent, Why))
				{
					Entry->SetArrayField(TEXT("extent"), VectorToJson(Extent));
				}
				else
				{
					OutResult.Warnings.Add(FString::Printf(
						TEXT("'%s': the post-process volume's extent could not be captured because %s. Re-applying "
							 "this entry will use spawn_atmosphere's default box; set the size by hand or add an "
							 "\"extent\" to the entry."), *EntryId, *Why));
				}
			}
			else
			{
				TArray<FString> Warnings;
				const TArray<FString> Keys =
					FMonolithMeshAtmosphereActions::LoadReadbackKeys(Target.Token, Warnings);
				OutResult.Warnings.Append(Warnings);

				FReadRequest Read;
				Read.Struct = Target.Struct;
				Read.Container = Target.Container;
				Read.Archetype = Target.CradleObject ? Target.CradleObject->GetArchetype() : nullptr;
				Read.Owner = Target.CradleObject;
				Read.bStrictNames = true;
				Read.WhatLabel = Target.Token;
				ReadAllowlisted(Read, Keys, Options.bIncludeDefaults, EntryId, Bag, OutResult.Warnings);
			}

			if (Options.bUsePresets)
			{
				TArray<FPresetCandidate> Candidates;
				for (const TPair<FString, FMonolithJsonPreset>& Pair : AtmoPresets)
				{
					if (!FMonolithMeshAtmosphereActions::IsPresetCompatible(Pair.Value.TypeToken, Target.Token))
					{
						continue;
					}
					Candidates.Add({ Pair.Value.Name, Pair.Value.Properties });
				}
				TArray<FString> Covered;
				const FString Winner = ChooseMatchingPreset(
					Candidates, Target.Struct, Target.Container, Target.CradleObject, Target.bOverrideAware, Covered);
				if (!Winner.IsEmpty())
				{
					Entry->SetStringField(TEXT("preset"), Winner);
					OutResult.PresetsUsed.AddUnique(Winner);
					for (const FString& Name : Covered) { Bag->RemoveField(Name); }
				}
			}

			if (Bag->Values.Num() > 0)
			{
				Entry->SetObjectField(TEXT("properties"), Bag);
			}
			bScaleMatters = true;
		}
		else if (!LightToken.IsEmpty())
		{
			// ---------------- light ----------------
			Entry->SetStringField(TEXT("kind"), TEXT("light"));
			Entry->SetStringField(TEXT("type"), LightToken);

			UClass* CompClass = LightComp->GetClass();
			TSharedPtr<FJsonObject> Bag = MakeShared<FJsonObject>();

			FReadRequest Read;
			Read.Struct = CompClass;
			Read.Container = LightComp;
			Read.Archetype = LightComp->GetArchetype();
			Read.Owner = LightComp;
			Read.bStrictNames = true;
			Read.WhatLabel = TEXT("light");
			ReadAllowlisted(
				Read, FMonolithMeshLightActions::LoadReadbackKeys(LightToken),
				Options.bIncludeDefaults, EntryId, Bag, OutResult.Warnings);

			if (Options.bUsePresets)
			{
				TArray<FPresetCandidate> Candidates;
				for (const TPair<FString, FMonolithMeshLightActions::FPreset>& Pair : LightPresets)
				{
					if (!Pair.Value.TypeToken.IsEmpty() &&
						!Pair.Value.TypeToken.Equals(LightToken, ESearchCase::IgnoreCase))
					{
						continue;
					}
					Candidates.Add({ Pair.Value.Name, Pair.Value.Properties });
				}
				TArray<FString> Covered;
				const FString Winner =
					ChooseMatchingPreset(Candidates, CompClass, LightComp, LightComp, false, Covered);
				if (!Winner.IsEmpty())
				{
					Entry->SetStringField(TEXT("preset"), Winner);
					OutResult.PresetsUsed.AddUnique(Winner);
					for (const FString& Name : Covered) { Bag->RemoveField(Name); }
				}
			}

			if (Bag->Values.Num() > 0)
			{
				Entry->SetObjectField(TEXT("properties"), Bag);
			}
			bScaleMatters = true;
		}
		else if (!VolumeToken.IsEmpty())
		{
			// ---------------- volume ----------------
			Entry->SetStringField(TEXT("kind"), TEXT("volume"));
			Entry->SetStringField(TEXT("type"), VolumeToken);

			FVector Extent;
			FString Why;
			if (ReadBoxExtent(Actor, Extent, Why))
			{
				Entry->SetArrayField(TEXT("extent"), VectorToJson(Extent));
			}
			else
			{
				OutResult.Warnings.Add(FString::Printf(
					TEXT("'%s': the volume's extent could not be captured because %s. Re-applying this entry will "
						 "use spawn_volume's default box; add an \"extent\" to the entry to fix the size."),
					*EntryId, *Why));
			}

			// spawn_volume's `properties` is a REFLECTION channel now (its curated
			// snake_case aliases are still accepted on the way in, but they are
			// translated to the UPROPERTY names they mean), so a volume reads back
			// exactly like a light or an actor does: an allowlist that lives in data,
			// plus the differs-from-the-archetype filter. The allowlist section is
			// `readback.volume_<type>` in Config/MonolithLevelLayouts.json — a type
			// with no section simply captures no properties, which is the honest
			// answer for a trigger or a blocking volume.
			TSharedPtr<FJsonObject> VolumeBag = MakeShared<FJsonObject>();
			TArray<FString> VolumeKeyWarnings;
			const TArray<FString> VolumeKeys =
				LoadCaptureKeys(FString::Printf(TEXT("volume_%s"), *VolumeToken), VolumeKeyWarnings);
			OutResult.Warnings.Append(VolumeKeyWarnings);

			FReadRequest VolumeRead;
			VolumeRead.Struct = Actor->GetClass();
			VolumeRead.Container = Actor;
			VolumeRead.Archetype = Actor->GetArchetype();
			VolumeRead.Owner = Actor;
			VolumeRead.WhatLabel = FString::Printf(TEXT("%s volume"), *VolumeToken);
			ReadAllowlisted(
				VolumeRead, VolumeKeys, Options.bIncludeDefaults, EntryId, VolumeBag, OutResult.Warnings);
			if (VolumeBag->Values.Num() > 0)
			{
				Entry->SetObjectField(TEXT("properties"), VolumeBag);
			}
			bScaleMatters = true;
		}
		else
		{
			// ---------------- generic actor ----------------
			UStaticMesh* Mesh = nullptr;
			if (AStaticMeshActor* SMActor = Cast<AStaticMeshActor>(Actor))
			{
				if (UStaticMeshComponent* SMComp = SMActor->GetStaticMeshComponent())
				{
					Mesh = SMComp->GetStaticMesh();
				}
			}

			FString Token;
			if (Mesh)
			{
				if (Mesh->GetPackage() == GetTransientPackage())
				{
					OutResult.Skipped.Add({ ActorName, ActorClassName, FString::Printf(
						TEXT("its static mesh '%s' lives in the transient package, so it has no asset path a "
							 "layout could name. Save the mesh as an asset first."), *Mesh->GetName()) });
					UsedIds.Remove(EntryId);
					continue;
				}
				if (Actor->GetClass() != AStaticMeshActor::StaticClass())
				{
					OutResult.Skipped.Add({ ActorName, ActorClassName, FString::Printf(
						TEXT("a mesh entry always spawns a plain StaticMeshActor, so capturing this %s would "
							 "silently change its class on re-apply."), *ActorClassName) });
					UsedIds.Remove(EntryId);
					continue;
				}
				Token = Mesh->GetPathName();
			}
			else
			{
				UClass* ActorClass = Actor->GetClass();
				const bool bBlueprintClass =
					ActorClass->ClassGeneratedBy != nullptr || !ActorClass->HasAnyClassFlags(CLASS_Native);
				if (bBlueprintClass)
				{
					// A Blueprint is named by its OBJECT PATH, which mesh.spawn_actor
					// resolves without the Blueprint having to be loaded first. (A short
					// name would not: that is why this used to be a refusal.) The only
					// Blueprint left that cannot be expressed is one with no asset path.
					if (ActorClass->GetPackage() == GetTransientPackage())
					{
						OutResult.Skipped.Add({ ActorName, ActorClassName, FString::Printf(
							TEXT("its class '%s' lives in the transient package, so it has no asset path a layout "
								 "could name. Save the Blueprint as an asset first."), *ActorClassName) });
						UsedIds.Remove(EntryId);
						continue;
					}
					Token = ActorClass->GetPathName();
				}
				else
				{
					Token = ActorClass->GetName();
				}
			}

			// Prove the token round-trips to exactly this actor before writing it.
			UClass* ResolvedClass = nullptr;
			UStaticMesh* ResolvedMesh = nullptr;
			FString ResolveError;
			if (!FMonolithMeshSceneActions::ResolveSpawnTarget(Token, ResolvedClass, ResolvedMesh, ResolveError))
			{
				OutResult.Skipped.Add({ ActorName, ActorClassName, FString::Printf(
					TEXT("mesh.spawn_actor would refuse '%s': %s"), *Token, *ResolveError) });
				UsedIds.Remove(EntryId);
				continue;
			}
			if (ResolvedMesh && ResolvedMesh != Mesh)
			{
				OutResult.Skipped.Add({ ActorName, ActorClassName, FString::Printf(
					TEXT("asset path '%s' resolves to a different mesh than the one on this actor."), *Token) });
				UsedIds.Remove(EntryId);
				continue;
			}
			if (ResolvedClass && ResolvedClass != Actor->GetClass())
			{
				OutResult.Skipped.Add({ ActorName, ActorClassName, FString::Printf(
					TEXT("class name '%s' resolves to %s, not %s, so re-applying would spawn the wrong actor."),
					*Token, *ResolvedClass->GetName(), *ActorClassName) });
				UsedIds.Remove(EntryId);
				continue;
			}

			Entry->SetStringField(TEXT("kind"), TEXT("actor"));
			Entry->SetStringField(TEXT("class"), Token);

			TSharedPtr<FJsonObject> ActorBag = MakeShared<FJsonObject>();
			FReadRequest ActorRead;
			ActorRead.Struct = Actor->GetClass();
			ActorRead.Container = Actor;
			ActorRead.Archetype = Actor->GetArchetype();
			ActorRead.Owner = Actor;
			ActorRead.WhatLabel = TEXT("actor");
			ReadAllowlisted(ActorRead, ActorKeys, Options.bIncludeDefaults, EntryId, ActorBag, OutResult.Warnings);
			if (ActorBag->Values.Num() > 0)
			{
				Entry->SetObjectField(TEXT("properties"), ActorBag);
			}

			if (USceneComponent* Root = Actor->GetRootComponent())
			{
				TSharedPtr<FJsonObject> CompBag = MakeShared<FJsonObject>();
				FReadRequest CompRead;
				CompRead.Struct = Root->GetClass();
				CompRead.Container = Root;
				CompRead.Archetype = Root->GetArchetype();
				CompRead.Owner = Root;
				CompRead.WhatLabel = TEXT("root component");
				ReadAllowlisted(CompRead, ComponentKeys, Options.bIncludeDefaults, EntryId, CompBag, OutResult.Warnings);
				if (CompBag->Values.Num() > 0)
				{
					Entry->SetObjectField(TEXT("component_properties"), CompBag);
				}
			}
			else
			{
				OutResult.Warnings.Add(FString::Printf(
					TEXT("'%s': %s has no root component, so its transform is not meaningful and no component "
						 "properties were captured."), *EntryId, *ActorName));
			}

			if (!Scale.Equals(FVector::OneVector))
			{
				Entry->SetArrayField(TEXT("scale"), VectorToJson(Scale));
			}
		}

		// --- transform + label (shared by every kind) -----------------------
		Entry->SetArrayField(TEXT("location"), VectorToJson(Location));
		if (!Rotation.IsNearlyZero())
		{
			Entry->SetArrayField(TEXT("rotation"), RotatorToJson(Rotation));
		}
		if (bScaleMatters)
		{
			// Compare against the ARCHETYPE scale, not 1: a stock ADirectionalLight
			// ships at 2.5 (that is its editor gizmo size), and a re-applied one gets
			// the same 2.5 back, so warning about it would be pure noise. Only a scale
			// the USER changed is actually lost.
			FVector DefaultScale = FVector::OneVector;
			if (const AActor* ArchetypeActor = Cast<AActor>(Actor->GetArchetype()))
			{
				if (const USceneComponent* ArchetypeRoot = ArchetypeActor->GetRootComponent())
				{
					DefaultScale = ArchetypeRoot->GetRelativeScale3D();
				}
			}
			if (!Scale.Equals(DefaultScale))
			{
				OutResult.Warnings.Add(FString::Printf(
					TEXT("'%s': %s is scaled %s (its default is %s), but the action that places this kind has no "
						 "'scale' parameter, so the scale was NOT captured and the re-applied actor will be at its "
						 "default scale."),
					*EntryId, *ActorName, *Scale.ToString(), *DefaultScale.ToString()));
			}
		}

		const FString FolderPath = Actor->GetFolderPath().ToString();
		if (!FolderPath.IsEmpty())
		{
			Folders.Add(FolderPath);
		}

		Entries.Add(Entry);
		EntryIds.Add(EntryId);
	}

	if (Entries.Num() == 0)
	{
		// Say WHY, right here. A bare "nothing captured" would be exactly the silent
		// drop this whole surface exists to avoid.
		TArray<FString> Reasons;
		for (const FCaptureSkip& Skip : OutResult.Skipped)
		{
			Reasons.Add(FString::Printf(TEXT("'%s' (%s): %s"), *Skip.ActorName, *Skip.ActorClass, *Skip.Reason));
		}
		OutError = FString::Printf(
			TEXT("None of the %d actor(s) could be expressed as layout entries, so no document was produced. %s"),
			Actors.Num(),
			Reasons.Num() > 0 ? *FString::Join(Reasons, TEXT("; ")) : TEXT("No actor was valid."));
		return false;
	}

	// --- order: the previous document's order first, then the rest by id ----
	TMap<FString, int32> Preferred;
	for (int32 i = 0; i < Options.PreferredOrder.Num(); ++i)
	{
		Preferred.Add(Options.PreferredOrder[i], i);
	}
	TArray<int32> Order;
	for (int32 i = 0; i < Entries.Num(); ++i) { Order.Add(i); }
	Order.Sort([&Preferred, &EntryIds](int32 A, int32 B)
	{
		const int32* PA = Preferred.Find(EntryIds[A]);
		const int32* PB = Preferred.Find(EntryIds[B]);
		if (PA && PB) { return *PA < *PB; }
		if (PA) { return true; }
		if (PB) { return false; }
		return EntryIds[A] < EntryIds[B];
	});

	TArray<TSharedPtr<FJsonValue>> EntryArr;
	for (const int32 Index : Order)
	{
		EntryArr.Add(MakeShared<FJsonValueObject>(Entries[Index]));
	}

	// --- assemble the body ---------------------------------------------------
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	if (!Options.Description.IsEmpty())
	{
		Body->SetStringField(TEXT("description"), Options.Description);
	}
	const FString Folder = !Options.Folder.IsEmpty()
		? Options.Folder
		: (Folders.Num() == 1 ? *Folders.CreateConstIterator() : FString());
	if (!Folder.IsEmpty())
	{
		Body->SetStringField(TEXT("folder"), Folder);
	}
	if (!Options.Origin.IsNearlyZero())
	{
		Body->SetArrayField(TEXT("origin"), VectorToJson(Options.Origin));
	}

	// Provenance is only recorded when there is something the reader must know:
	// a clean capture stays a clean diff.
	if (OutResult.Warnings.Num() > 0 || OutResult.Skipped.Num() > 0)
	{
		TSharedPtr<FJsonObject> CaptureBlock = MakeShared<FJsonObject>();
		CaptureBlock->SetArrayField(TEXT("warnings"), StringsToJson(OutResult.Warnings));
		TArray<TSharedPtr<FJsonValue>> SkipArr;
		for (const FCaptureSkip& Skip : OutResult.Skipped)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("actor_name"), Skip.ActorName);
			Obj->SetStringField(TEXT("actor_class"), Skip.ActorClass);
			Obj->SetStringField(TEXT("reason"), Skip.Reason);
			SkipArr.Add(MakeShared<FJsonValueObject>(Obj));
		}
		CaptureBlock->SetArrayField(TEXT("not_captured"), SkipArr);
		Body->SetObjectField(TEXT("capture"), CaptureBlock);
	}

	Body->SetArrayField(TEXT("entries"), EntryArr);

	OutResult.Body = Body;
	OutResult.EntryCount = EntryArr.Num();
	return true;
}

// ============================================================================
// mesh.capture_level_layout
// ============================================================================

void FMonolithMeshLayoutActions::RegisterCaptureAction(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("mesh"), TEXT("capture_level_layout"),
		TEXT("Capture actors that are already in the open level BACK into a layout document — the reverse of "
			 "mesh.apply_level_layout. Source is an applied layout's tagged actors (default), an explicit actor "
			 "list, or the current editor selection. Writes only what differs from each object's default, using "
			 "the same data-driven read-back property sets as mesh.get_light_properties / "
			 "mesh.get_atmosphere_properties, and references a light/atmosphere PRESET by name whenever applying "
			 "that preset would reproduce the live values exactly. Blueprint actors are captured by their class "
			 "object path, so they re-apply without having to be loaded first. Anything the layout format cannot "
			 "express (unsaved meshes or Blueprints, scaled lights, hand-built brushes) is reported as a warning, "
			 "never dropped in silence. Pass save=true to write it through mesh.save_level_layout."),
		FMonolithActionHandler::CreateStatic(&FMonolithMeshLayoutActions::CaptureLevelLayout),
		FParamSchemaBuilder()
			.Required(TEXT("layout"), TEXT("string"),
				TEXT("Layout id the captured document gets (and, for source='layout', the tag that is searched)"))
			.Optional(TEXT("source"), TEXT("string"),
				TEXT("'layout' (default: every actor tagged Monolith.Layout:<layout>), 'actors' (the names in "
					 "`actors`), or 'selection' (whatever is selected in the editor right now)"),
				TEXT("layout"))
			.Optional(TEXT("from_layout"), TEXT("string"),
				TEXT("For source='layout': capture the actors of THIS layout id but write the document under `layout`"))
			.Optional(TEXT("actors"), TEXT("array"), TEXT("Actor names or labels, for source='actors'"))
			.Optional(TEXT("origin"), TEXT("array"),
				TEXT("World offset [x, y, z] subtracted from every captured location and stored as the document origin"),
				TEXT("[0,0,0]"))
			.OptionalAssetPath(TEXT("folder"), TEXT("Outliner folder to record (default: the folder the captured actors share)"))
			.Optional(TEXT("description"), TEXT("string"), TEXT("Description stored in the document"))
			.Optional(TEXT("include_defaults"), TEXT("boolean"),
				TEXT("Write every read-back property, including those still at their default value"), TEXT("false"))
			.Optional(TEXT("use_presets"), TEXT("boolean"),
				TEXT("Reference a preset by name when applying it would reproduce the live values exactly"), TEXT("true"))
			.Optional(TEXT("save"), TEXT("boolean"),
				TEXT("Write the document through mesh.save_level_layout (which validates it first)"), TEXT("false"))
			.Optional(TEXT("overwrite"), TEXT("boolean"), TEXT("With save=true: replace an existing user layout file"), TEXT("false"))
			.Build());
}

FMonolithActionResult FMonolithMeshLayoutActions::CaptureLevelLayout(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithLayoutCaptureInternal;

	const double StartSeconds = FPlatformTime::Seconds();

	FString LayoutId;
	if (!Params->TryGetStringField(TEXT("layout"), LayoutId) || LayoutId.TrimStartAndEnd().IsEmpty())
	{
		return FMonolithActionResult::Error(
			TEXT("Missing required param: layout (the id the captured document gets — for source='layout' it is "
				 "also the tag whose actors are read)."));
	}
	LayoutId = LayoutId.TrimStartAndEnd();

	FString Source = TEXT("layout");
	Params->TryGetStringField(TEXT("source"), Source);
	Source = Source.TrimStartAndEnd().ToLower();
	if (Source.IsEmpty()) { Source = TEXT("layout"); }
	if (Source != TEXT("layout") && Source != TEXT("actors") && Source != TEXT("selection"))
	{
		return FMonolithActionResult::Error(FString::Printf(
			TEXT("Unknown source '%s'. Use 'layout' (the actors tagged with a layout id — the default), 'actors' "
				 "(an explicit list in the `actors` param) or 'selection' (the current editor selection)."),
			*Source));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	FString Error;
	if (!RequireEditorWorld(World, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	FCaptureOptions Options;
	Options.LayoutId = LayoutId;
	Params->TryGetStringField(TEXT("folder"), Options.Folder);
	Params->TryGetStringField(TEXT("description"), Options.Description);
	Params->TryGetBoolField(TEXT("include_defaults"), Options.bIncludeDefaults);
	Options.bUsePresets = true;
	Params->TryGetBoolField(TEXT("use_presets"), Options.bUsePresets);

	if (Params->HasField(TEXT("origin")) && !MonolithMeshUtils::ParseVector(Params, TEXT("origin"), Options.Origin))
	{
		return FMonolithActionResult::Error(TEXT("Param `origin` must be [x, y, z] numbers."));
	}

	// --- gather the source actors -------------------------------------------
	TArray<AActor*> Actors;
	FString SourceDetail;

	if (Source == TEXT("layout"))
	{
		FString FromLayout;
		Params->TryGetStringField(TEXT("from_layout"), FromLayout);
		FromLayout = FromLayout.TrimStartAndEnd();
		if (FromLayout.IsEmpty()) { FromLayout = LayoutId; }
		SourceDetail = FromLayout;

		Actors = FindLayoutActors(World, FromLayout);
		if (Actors.Num() == 0)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("No actor in the open level is tagged '%s%s', so there is nothing to capture. Apply the "
					 "layout first (mesh.apply_level_layout), or capture an explicit set with source='actors' / "
					 "source='selection'."),
				LayoutTagPrefix(), *FromLayout));
		}

		// Keep the previous document's entry order so a re-capture diffs cleanly.
		TArray<FString> LoadWarnings;
		const TMap<FString, FMonolithNamedJsonObject> Layouts = LoadLayouts(LoadWarnings);
		if (const FMonolithNamedJsonObject* Existing = Layouts.Find(FromLayout))
		{
			const TArray<TSharedPtr<FJsonValue>>* EntriesArr = nullptr;
			if (Existing->Object.IsValid() && Existing->Object->TryGetArrayField(TEXT("entries"), EntriesArr) && EntriesArr)
			{
				for (const TSharedPtr<FJsonValue>& V : *EntriesArr)
				{
					const TSharedPtr<FJsonObject>* E = nullptr;
					FString Id;
					if (V.IsValid() && V->TryGetObject(E) && E && (*E)->TryGetStringField(TEXT("id"), Id))
					{
						Options.PreferredOrder.Add(Id);
					}
				}
			}
		}
	}
	else if (Source == TEXT("actors"))
	{
		const TArray<TSharedPtr<FJsonValue>>* NameArr = nullptr;
		if (!Params->TryGetArrayField(TEXT("actors"), NameArr) || !NameArr || NameArr->Num() == 0)
		{
			return FMonolithActionResult::Error(
				TEXT("source='actors' needs a non-empty `actors` array of actor names or labels."));
		}
		TArray<FString> Unknown;
		for (const TSharedPtr<FJsonValue>& V : *NameArr)
		{
			const FString Name = V.IsValid() ? V->AsString().TrimStartAndEnd() : FString();
			if (Name.IsEmpty()) { continue; }
			FString FindError;
			AActor* Actor = MonolithMeshUtils::FindActorByName(Name, FindError);
			if (!Actor)
			{
				Unknown.Add(Name);
				continue;
			}
			Actors.AddUnique(Actor);
		}
		if (Unknown.Num() > 0)
		{
			// A typo must not quietly produce a smaller layout than the caller asked for.
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("%d of the named actor(s) are not in the open level, so nothing was captured: %s. Fix the "
					 "names (mesh.get_actor_info / mesh.list_actors) and re-run."),
				Unknown.Num(), *FString::Join(Unknown, TEXT(", "))));
		}
		SourceDetail = FString::Printf(TEXT("%d named actor(s)"), Actors.Num());
	}
	else
	{
		if (!GEditor)
		{
			return FMonolithActionResult::Error(
				TEXT("source='selection' needs the editor: there is no GEditor in this process."));
		}
		USelection* Selection = GEditor->GetSelectedActors();
		if (Selection)
		{
			for (FSelectionIterator It(*Selection); It; ++It)
			{
				if (AActor* Actor = Cast<AActor>(*It))
				{
					Actors.AddUnique(Actor);
				}
			}
		}
		if (Actors.Num() == 0)
		{
			return FMonolithActionResult::Error(
				TEXT("Nothing is selected in the editor, so there is nothing to capture. Select the actors in the "
					 "viewport or the outliner (mesh.select_actors can do it too) and re-run."));
		}
		SourceDetail = FString::Printf(TEXT("%d selected actor(s)"), Actors.Num());
	}

	// --- capture --------------------------------------------------------------
	FCaptureResult Capture;
	if (!CaptureActors(Actors, Options, Capture, Error))
	{
		return FMonolithActionResult::Error(Error);
	}

	// --- self-check: would this document apply? --------------------------------
	TArray<FResolvedEntry> Resolved;
	TArray<FString> Problems;
	const bool bValid =
		ResolveLayout(LayoutId, Capture.Body, FVector::ZeroVector, Options.Folder, Resolved, Problems);

	// --- optional save, through the existing validating writer -----------------
	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);
	FString SavedFile;
	if (bSave)
	{
		bool bOverwrite = false;
		Params->TryGetBoolField(TEXT("overwrite"), bOverwrite);

		TSharedPtr<FJsonObject> SaveParams = MakeShared<FJsonObject>();
		SaveParams->SetStringField(TEXT("layout"), LayoutId);
		SaveParams->SetObjectField(TEXT("layout_json"), Capture.Body);
		SaveParams->SetBoolField(TEXT("overwrite"), bOverwrite);

		const FMonolithActionResult SaveResult =
			FMonolithToolRegistry::Get().ExecuteAction(TEXT("mesh"), TEXT("save_level_layout"), SaveParams);
		if (!SaveResult.bSuccess)
		{
			return FMonolithActionResult::Error(FString::Printf(
				TEXT("Captured %d entry/entries from %d actor(s), but writing the document failed: %s. Re-run "
					 "with save=false to get the document back in the response instead."),
				Capture.EntryCount, Actors.Num(), *SaveResult.ErrorMessage));
		}
		if (SaveResult.Result.IsValid())
		{
			SaveResult.Result->TryGetStringField(TEXT("file"), SavedFile);
		}
	}

	// --- response --------------------------------------------------------------
	TArray<TSharedPtr<FJsonValue>> SkipArr;
	for (const FCaptureSkip& Skip : Capture.Skipped)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("actor_name"), Skip.ActorName);
		Obj->SetStringField(TEXT("actor_class"), Skip.ActorClass);
		Obj->SetStringField(TEXT("reason"), Skip.Reason);
		SkipArr.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("layout"), LayoutId);
	Result->SetStringField(TEXT("source"), Source);
	Result->SetStringField(TEXT("source_detail"), SourceDetail);
	Result->SetNumberField(TEXT("actors_read"), Actors.Num());
	Result->SetNumberField(TEXT("entry_count"), Capture.EntryCount);
	Result->SetNumberField(TEXT("skipped_count"), Capture.Skipped.Num());
	Result->SetArrayField(TEXT("skipped"), SkipArr);
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Capture.Warnings));
	Result->SetArrayField(TEXT("presets_used"), StringsToJson(Capture.PresetsUsed));
	Result->SetBoolField(TEXT("include_defaults"), Options.bIncludeDefaults);
	Result->SetBoolField(TEXT("use_presets"), Options.bUsePresets);
	Result->SetBoolField(TEXT("valid"), bValid);
	Result->SetArrayField(TEXT("problems"), StringsToJson(Problems));
	Result->SetObjectField(TEXT("layout_json"), Capture.Body);
	Result->SetBoolField(TEXT("saved"), bSave);
	if (!SavedFile.IsEmpty())
	{
		Result->SetStringField(TEXT("file"), SavedFile);
	}
	Result->SetNumberField(TEXT("elapsed_ms"), (FPlatformTime::Seconds() - StartSeconds) * 1000.0);

	return FMonolithActionResult::Success(Result);
}
