#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "Containers/Set.h"
#include "Delegates/IDelegateInstance.h"
#include "MonolithAudioPerceptionSubsystem.generated.h"

class AActor;
class UAudioComponent;
enum class EAudioComponentPlayState : uint8;

/**
 * Reports AI hearing events when registered audio components play a sound carrying
 * UMonolithSoundPerceptionUserData. Notifications are handled on the game thread.
 * Placed components and components present at actor spawn are registered automatically.
 * Components created later (including gameplay-spawned sounds) must be registered
 * before Play via RegisterAudioComponent. Fire-and-forget sounds can use
 * UMonolithAudioPerceptionStatics::PlaySoundAndReportNoise instead.
 * Reports are authority-only and support non-pawn sound owners.
 */
UCLASS(MinimalAPI)
class UMonolithAudioPerceptionSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	//~ Begin UWorldSubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void PostInitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	/** Register dynamic components; catches existing playback. Prefer registering before Play. Idempotent. */
	UFUNCTION(BlueprintCallable, Category="Monolith|Audio|Perception")
	MONOLITHAUDIORUNTIME_API void RegisterAudioComponent(UAudioComponent* Component);
	//~ End UWorldSubsystem

private:
	/** Bound to UWorld::OnActorSpawned — newly spawned actors get their AudioComponents wired. */
	void OnActorSpawned(AActor* Actor);

	/** Walks an actor's components and binds OnAudioPlayStateChangedNative on each UAudioComponent. */
	void HookActorAudioComponents(AActor* Actor);

	/** Inspects a single component and binds the native delegate if it carries a perception-bound sound. */
	void TryHookAudioComponent(UAudioComponent* AudioComp);

	/** Native multicast handler: dispatches hearing on Playing (and optionally FadingIn). */
	void OnAudioPlayStateChanged(const UAudioComponent* AudioComp, EAudioComponentPlayState NewState);

	/** Walks all existing actors once on PostInitialize / OnWorldBeginPlay (catches placed AudioComponents). */
	void HookAllExistingActors();

	/** Reentrancy guard — components that have already fired this play. Cleared on Stopped. */
	TSet<TWeakObjectPtr<const UAudioComponent>> AlreadyFiredThisPlay;

	/** Tracked DelegateHandles per AudioComponent so Deinitialize can cleanly unregister. */
	TMap<TWeakObjectPtr<const UAudioComponent>, FDelegateHandle> BoundComponents;

	/** OnActorSpawned subscription. */
	FDelegateHandle ActorSpawnedHandle;

	/** Latched after first PostInitialize run so we don't re-walk if Initialize is invoked twice. */
	bool bHasWalkedExistingActors = false;
};
