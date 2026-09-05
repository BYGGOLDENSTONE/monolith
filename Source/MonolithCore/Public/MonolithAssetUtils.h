#pragma once

#include "CoreMinimal.h"

class UObject;
class UPackage;
class UBlueprint;
struct FMonolithActionResult;

class MONOLITHCORE_API FMonolithAssetUtils
{
public:
	/** Resolve a user-provided path to a proper asset path (handles /Game/, /Content/, relative, etc.) */
	static FString ResolveAssetPath(const FString& InPath);

	/** Load a package by path, returns nullptr on failure */
	static UPackage* LoadPackageByPath(const FString& AssetPath);

	/** Load an asset object by path, returns nullptr on failure */
	static UObject* LoadAssetByPath(const FString& AssetPath);

	/**
	 * Canonical 4-tier asset lookup with expected-class authoritative check.
	 *
	 * Tier 1: Normalize path (strip .uasset / :SubObject suffix; build Package.AssetName form).
	 * Tier 2: AssetRegistry — class match authoritative; class mismatch is terminal (no disk fall-through).
	 * Tier 3: FindPackage + FindObject — catches freshly-created unsaved assets in this session.
	 * Tier 4: StaticLoadObject — disk fallback. Retries with UObject::StaticClass() if class-typed call fails.
	 *
	 * Returns nullptr on miss, on class mismatch in tiers 2/3, and on bogus path.
	 * Class-mismatch returns nullptr (does NOT silently load wrong-class objects at the same path).
	 */
	static UObject* LoadAssetByPath(UClass* ExpectedClass, const FString& AssetPath);

	/** Load and cast to a specific type */
	template<typename T>
	static T* LoadAssetByPath(const FString& AssetPath)
	{
		return Cast<T>(LoadAssetByPath(T::StaticClass(), AssetPath));
	}

	/** Check if an asset exists at the given path */
	static bool AssetExists(const FString& AssetPath);

	/** Get all assets of a given class in a directory */
	static TArray<FAssetData> GetAssetsByClass(const FTopLevelAssetPath& ClassPath, const FString& PackagePath = FString());

	/** Metadata-only suggestions: at most 256 package names, expected class (including subclasses).
	 * Searches the exact folder, then its parent and up to three similar immediate sibling
	 * folders if empty. Never loads assets, scans synchronously, or traverses recursively. */
	static TArray<FString> GetAssetPathCandidates(const FString& AssetPath, UClass* ExpectedClass = nullptr);

	/** Structured lookup failure retaining the requested path; empty paths are invalid parameters. */
	static FMonolithActionResult AssetNotFound(const FString& Kind, const FString& AssetPath, UClass* ExpectedClass = nullptr);

	/** Get display-friendly name from an asset path */
	static FString GetAssetName(const FString& AssetPath);
};
