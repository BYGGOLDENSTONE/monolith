// Copyright tumourlove. All Rights Reserved.
// UIPropertyAllowlist.cpp
//
// Implementation: lazy projection over `FUITypeRegistry::PropertyMappings`.
// First call for a given token populates two parallel mutable maps:
//   * `AllowedPaths` — the hot-path TSet used by `IsAllowed`.
//   * `AllowedPathsList` — a stable-ordered TArray for diagnostic dumps.
//
// Both maps share the same key (FName widget token). Keeping them parallel
// (rather than just rebuilding a TArray from the TSet on every call) avoids
// repeated allocations in the dump diagnostic path.

#include "Registry/UIPropertyAllowlist.h"
#include "Registry/UITypeRegistry.h"

#include "Components/Widget.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

namespace
{
    /**
     * True when `WidgetToken` names a loaded UClass that derives from UWidget.
     *
     * Needed because "no registry entry" is NOT the same as "not a widget":
     * `UMonolithUIRegistrySubsystem::PopulateFromReflectionWalk` deliberately
     * skips Blueprint-generated widget classes (WBP_*_C), yet those are real
     * widgets whose UWidget base properties must stay writable. A token that
     * resolves to no widget class at all is genuinely unknown and gets nothing.
     *
     * Tokens are class names with the leading `U` stripped
     * (`MonolithUI::MakeTokenFromClassName`), so both spellings are probed —
     * callers such as the set_widget_property diagnostics pass the raw class
     * name. The Safe variant never logs and is GC/async-load tolerant; the
     * result is cached per token by the caller, so this runs once per type.
     */
    bool TokenNamesWidgetClass(const FName& WidgetToken)
    {
        const UClass* const WidgetBase = UWidget::StaticClass();
        if (!WidgetBase)
        {
            return false;
        }

        const FString TokenStr = WidgetToken.ToString();
        const UClass* Found = FindFirstObjectSafe<UClass>(*TokenStr, EFindFirstObjectOptions::NativeFirst);
        if (!Found)
        {
            Found = FindFirstObjectSafe<UClass>(*(FString(TEXT("U")) + TokenStr), EFindFirstObjectOptions::NativeFirst);
        }

        return Found != nullptr && Found->IsChildOf(WidgetBase);
    }
}

FUIPropertyAllowlist::FUIPropertyAllowlist(const FUITypeRegistry& InRegistry)
    : Registry(InRegistry)
{
}

bool FUIPropertyAllowlist::IsAllowed(const FName& WidgetToken, const FString& JsonPath) const
{
    if (WidgetToken.IsNone() || JsonPath.IsEmpty())
    {
        return false;
    }

    if (!AllowedPaths.Contains(WidgetToken))
    {
        BuildCacheFor(WidgetToken);
    }

    const TSet<FString>* PathSet = AllowedPaths.Find(WidgetToken);
    return PathSet && PathSet->Contains(JsonPath);
}

const TArray<FString>& FUIPropertyAllowlist::GetAllowedPaths(const FName& WidgetToken) const
{
    if (!AllowedPathsList.Contains(WidgetToken))
    {
        BuildCacheFor(WidgetToken);
    }

    if (const TArray<FString>* List = AllowedPathsList.Find(WidgetToken))
    {
        return *List;
    }

    static const TArray<FString> Empty;
    return Empty;
}

void FUIPropertyAllowlist::Invalidate()
{
    AllowedPaths.Reset();
    AllowedPathsList.Reset();
}

void FUIPropertyAllowlist::BuildCacheFor(const FName& WidgetToken) const
{
    TSet<FString>& PathSet = AllowedPaths.FindOrAdd(WidgetToken);
    TArray<FString>& PathList = AllowedPathsList.FindOrAdd(WidgetToken);

    PathSet.Reset();
    PathList.Reset();

    // De-dup helper: keeps the two parallel containers consistent.
    auto AddPath = [&PathSet, &PathList](const FString& Path)
    {
        bool bAlreadyInSet = false;
        PathSet.Add(Path, &bAlreadyInSet);
        if (!bAlreadyInSet)
        {
            PathList.Add(Path);
        }
    };

    const FUITypeRegistryEntry* Entry = Registry.FindByToken(WidgetToken);

    // Safe default: a token that names neither a registry entry nor a loaded
    // UWidget subclass is unknown, and unknown types get NO writes at all —
    // not even the common base props below. This is the contract documented on
    // `IsAllowed`/`GetAllowedPaths` and is what gates the reflection helper.
    // The empty set/list stay cached, so repeated probes short-circuit here.
    if (!Entry && !TokenNamesWidgetClass(WidgetToken))
    {
        return;
    }

    // Common UWidget base-class property paths — allowlisted for every widget
    // token that resolves to a real widget type, registered or not. They live
    // on UWidget itself, so any subclass carries them; the per-type registry
    // below only maps the type-specific surface. Injected before the
    // unregistered-token early-return so Blueprint widget classes (kept out of
    // the registry on purpose) still accept the base props.
    static const TCHAR* const CommonWidgetPaths[] = {
        TEXT("Visibility"),
        TEXT("RenderOpacity"),
        TEXT("ToolTipText"),
        TEXT("bIsEnabled"),
        TEXT("RenderTransform.Angle"),
        TEXT("RenderTransform.Scale"),
        TEXT("RenderTransform.Translation"),
    };
    for (const TCHAR* CommonPath : CommonWidgetPaths)
    {
        AddPath(CommonPath);
    }

    if (!Entry)
    {
        // Real widget class, but outside the curated registry (e.g. a
        // Blueprint widget class). Base props are cached above; there is no
        // type-specific surface to add.
        return;
    }

    PathList.Reserve(PathList.Num() + Entry->PropertyMappings.Num());
    for (const FUIPropertyMapping& Mapping : Entry->PropertyMappings)
    {
        AddPath(Mapping.JsonPath);
    }
}
