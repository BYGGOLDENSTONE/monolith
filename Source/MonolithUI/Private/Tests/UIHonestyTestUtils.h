#pragma once

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DirectoryWatcherModule.h"
#include "IDirectoryWatcher.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"
#include "ObjectTools.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/VerticalBox.h"

namespace MonolithUI::HonestyTests
{
    // Every instance owns a fresh GUID path; cleanup cannot touch existing assets.
    struct FScopedWidget
    {
        FAutomationTestBase& Test;
        const FString Path;

        explicit FScopedWidget(FAutomationTestBase& InTest)
            : Test(InTest)
            , Path(TEXT("/Game/Tests/Monolith/UI/Phase0/WBP_") + FGuid::NewGuid().ToString(EGuidFormats::Digits))
        {
        }

        ~FScopedWidget()
        {
            if (UWidgetBlueprint* Widget = FindObject<UWidgetBlueprint>(nullptr, *ObjectPath()))
            {
                // Save and deletion happen in the same Automation tick. Deliver
                // queued file-add notifications before AssetDeleted marks the
                // package empty, otherwise the next watcher tick sees a stale
                // add after deletion and reports that the package reappeared.
                IDirectoryWatcher* Watcher = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(
                    TEXT("DirectoryWatcher")).Get();
                if (Watcher) Watcher->Tick(-1.f);
                IAssetRegistry& Registry = FAssetRegistryModule::GetRegistry();
                Registry.ScanModifiedAssetFiles({ Filename() });
                Registry.WaitForCompletion();
                TArray<UObject*> Assets = { Widget };
                Test.TestEqual(TEXT("GUID-owned fixture is deleted"), ObjectTools::ForceDeleteObjects(Assets, false), 1);
                if (Watcher) Watcher->Tick(-1.f);
                Registry.WaitForCompletion();
            }
            Test.TestFalse(TEXT("Fixture leaves no saved asset"), IFileManager::Get().FileExists(*Filename()));
        }

        FString ObjectPath() const
        {
            return Path + TEXT(".") + FPackageName::GetLongPackageAssetName(Path);
        }

        FString Filename() const
        {
            return FPackageName::LongPackageNameToFilename(Path, FPackageName::GetAssetPackageExtension());
        }

        UWidgetBlueprint* Load() const
        {
            return LoadObject<UWidgetBlueprint>(nullptr, *ObjectPath());
        }
    };

    inline TSharedPtr<FJsonObject> MinimalSpec()
    {
        return FMonolithJsonUtils::Parse(TEXT("{\"name\":\"Phase0Honesty\",\"parentClass\":\"UserWidget\",\"rootWidget\":{\"type\":\"VerticalBox\",\"id\":\"RootBox\"}}"));
    }

    inline TSharedPtr<FJsonObject> Screen(const FString& Path, bool bWithSpec)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetStringField(TEXT("id"), TEXT("screen"));
        Result->SetStringField(TEXT("asset_path"), Path);
        if (bWithSpec) Result->SetObjectField(TEXT("spec"), MinimalSpec());
        else Result->SetStringField(TEXT("kind"), TEXT("main_menu"));
        return Result;
    }

    inline TSharedPtr<FJsonObject> Menu(const FScopedWidget& Widget, bool bWithSpec, bool bDryRun)
    {
        TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
        Result->SetArrayField(TEXT("screens"), { MakeShared<FJsonValueObject>(Screen(Widget.Path, bWithSpec)) });
        Result->SetBoolField(TEXT("dry_run"), bDryRun);
        Result->SetBoolField(TEXT("overwrite"), false);
        return Result;
    }

    inline bool HasString(const TSharedPtr<FJsonObject>& Data, const TCHAR* Key, const FString& Value)
    {
        const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
        if (!Data.IsValid() || !Data->TryGetArrayField(Key, Values)) return false;
        for (const TSharedPtr<FJsonValue>& Item : *Values)
        {
            FString ItemString;
            if (Item.IsValid() && Item->TryGetString(ItemString) && ItemString == Value) return true;
        }
        return false;
    }

    inline TSharedPtr<FJsonObject> CheckNotImplemented(FAutomationTestBase& Test, const FMonolithActionResult& Result)
    {
        Test.TestFalse(TEXT("Unsupported work is an action error"), Result.bSuccess);
        Test.TestEqual(TEXT("Capability error code"), Result.ErrorCode, FMonolithJsonUtils::ErrNotImplemented);
        const TSharedPtr<FJsonObject>* Data = nullptr;
        if (!Result.ErrorData.IsValid() || !Result.ErrorData->TryGetObject(Data) || !Data || !Data->IsValid())
        {
            Test.AddError(TEXT("Missing structured capability error data"));
            return nullptr;
        }
        Test.TestEqual(TEXT("Machine-readable reason"), (*Data)->GetStringField(TEXT("reason")), FString(TEXT("not_implemented")));
        Test.TestTrue(TEXT("Implemented flag is present"), (*Data)->HasTypedField<EJson::Boolean>(TEXT("implemented")));
        Test.TestFalse(TEXT("Unsupported feature is not implemented"), (*Data)->GetBoolField(TEXT("implemented")));
        return *Data;
    }
}
