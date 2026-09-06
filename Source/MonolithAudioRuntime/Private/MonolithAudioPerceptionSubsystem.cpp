#include "MonolithAudioPerceptionSubsystem.h"
#include "MonolithAudioRuntimeModule.h"
#include "MonolithSoundPerceptionUserData.h"

#include "Components/AudioComponent.h"
#include "Sound/SoundBase.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "Engine/EngineTypes.h"
#include "EngineUtils.h"

#include "Perception/AISense_Hearing.h"

// ============================================================================
// UWorldSubsystem lifecycle
// ============================================================================

bool UMonolithAudioPerceptionSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	// Game, PIE, GamePreview, GameRPC. Skip Editor (no actors play sounds in the editor view),
	// EditorPreview (thumbnails), Inactive.
	return WorldType == EWorldType::Game
	    || WorldType == EWorldType::PIE
	    || WorldType == EWorldType::GamePreview
	    || WorldType == EWorldType::GameRPC;
}

void UMonolithAudioPerceptionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	if (UWorld* World = GetWorld())
	{
		ActorSpawnedHandle = World->AddOnActorSpawnedHandler(
			FOnActorSpawned::FDelegate::CreateUObject(this, &UMonolithAudioPerceptionSubsystem::OnActorSpawned));

		UE_LOG(LogMonolithAudioRuntime, Log,
			TEXT("MonolithAudioPerceptionSubsystem: Initialized for world '%s' (actor-spawn hook armed)"),
			*World->GetName());
	}
}

void UMonolithAudioPerceptionSubsystem::PostInitialize()
{
	Super::PostInitialize();

	// Walk existing actors once — covers level-placed components present before this subsystem armed.
	if (!bHasWalkedExistingActors)
	{
		HookAllExistingActors();
		bHasWalkedExistingActors = true;
	}
}

void UMonolithAudioPerceptionSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	// PostInitialize can run before level actors have registered components.
	HookAllExistingActors();
}

void UMonolithAudioPerceptionSubsystem::RegisterAudioComponent(UAudioComponent* Component)
{
	if (Component && Component->GetWorld() == GetWorld()) TryHookAudioComponent(Component);
}

void UMonolithAudioPerceptionSubsystem::Deinitialize()
{
	if (UWorld* World = GetWorld(); World && ActorSpawnedHandle.IsValid())
	{
		World->RemoveOnActorSpawnedHandler(ActorSpawnedHandle);
	}
	ActorSpawnedHandle.Reset();

	// Clean up native delegate bindings on every tracked component.
	for (const TPair<TWeakObjectPtr<const UAudioComponent>, FDelegateHandle>& Pair : BoundComponents)
	{
		if (const UAudioComponent* Comp = Pair.Key.Get())
		{
			// const_cast: OnAudioPlayStateChangedNative is a non-const native delegate;
			// we held weak-const for tracking only.
			UAudioComponent* MutableComp = const_cast<UAudioComponent*>(Comp);
			MutableComp->OnAudioPlayStateChangedNative.Remove(Pair.Value);
		}
	}
	BoundComponents.Reset();
	AlreadyFiredThisPlay.Reset();

	Super::Deinitialize();
}

// ============================================================================
// Discovery — hooking AudioComponents
// ============================================================================

void UMonolithAudioPerceptionSubsystem::HookAllExistingActors()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	int32 ActorsScanned = 0;
	int32 ComponentsHooked = 0;

	for (FActorIterator It(World); It; ++It)
	{
		if (AActor* Actor = *It)
		{
			++ActorsScanned;
			const int32 BeforeCount = BoundComponents.Num();
			HookActorAudioComponents(Actor);
			ComponentsHooked += BoundComponents.Num() - BeforeCount;
		}
	}

	if (ComponentsHooked > 0)
	{
		UE_LOG(LogMonolithAudioRuntime, Log,
			TEXT("MonolithAudioPerceptionSubsystem: Initial walk hooked %d AudioComponents across %d actors"),
			ComponentsHooked, ActorsScanned);
	}
}

void UMonolithAudioPerceptionSubsystem::OnActorSpawned(AActor* Actor)
{
	if (Actor)
	{
		HookActorAudioComponents(Actor);
	}
}

void UMonolithAudioPerceptionSubsystem::HookActorAudioComponents(AActor* Actor)
{
	if (!Actor)
	{
		return;
	}

	TArray<UAudioComponent*> AudioComps;
	Actor->GetComponents<UAudioComponent>(AudioComps);
	for (UAudioComponent* Comp : AudioComps)
	{
		TryHookAudioComponent(Comp);
	}
}

void UMonolithAudioPerceptionSubsystem::TryHookAudioComponent(UAudioComponent* AudioComp)
{
	if (!AudioComp)
	{
		return;
	}

	// Auto-destroy components do not survive until world teardown; discard their weak
	// bookkeeping while registering new components so long-running levels stay bounded.
	for (auto It = BoundComponents.CreateIterator(); It; ++It)
		if (!It.Key().IsValid()) It.RemoveCurrent();
	for (auto It = AlreadyFiredThisPlay.CreateIterator(); It; ++It)
		if (!It->IsValid()) It.RemoveCurrent();

	// Avoid double-hook (level-placed actors can be re-hit by OnActorSpawned during seamless travel).
	const TWeakObjectPtr<const UAudioComponent> Key(AudioComp);
	if (BoundComponents.Contains(Key))
	{
		return;
	}

	// We bind unconditionally — the binding might be added LATER via the bind action.
	// The handler itself short-circuits when no UserData is present (cheap GetAssetUserDataOfClass).
	FDelegateHandle Handle = AudioComp->OnAudioPlayStateChangedNative.AddUObject(
		this, &UMonolithAudioPerceptionSubsystem::OnAudioPlayStateChanged);

	BoundComponents.Add(Key, Handle);
	// SpawnSound helpers return already-playing components. Catch up once when callers
	// register those results, or when discovery runs after an auto-activated placed sound.
	// The normal per-play guard prevents duplicate reports on subsequent transitions.
	const EAudioComponentPlayState State = AudioComp->GetPlayState();
	if (State == EAudioComponentPlayState::Playing || State == EAudioComponentPlayState::FadingIn)
		OnAudioPlayStateChanged(AudioComp, State);
}

// ============================================================================
// Dispatch hearing on Playing / FadingIn
// ============================================================================

void UMonolithAudioPerceptionSubsystem::OnAudioPlayStateChanged(
	const UAudioComponent* AudioComp,
	EAudioComponentPlayState NewState)
{
	// Hearing events must be delivered on the game thread.
	// Header doesn't enforce game-thread; assert here for safety.
	if (!ensureMsgf(IsInGameThread(),
		TEXT("MonolithAudioPerceptionSubsystem::OnAudioPlayStateChanged invoked off the game thread — refusing to report hearing")))
	{
		return;
	}

	if (!AudioComp)
	{
		return;
	}

	// Track per-play reentrancy: clear flag on Stopped so the next Play() can fire again.
	if (NewState == EAudioComponentPlayState::Stopped
	 || NewState == EAudioComponentPlayState::FadingOut)
	{
		AlreadyFiredThisPlay.Remove(AudioComp);
		return;
	}

	const bool bIsFireState =
		(NewState == EAudioComponentPlayState::Playing) ||
		(NewState == EAudioComponentPlayState::FadingIn);
	if (!bIsFireState)
	{
		return;
	}

	USoundBase* Sound = const_cast<UAudioComponent*>(AudioComp)->GetSound();
	if (!Sound)
	{
		return;
	}

	UMonolithSoundPerceptionUserData* Data = Cast<UMonolithSoundPerceptionUserData>(
		Sound->GetAssetUserDataOfClass(UMonolithSoundPerceptionUserData::StaticClass()));
	if (!Data || !Data->bEnabled)
	{
		return;
	}

	if (NewState == EAudioComponentPlayState::FadingIn && !Data->bFireOnFadeIn)
	{
		return;
	}

	// Reentrancy guard: avoid double-fire on FadingIn → Playing transition for the same Play() call.
	if (AlreadyFiredThisPlay.Contains(AudioComp))
	{
		return;
	}
	AlreadyFiredThisPlay.Add(AudioComp);

	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client)
	{
		return;
	}

	AActor* Owner = AudioComp->GetOwner();
	const FVector NoiseLocation = AudioComp->GetComponentLocation();

	if (Owner)
	{
		// Authority gating per UE 5.7 BlueprintAuthorityOnly contract on AActor::MakeNoise.
		if (!Owner->HasAuthority())
		{
			return;
		}

		// Report directly: AActor::MakeNoise silently drops non-pawn owners
		// with no pawn instigator. Hearing supports any actor as the source.
		UAISense_Hearing::ReportNoiseEvent(World, NoiseLocation, Data->Loudness,
			Owner, Data->MaxRange, Data->Tag);
	}
	else
	{
		// 2D / no-owner sound. Honor the design-time switch.
		if (Data->bRequireOwningActor)
		{
			return;
		}

		UAISense_Hearing::ReportNoiseEvent(
			World,
			NoiseLocation,
			Data->Loudness,
			/*Instigator=*/nullptr,
			Data->MaxRange,
			Data->Tag);
	}
}
