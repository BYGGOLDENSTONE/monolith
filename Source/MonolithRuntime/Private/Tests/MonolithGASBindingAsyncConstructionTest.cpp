#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Async/Async.h"
#include "MonolithGASAttributeBindingClassExtension.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithGASBindingAsyncConstructionTest,
    "Monolith.GAS.Runtime.AsyncExtensionConstruction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FMonolithGASBindingAsyncConstructionTest::RunTest(const FString& Parameters)
{
    // Resolve the class/CDO on the game thread, then exercise the same UObject
    // constructor from a worker as cooked async package loading. The former direct
    // FTickableGameObject base emitted an ensure here before any widget existed.
    UClass* ExtensionClass = UMonolithGASAttributeBindingClassExtension::StaticClass();
    ExtensionClass->GetDefaultObject();
    UPackage* Outer = GetTransientPackage();
    UMonolithGASAttributeBindingClassExtension* Extension = Async(EAsyncExecution::Thread,
        [Outer, ExtensionClass]()
        {
            FGCScopeGuard Guard;
            check(!IsInGameThread());
            return NewObject<UMonolithGASAttributeBindingClassExtension>(Outer, ExtensionClass);
        }).Get();

    if (!TestNotNull(TEXT("Extension construction succeeds off the game thread without tick registration"), Extension))
        return false;

    // Worker-created UObjects acquire Async protection; release it on the game
    // thread once the task finishes so this GUID-free transient fixture can be GC'd.
    Extension->ClearInternalFlags(EInternalObjectFlags::Async);
    TestFalse(TEXT("No instance is tickable during async loading"), Extension->IsTickable());
    Extension->ConditionalBeginDestroy();
    TestTrue(TEXT("An extension never constructed by a widget can be destroyed safely"),
        Extension->HasAnyFlags(RF_BeginDestroyed));
    return true;
}
#endif
