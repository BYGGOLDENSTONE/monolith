// Copyright tumourlove. All Rights Reserved.
// UIPackageFullyLoadTests.cpp
//
// Regression cover for the repo-wide "missing Package->FullyLoad()" defect.
//
// UPackage::SavePackage refuses (SavePackage2.cpp:226, "cannot be saved as it has only been
// partially loaded") to write any package that has a file on disk but whose
// bHasBeenFullyLoaded flag is still false. FSavePackageArgs::Error defaults to GError, so that
// refusal is not a soft `false` return — it is appError, i.e. it takes the whole editor process
// down. This is the state a package is left in when it was pulled
// into memory as an *import* of some other asset, or when an object inside it was reached via
// FAssetData::GetAsset() / a dotted LoadObject (both of which short-circuit on
// StaticFindObjectFast and never finish the package load, UObjectGlobals.cpp:1344).
//
// Every Monolith code path that obtains a package and later saves it must therefore call
// Package->FullyLoad() in between. This test pins that contract for the MonolithUI scaffolder,
// which is the most user-facing instance: ui::scaffold_* all go through
// MonolithUIInternal::CreateNewWidgetBlueprint + SaveAndCompileWidgetBlueprint.
//
// Test list:
//   MonolithUI.PackageFullyLoad.SaveAndCompileReclaimsPartiallyLoadedPackage
//       Puts a real on-disk fixture package into the exact partially-loaded state the engine
//       rejects (UPackage::MarkAsUnloaded + non-zero file size), then runs the production save
//       helper and asserts the package came back fully loaded and the write actually landed.
//       Verified negatively: with MonolithUIInternal::SaveAndCompileWidgetBlueprint's
//       FullyLoad() removed, this test does not merely fail — it aborts the whole run with
//       "Asset '.../WBP_PartialReclaim.uasset' cannot be saved as it has only been partially
//       loaded" at the SaveAndCompileWidgetBlueprint call. So if the suite ever comes back
//       passed=0 / failed=-1 pointing at this file, the cause is a reintroduced missing
//       Package->FullyLoad() somewhere on that path, not a flaky fixture.
//
// Fixture lands under /Game/Tests/Monolith/UI/PackageFullyLoad/ per the test-asset rule.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "WidgetBlueprint.h"
#include "Components/Image.h"

#include "MonolithUIInternal.h"
#include "Tests/Hoisted/MonolithUITestFixtureUtils.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FMonolithUIPackageFullyLoadSaveAndCompileTest,
    "MonolithUI.PackageFullyLoad.SaveAndCompileReclaimsPartiallyLoadedPackage",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithUIPackageFullyLoadSaveAndCompileTest::RunTest(const FString& Parameters)
{
    const FString AssetPath = TEXT("/Game/Tests/Monolith/UI/PackageFullyLoad/WBP_PartialReclaim");

    // --- Arrange: a real fixture package, written to disk. -------------------------------
    FString FixtureError;
    UWidgetBlueprint* WBP = nullptr;
    if (!TestTrue(
            FString::Printf(TEXT("fixture created (%s)"), *FixtureError),
            MonolithUI::TestUtils::CreateOrReuseTestWidgetBlueprint(
                AssetPath,
                TEXT("Img"),
                UImage::StaticClass(),
                FixtureError,
                /*OutChildWidget=*/ nullptr,
                &WBP)))
    {
        return false;
    }
    if (!TestNotNull(TEXT("fixture WBP"), WBP))
    {
        return false;
    }

    UPackage* Package = WBP->GetPackage();
    if (!TestNotNull(TEXT("fixture package"), Package))
    {
        return false;
    }

    const FString PackageFilename = FPackageName::LongPackageNameToFilename(
        AssetPath, FPackageName::GetAssetPackageExtension());
    if (!TestTrue(TEXT("fixture .uasset exists on disk"),
            IFileManager::Get().FileSize(*PackageFilename) > 0))
    {
        return false;
    }

    // --- Arrange: force the exact state SavePackage rejects. ------------------------------
    // MarkAsUnloaded() clears bHasBeenFullyLoaded. UPackage::IsFullyLoaded then falls through
    // to its "is there a file on disk?" probe (Package.cpp:355-367) — and since the fixture
    // .uasset above really exists, it answers "partially loaded". That is exactly the state the
    // engine leaves a package in when it is pulled in as an import of another asset, or when
    // CreatePackage hands back a shell for a path whose .uasset the registry has not scanned.
    // Reproducing it with public API keeps the test deterministic instead of depending on what
    // some earlier test happened to load.
    Package->MarkAsUnloaded();
    if (!TestFalse(TEXT("precondition: package reports partially loaded"), Package->IsFullyLoaded()))
    {
        return false;
    }
    Package->MarkPackageDirty();

    // --- Act: the production save path used by every ui::scaffold_* action. ---------------
    MonolithUIInternal::SaveAndCompileWidgetBlueprint(WBP, AssetPath);

    // --- Assert -------------------------------------------------------------------------
    // FullyLoad() ran: the package is whole again. Without the fix this stays false.
    TestTrue(TEXT("SaveAndCompileWidgetBlueprint reclaimed the partially-loaded package"),
        Package->IsFullyLoaded());

    // The write actually landed: SavePackage clears the dirty flag only on success
    // (SavePackage2.cpp:3560). Without the fix SavePackage bails out before that.
    TestFalse(TEXT("package was written to disk (dirty flag cleared)"), Package->IsDirty());

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
