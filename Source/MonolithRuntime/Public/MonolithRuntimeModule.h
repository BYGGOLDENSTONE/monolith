#pragma once
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMonolithRuntime, Log, All);

class FMonolithRuntimeModule : public IModuleInterface
{
public:
    virtual void StartupModule() override;
};
