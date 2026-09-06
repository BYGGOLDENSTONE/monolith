using UnrealBuildTool;

public class MonolithRuntime : ModuleRules
{
    public MonolithRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "AIModule", "GameplayAbilities",
            "GameplayTags", "GameplayTasks", "UMG", "Slate", "SlateCore"
        });
    }
}
