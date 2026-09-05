// MonolithCommonUIActions.h
// Public aggregator; absent CommonUI implementations return a dependency error.
#pragma once


#include "CoreMinimal.h"

class FMonolithToolRegistry;

class MONOLITHUI_API FMonolithCommonUIActions
{
public:
	/** Register every CommonUI action, including unavailable handlers when absent. */
	static void RegisterAll(FMonolithToolRegistry& Registry);
};
