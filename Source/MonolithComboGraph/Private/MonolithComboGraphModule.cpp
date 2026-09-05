#include "MonolithComboGraphModule.h"
#include "MonolithToolRegistry.h"
#include "MonolithSettings.h"
#include "MonolithComboGraphActions.h"
#include "MonolithComboGraphBulkFillAdapter.h"

DEFINE_LOG_CATEGORY(LogMonolithComboGraph);

void FMonolithComboGraphModule::StartupModule()
{
	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableComboGraph)
	{
		UE_LOG(LogMonolithComboGraph, Log,
			TEXT("MonolithComboGraph: ComboGraph integration disabled in settings"));
		return;
	}

	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	Registry.SetOptionalDependencyAvailability(TEXT("combograph"), TEXT("ComboGraph"),
		WITH_COMBOGRAPH != 0, WITH_COMBOGRAPH ? TEXT("") : TEXT("not_compiled"));
	FMonolithComboGraphActions::RegisterActions(Registry);
#if WITH_COMBOGRAPH
	int32 ActionCount = FMonolithToolRegistry::Get().GetActions(TEXT("combograph")).Num();
	UE_LOG(LogMonolithComboGraph, Log,
		TEXT("MonolithComboGraph: Loaded (%d actions)"), ActionCount);
#else
	UE_LOG(LogMonolithComboGraph, Log,
		TEXT("MonolithComboGraph: ComboGraph not compiled; actions retained with dependency errors"));
#endif

	// Phase 5 Step 8 (MCP Ergonomics, 2026-05-11) — register the combograph adapter
	// UNCONDITIONALLY per H5 stub-adapter invariant. Body switches on WITH_COMBOGRAPH:
	// dev build wires real handlers, release/no-ComboGraph build returns a clean
	// error. **TargetType writes return EXPLICIT unsupported-field error** pointing
	// at the v1.1 custom-serialisation hook — NOT a silent no-op (Step 8 post-review
	// lock).
	FMonolithComboGraphBulkFillAdapter::Register();
}

void FMonolithComboGraphModule::ShutdownModule()
{
	FMonolithComboGraphBulkFillAdapter::Unregister();
	FMonolithToolRegistry::Get().UnregisterNamespace(TEXT("combograph"));
}

IMPLEMENT_MODULE(FMonolithComboGraphModule, MonolithComboGraph)
