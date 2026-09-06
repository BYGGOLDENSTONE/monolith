// Copyright Monolith. All Rights Reserved.
//
// Generic AIController that auto-starts an assigned BehaviorTree in OnPossess().

#include "Controllers/MonolithBehaviorTreeAIController.h"
#include "MonolithRuntimeModule.h"

#include "BehaviorTree/BehaviorTree.h"

void AMonolithBehaviorTreeAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	if (BehaviorTreeToRun)
	{
		StartBehaviorTree();
	}
}

bool AMonolithBehaviorTreeAIController::StartBehaviorTree()
{
	if (!BehaviorTreeToRun)
	{
		UE_LOG(LogMonolithRuntime, Verbose,
			TEXT("MonolithBehaviorTreeAIController[%s]: no BehaviorTreeToRun assigned"),
			*GetName());
		return false;
	}

	const bool bStarted = RunBehaviorTree(BehaviorTreeToRun);
	if (!bStarted)
	{
		UE_LOG(LogMonolithRuntime, Warning,
			TEXT("MonolithBehaviorTreeAIController[%s]: RunBehaviorTree failed for %s"),
			*GetName(), *BehaviorTreeToRun->GetName());
	}

	return bStarted;
}
