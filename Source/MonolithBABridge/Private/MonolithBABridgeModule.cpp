#include "Modules/ModuleManager.h"
#include "IMonolithGraphFormatter.h"
#include "MonolithBAFormatterImpl.h"
#include "MonolithSettings.h"
#include "MonolithToolRegistry.h"

DEFINE_LOG_CATEGORY(LogMonolithBABridge);

class FMonolithBABridgeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
		if (!Settings || !Settings->bEnableBlueprintAssist)
		{
			ReportAvailability(false, TEXT("Blueprint Assist integration is disabled in Monolith settings."));
			UE_LOG(LogMonolithBABridge, Log,
				TEXT("MonolithBABridge: Blueprint Assist integration disabled in settings"));
			return;
		}

#if WITH_BLUEPRINT_ASSIST
		Formatter = MakeUnique<FMonolithBAFormatterImpl>();
		IModularFeatures::Get().RegisterModularFeature(
			IMonolithGraphFormatter::GetModularFeatureName(),
			Formatter.Get());
		ReportAvailability(true, FString());
		UE_LOG(LogMonolithBABridge, Log,
			TEXT("MonolithBABridge: Registered BA graph formatter"));
#else
		ReportAvailability(false, TEXT("Blueprint Assist is not compiled into this Monolith build."));
		UE_LOG(LogMonolithBABridge, Log,
			TEXT("MonolithBABridge: Blueprint Assist not found at compile time, bridge inactive"));
#endif
	}

	virtual void ShutdownModule() override
	{
		ReportAvailability(false, TEXT("Blueprint Assist bridge is shut down."));
#if WITH_BLUEPRINT_ASSIST
		if (Formatter.IsValid())
		{
			IModularFeatures::Get().UnregisterModularFeature(
				IMonolithGraphFormatter::GetModularFeatureName(),
				Formatter.Get());
			Formatter.Reset();
		}
#endif
	}

private:
	static void ReportAvailability(bool bAvailable, const FString& Reason)
	{
		for (const TCHAR* Namespace : {TEXT("blueprint"), TEXT("animation")})
			FMonolithToolRegistry::Get().SetOptionalDependencyAvailability(
				Namespace, TEXT("BlueprintAssist"), bAvailable, Reason, false);
	}
#if WITH_BLUEPRINT_ASSIST
	TUniquePtr<FMonolithBAFormatterImpl> Formatter;
#endif
};

IMPLEMENT_MODULE(FMonolithBABridgeModule, MonolithBABridge)
