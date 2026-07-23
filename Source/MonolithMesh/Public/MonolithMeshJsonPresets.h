#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * One named preset loaded from a Monolith `<Kind>Presets.json` data file.
 *
 * Layout of a preset document (see Config/MonolithLightPresets.json and
 * Config/MonolithAtmospherePresets.json):
 *
 *   {
 *     "readback": { "<token>": [ "PropName", ... ], ... },
 *     "presets":  { "<name>": { "type": "<token>", "description": "...",
 *                               "properties": { "PropName": <value>, ... } } }
 *   }
 *
 * Every key under "properties" is a canonical UPROPERTY name on whatever struct
 * or component the owning action maps `type` to. Values are written through
 * FMonolithReflectionWalker, so nothing is hardcoded in C++.
 */
struct FMonolithJsonPreset
{
	FString Name;
	/** Type token the preset targets ("point", "height_fog", ...). Empty = untyped. */
	FString TypeToken;
	FString Description;
	/** Absolute path of the file the preset came from (built-in or user override). */
	FString SourceFile;
	/** UPROPERTY name -> value. */
	TSharedPtr<FJsonObject> Properties;
};

/**
 * Generic loader for Monolith JSON preset libraries.
 *
 * Built-in file ships with the plugin (Plugins/Monolith/Config/<File>); user files
 * live in Plugins/Monolith/Saved/Monolith/<SubDir>/*.json and override built-ins by
 * preset name. A malformed user file is reported as a warning and never prevents the
 * rest of the library from loading.
 *
 * NOTE (2026-07-24): FMonolithMeshLightActions still carries its own private copy of
 * this logic (MonolithLightPresetIO + LoadPresets/LoadReadbackKeys/ResolvePreset).
 * Migrating it onto this class is a mechanical 3-call change but was deliberately left
 * alone by the slice that introduced this file — that code had just been verified green
 * and destabilising it was not worth the tidy-up.
 */
class FMonolithMeshJsonPresets
{
public:
	/**
	 * @param InBuiltinFileName Leaf name inside Plugins/Monolith/Config (e.g. "MonolithAtmospherePresets.json").
	 * @param InUserSubDir      Leaf name inside Plugins/Monolith/Saved/Monolith (e.g. "AtmospherePresets").
	 * @param InKindLabel       Human word used in error text ("atmosphere preset").
	 */
	FMonolithMeshJsonPresets(const FString& InBuiltinFileName, const FString& InUserSubDir, const FString& InKindLabel);

	/** Absolute path of the built-in file that ships with the plugin. */
	FString GetBuiltinFile() const;

	/** Absolute path of the user override directory (may not exist). */
	FString GetUserDirectory() const;

	/** Built-ins first, then user files (which override by name). */
	TMap<FString, FMonolithJsonPreset> LoadPresets(TArray<FString>& OutWarnings) const;

	/**
	 * Read-back property names for one section token, in document order with
	 * duplicates collapsed. Sections named "common" are always prepended when present.
	 */
	TArray<FString> LoadReadbackKeys(const FString& SectionToken, TArray<FString>& OutWarnings) const;

	/**
	 * Resolve one preset by name (exact, then case-insensitive).
	 * On a miss OutError lists every available name and the user override directory.
	 */
	bool ResolvePreset(const FString& Name, FMonolithJsonPreset& OutPreset, FString& OutError) const;

private:
	void CollectDocuments(TArray<TPair<FString, TSharedPtr<FJsonObject>>>& Out, TArray<FString>& OutWarnings) const;

	FString BuiltinFileName;
	FString UserSubDir;
	FString KindLabel;
};
