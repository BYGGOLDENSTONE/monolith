// SPDX-License-Identifier: MIT
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Animation/AnimBlueprint.h"
#include "AnimationGraph.h"
#include "AnimationGraphSchema.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "IMonolithGraphFormatter.h"
#include "Misc/Guid.h"
#include "Misc/ScopeExit.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "UObject/Package.h"

namespace MonolithLayoutAvailabilityTest
{
class FUnsupportedFormatter : public IMonolithGraphFormatter
{
public:
	virtual bool SupportsGraph(UEdGraph*) const override { return false; }
	virtual bool FormatGraph(UEdGraph*, int32&, FString&) override { return false; }
	virtual FMonolithFormatterInfo GetFormatterInfo(UEdGraph*) const override { return {}; }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithLayoutAvailabilityTest,
	"Monolith.Animation.Layout.OptionalFormatterAvailability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithLayoutAvailabilityTest::RunTest(const FString&)
{
	auto& Features = IModularFeatures::Get();
	const FName FeatureName = IMonolithGraphFormatter::GetModularFeatureName();
	const auto OriginalProviders = Features.GetModularFeatureImplementations<IMonolithGraphFormatter>(FeatureName);
	// Exercise provider absence deterministically and restore any real provider.
	// No engine task or latent command runs while the feature list is overridden.
	for (auto* Provider : OriginalProviders) Features.UnregisterModularFeature(FeatureName, Provider);
	ON_SCOPE_EXIT
	{
		for (auto* Provider : OriginalProviders) Features.RegisterModularFeature(FeatureName, Provider);
	};

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString BPName = TEXT("BP_LayoutAvailability_") + Suffix;
	const FString ABPName = TEXT("ABP_LayoutAvailability_") + Suffix;
	UPackage* BPPackage = CreatePackage(*(TEXT("/Game/Tests/Monolith/Blueprint/") + BPName));
	UPackage* ABPPackage = CreatePackage(*(TEXT("/Game/Tests/Monolith/Animation/") + ABPName));
	UBlueprint* BP = NewObject<UBlueprint>(BPPackage, *BPName, RF_Public | RF_Standalone);
	UAnimBlueprint* ABP = NewObject<UAnimBlueprint>(ABPPackage, *ABPName, RF_Public | RF_Standalone);
	ON_SCOPE_EXIT
	{
		for (UObject* Asset : TArray<UObject*>{BP, ABP})
		{
			if (!Asset) continue;
			Asset->GetOutermost()->SetDirtyFlag(false);
			Asset->ClearFlags(RF_Public | RF_Standalone);
			Asset->MarkAsGarbage();
		}
	};
	if (!TestNotNull(TEXT("GUID Blueprint fixture created"), BP) ||
		!TestNotNull(TEXT("GUID Animation Blueprint fixture created"), ABP)) return false;
	UEdGraph* BPGraph = NewObject<UEdGraph>(BP, TEXT("EventGraph"));
	BPGraph->Schema = UEdGraphSchema_K2::StaticClass();
	BP->UbergraphPages.Add(BPGraph);
	UAnimationGraph* AnimGraph = NewObject<UAnimationGraph>(ABP, TEXT("AnimGraph"));
	AnimGraph->Schema = UAnimationGraphSchema::StaticClass();
	ABP->FunctionGraphs.Add(AnimGraph);

	for (int32 Index = 0; Index < 2; ++Index)
	{
		const TCHAR* Namespace = Index == 0 ? TEXT("blueprint") : TEXT("animation");
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Index == 0 ? BP->GetPathName() : ABP->GetPathName());
		Params->SetStringField(TEXT("graph_name"), Index == 0 ? TEXT("EventGraph") : TEXT("AnimGraph"));
		if (Index == 0) Params->SetStringField(TEXT("layout_mode"), TEXT("all"));
		Params->SetStringField(TEXT("formatter"), TEXT("blueprint_assist"));
		const auto Missing = FMonolithToolRegistry::Get().ExecuteAction(Namespace, TEXT("auto_layout"), Params);
		TestFalse(TEXT("Explicit absent formatter fails on valid asset and graph"), Missing.bSuccess);
		TestEqual(TEXT("Explicit absent formatter has optional dependency code"), Missing.ErrorCode, FMonolithJsonUtils::ErrOptionalDepUnavailable);
		if (TestTrue(TEXT("Absent formatter provides structured data"), Missing.ErrorData.IsValid()))
		{
			const auto Data = Missing.ErrorData->AsObject();
			if (TestTrue(TEXT("Absent formatter data is an object"), Data.IsValid()))
			{
				TestEqual(TEXT("Absent formatter names BlueprintAssist"), Data->GetStringField(TEXT("dep_name")), FString(TEXT("BlueprintAssist")));
				TestEqual(TEXT("Absent formatter has correct error class"), Data->GetStringField(TEXT("class")), FString(TEXT("optional_dep_unavailable")));
				TestFalse(TEXT("Absent formatter performs no layout"), Data->GetBoolField(TEXT("executed")));
			}
		}
		Params->SetStringField(TEXT("formatter"), TEXT("auto"));
		const auto Automatic = FMonolithToolRegistry::Get().ExecuteAction(Namespace, TEXT("auto_layout"), Params);
		if (TestTrue(TEXT("Default automatic layout remains available without external provider"), Automatic.bSuccess && Automatic.Result.IsValid()))
			TestEqual(TEXT("Fallback explicitly reports the implementation used"), Automatic.Result->GetStringField(TEXT("formatter_used")),
				FString(Index == 0 ? TEXT("monolith") : TEXT("builtin")));

		// An installed provider that rejects this graph must not be called missing.
		MonolithLayoutAvailabilityTest::FUnsupportedFormatter Unsupported;
		Features.RegisterModularFeature(FeatureName, &Unsupported);
		ON_SCOPE_EXIT { Features.UnregisterModularFeature(FeatureName, &Unsupported); };
		Params->SetStringField(TEXT("formatter"), TEXT("blueprint_assist"));
		const auto Rejected = FMonolithToolRegistry::Get().ExecuteAction(Namespace, TEXT("auto_layout"), Params);
		TestFalse(TEXT("Unsupported graph remains an error"), Rejected.bSuccess);
		TestTrue(TEXT("Unsupported graph is distinct from absent dependency"), Rejected.ErrorCode != FMonolithJsonUtils::ErrOptionalDepUnavailable);
	}
	return true;
}

#endif
