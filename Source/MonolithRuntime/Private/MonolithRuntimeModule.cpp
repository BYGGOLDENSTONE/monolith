#include "MonolithRuntimeModule.h"
#include "UObject/CoreRedirects.h"

DEFINE_LOG_CATEGORY(LogMonolithRuntime);
IMPLEMENT_MODULE(FMonolithRuntimeModule, MonolithRuntime)

void FMonolithRuntimeModule::StartupModule()
{
    // Preserve serialized assets authored before the runtime/editor split.
    TArray<FCoreRedirect> Redirects;
    Redirects.Emplace(ECoreRedirectFlags::Type_Class,
        TEXT("/Script/MonolithAI.MonolithBehaviorTreeAIController"),
        TEXT("/Script/MonolithRuntime.MonolithBehaviorTreeAIController"));
    Redirects.Emplace(ECoreRedirectFlags::Type_Class,
        TEXT("/Script/MonolithAI.BTTask_TryActivateAbility"),
        TEXT("/Script/MonolithRuntime.BTTask_TryActivateAbility"));
    for (const TCHAR* Name : { TEXT("BTTask_SetMaxWalkSpeed"), TEXT("BTTask_SetCrouch"), TEXT("BTTask_RandomizeFloat") })
    {
        Redirects.Emplace(ECoreRedirectFlags::Type_Class,
            *FString::Printf(TEXT("/Script/MonolithAI.%s"), Name),
            *FString::Printf(TEXT("/Script/MonolithRuntime.%s"), Name));
    }
    Redirects.Emplace(ECoreRedirectFlags::Type_Class,
        TEXT("/Script/MonolithGAS.MonolithGASAttributeBindingClassExtension"),
        TEXT("/Script/MonolithRuntime.MonolithGASAttributeBindingClassExtension"));
    Redirects.Emplace(ECoreRedirectFlags::Type_Struct,
        TEXT("/Script/MonolithGAS.MonolithGASAttributeBindingSpec"),
        TEXT("/Script/MonolithRuntime.MonolithGASAttributeBindingSpec"));
    for (const TCHAR* Name : { TEXT("EMonolithAttrBindOwner"), TEXT("EMonolithAttrBindFormat"), TEXT("EMonolithAttrBindUpdate") })
    {
        Redirects.Emplace(ECoreRedirectFlags::Type_Enum,
            *FString::Printf(TEXT("/Script/MonolithGAS.%s"), Name),
            *FString::Printf(TEXT("/Script/MonolithRuntime.%s"), Name));
    }
    FCoreRedirects::AddRedirectList(Redirects, TEXT("MonolithRuntime"));
}
