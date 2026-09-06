// Runtime BT-to-GAS task. GameplayAbilities is a required Monolith dependency.
#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "GameplayTagContainer.h"
#include "Templates/SubclassOf.h"

#include "GameplayAbilitySpec.h"   // FGameplayAbilitySpecHandle (used by-value in BT memory struct)

#include "BTTask_TryActivateAbility.generated.h"   // MUST be last include — UE clobbers CURRENT_FILE_ID via subsequent .generated.h transitives

class UBehaviorTreeComponent;
class UAbilitySystemComponent;

class UGameplayAbility;
struct FAbilityEndedData;

/**
 * Per-instance BT memory for UBTTask_TryActivateAbility.
 *
 * Holds:
 *  - WeakPtr to the resolved ASC (pawn may despawn mid-ability)
 *  - The spec handle that came back from TryActivateAbility (used to filter
 *    OnAbilityEndedWithData callbacks to *our* ability instance)
 *  - The delegate handle so we can detach cleanly on finish or abort
 *
 * GetInstanceMemorySize() returns sizeof(FBTTaskTryActivateAbilityMemory) so
 * the BT memory allocator gives every parallel AI instance its own copy.
 *
 * NOTE: This is a plain C++ struct (no USTRUCT), so the WITH_GAMEPLAYABILITIES
 * gate around it does NOT trigger UHT's "must not be inside preprocessor
 * blocks" rule — that rule applies only to reflection markers. Gating here is
 * safe and keeps the FGameplayAbilitySpecHandle dependency contained.
 */
struct FBTTaskTryActivateAbilityMemory
{
	TWeakObjectPtr<UAbilitySystemComponent> ASC;
	FGameplayAbilitySpecHandle ActivatedSpec;
	FDelegateHandle EndedHandle;
	bool bAwaitingEnd = false;
	bool bWasCancelled = false;
	bool bInsideExecute = false;
	bool bEndedDuringExecute = false;
};


/**
 * BT task that activates a Gameplay Ability on the AI pawn's ASC.
 *
 * The reflection surface is unconditional (UHT requirement). The runtime path
 * is gated in the .cpp via WITH_GAMEPLAYABILITIES — when GAS is unavailable,
 * ExecuteTask returns Failed immediately and the action handler refuses to
 * register a node of this class.
 */
UCLASS(meta = (DisplayName = "Try Activate Gameplay Ability"))
class MONOLITHRUNTIME_API UBTTask_TryActivateAbility : public UBTTaskNode
{
	GENERATED_UCLASS_BODY()

public:

	// --- Configuration (set at design-time by add_bt_use_ability_task) ---

	/**
	 * Class of ability to activate. Mutually exclusive with AbilityTags.
	 *
	 * Type-erased to TSubclassOf<UObject> so this header compiles without the
	 * GameplayAbilities module. The MetaClass hint constrains the editor
	 * picker to UGameplayAbility subclasses when GAS is present; the .cpp
	 * casts via UGameplayAbility::StaticClass()->IsChildOf-checked logic.
	 */
	UPROPERTY(EditAnywhere, Category = "Ability", meta = (MetaClass = "/Script/GameplayAbilities.GameplayAbility", AllowAbstract = "false"))
	TSubclassOf<UObject> AbilityClass;

	/** Tag query — activate ANY granted ability matching ALL of these tags. Mutually exclusive with AbilityClass. */
	UPROPERTY(EditAnywhere, Category = "Ability")
	FGameplayTagContainer AbilityTags;

	/** If true, the task holds (returns InProgress) and finishes only when the ability ends. */
	UPROPERTY(EditAnywhere, Category = "Behavior")
	bool bWaitForEnd = true;

	/** If true, return Succeeded even when activation is blocked (cooldown, missing ASC, etc.). */
	UPROPERTY(EditAnywhere, Category = "Behavior")
	bool bSucceedOnBlocked = false;

	/** Optional: send a gameplay event with this tag immediately after successful activation. */
	UPROPERTY(EditAnywhere, Category = "Behavior")
	FGameplayTag EventTagOnActivate;

	// --- BTTaskNode overrides ---

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void OnTaskFinished(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, EBTNodeResult::Type TaskResult) override;
	virtual uint16 GetInstanceMemorySize() const override;
	virtual FString GetStaticDescription() const override;

protected:
	/** Bound to ASC->OnAbilityEndedWithData. Filters by spec handle then resolves Succeeded/Failed. */
	void HandleAbilityEnded(const FAbilityEndedData& EndedData,
		TWeakObjectPtr<UBehaviorTreeComponent> OwnerCompWeak,
		uint8* NodeMemoryPtr);

	/** Resolve the ASC from the AIController's possessed pawn (interface or component). */
	UAbilitySystemComponent* ResolveASC(UBehaviorTreeComponent& OwnerComp) const;

	/** Detach our delegate from ASC->OnAbilityEndedWithData (idempotent). */
	void UnbindEnded(FBTTaskTryActivateAbilityMemory& Mem) const;

};
