#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
class UWidgetBlueprint;

namespace MonolithUI::MenuAuthoring
{
    bool ApplyScreen(UWidgetBlueprint* WBP, const FString& FocusTarget,
        const TArray<TSharedPtr<FJsonObject>>& Navigation, FString& Error);
    bool ApplyLayers(UWidgetBlueprint* WBP, const TArray<TSharedPtr<FJsonObject>>& Layers,
        const TMap<FString, FString>& ScreenPaths, FString& Error);
}
