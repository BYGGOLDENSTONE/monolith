#include "MonolithMeshJsonPresets.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

namespace MonolithJsonPresetIO
{
	/** Read one JSON document. Returns null + reason on failure. */
	static TSharedPtr<FJsonObject> ReadDocument(const FString& FilePath, FString& OutError)
	{
		FString Raw;
		if (!FFileHelper::LoadFileToString(Raw, *FilePath))
		{
			OutError = FString::Printf(TEXT("could not read '%s'"), *FilePath);
			return nullptr;
		}
		TSharedPtr<FJsonObject> Doc;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		if (!FJsonSerializer::Deserialize(Reader, Doc) || !Doc.IsValid())
		{
			OutError = FString::Printf(TEXT("'%s' is not valid JSON"), *FilePath);
			return nullptr;
		}
		return Doc;
	}
}

FMonolithMeshJsonPresets::FMonolithMeshJsonPresets(
	const FString& InBuiltinFileName, const FString& InUserSubDir, const FString& InKindLabel)
	: BuiltinFileName(InBuiltinFileName)
	, UserSubDir(InUserSubDir)
	, KindLabel(InKindLabel)
{
}

FString FMonolithMeshJsonPresets::GetBuiltinFile() const
{
	return FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Config") / BuiltinFileName;
}

FString FMonolithMeshJsonPresets::GetUserDirectory() const
{
	return FPaths::ProjectPluginsDir() / TEXT("Monolith") / TEXT("Saved") / TEXT("Monolith") / UserSubDir;
}

void FMonolithMeshJsonPresets::CollectDocuments(
	TArray<TPair<FString, TSharedPtr<FJsonObject>>>& Out, TArray<FString>& OutWarnings) const
{
	const FString Builtin = GetBuiltinFile();
	if (IFileManager::Get().FileExists(*Builtin))
	{
		FString Err;
		if (TSharedPtr<FJsonObject> Doc = MonolithJsonPresetIO::ReadDocument(Builtin, Err))
		{
			Out.Emplace(Builtin, Doc);
		}
		else
		{
			OutWarnings.Add(Err);
		}
	}
	else
	{
		OutWarnings.Add(FString::Printf(
			TEXT("built-in %s file missing: %s (plugin install is incomplete)"), *KindLabel, *Builtin));
	}

	const FString UserDir = GetUserDirectory();
	TArray<FString> UserFiles;
	IFileManager::Get().FindFiles(UserFiles, *(UserDir / TEXT("*.json")), true, false);
	UserFiles.Sort();
	for (const FString& Leaf : UserFiles)
	{
		const FString Full = UserDir / Leaf;
		FString Err;
		if (TSharedPtr<FJsonObject> Doc = MonolithJsonPresetIO::ReadDocument(Full, Err))
		{
			Out.Emplace(Full, Doc);
		}
		else
		{
			OutWarnings.Add(Err);
		}
	}
}

TMap<FString, FMonolithJsonPreset> FMonolithMeshJsonPresets::LoadPresets(TArray<FString>& OutWarnings) const
{
	TMap<FString, FMonolithJsonPreset> Presets;

	TArray<TPair<FString, TSharedPtr<FJsonObject>>> Docs;
	CollectDocuments(Docs, OutWarnings);

	for (const TPair<FString, TSharedPtr<FJsonObject>>& Doc : Docs)
	{
		const TSharedPtr<FJsonObject>* PresetsObj = nullptr;
		if (!Doc.Value->TryGetObjectField(TEXT("presets"), PresetsObj) || !PresetsObj || !(*PresetsObj).IsValid())
		{
			continue;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PresetsObj)->Values)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(Entry) || !Entry || !(*Entry).IsValid())
			{
				OutWarnings.Add(FString::Printf(TEXT("preset '%s' in '%s' is not an object — skipped"),
					*Pair.Key, *Doc.Key));
				continue;
			}

			FMonolithJsonPreset P;
			P.Name = Pair.Key;
			P.SourceFile = Doc.Key;
			(*Entry)->TryGetStringField(TEXT("type"), P.TypeToken);
			(*Entry)->TryGetStringField(TEXT("description"), P.Description);

			const TSharedPtr<FJsonObject>* Props = nullptr;
			if ((*Entry)->TryGetObjectField(TEXT("properties"), Props) && Props && (*Props).IsValid())
			{
				P.Properties = MakeShared<FJsonObject>();
				P.Properties->Values = (*Props)->Values;
			}
			else
			{
				OutWarnings.Add(FString::Printf(TEXT("preset '%s' in '%s' has no 'properties' object — skipped"),
					*Pair.Key, *Doc.Key));
				continue;
			}

			// Later document wins — user files override built-ins by name.
			Presets.Add(P.Name, MoveTemp(P));
		}
	}

	return Presets;
}

TArray<FString> FMonolithMeshJsonPresets::LoadReadbackKeys(
	const FString& SectionToken, TArray<FString>& OutWarnings) const
{
	TArray<FString> Keys;

	TArray<TPair<FString, TSharedPtr<FJsonObject>>> Docs;
	CollectDocuments(Docs, OutWarnings);

	auto Append = [&Keys](const TSharedPtr<FJsonObject>& Obj, const FString& Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj->TryGetArrayField(Field, Arr) || !Arr)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const FString Name = V.IsValid() ? V->AsString() : FString();
			if (!Name.IsEmpty())
			{
				Keys.AddUnique(Name);
			}
		}
	};

	for (const TPair<FString, TSharedPtr<FJsonObject>>& Doc : Docs)
	{
		const TSharedPtr<FJsonObject>* Readback = nullptr;
		if (!Doc.Value->TryGetObjectField(TEXT("readback"), Readback) || !Readback || !(*Readback).IsValid())
		{
			continue;
		}
		Append(*Readback, TEXT("common"));
		if (!SectionToken.IsEmpty())
		{
			Append(*Readback, SectionToken);
		}
	}

	return Keys;
}

bool FMonolithMeshJsonPresets::ResolvePreset(
	const FString& Name, FMonolithJsonPreset& OutPreset, FString& OutError) const
{
	TArray<FString> Warnings;
	const TMap<FString, FMonolithJsonPreset> Presets = LoadPresets(Warnings);

	if (const FMonolithJsonPreset* Found = Presets.Find(Name))
	{
		OutPreset = *Found;
		OutError.Reset();
		return true;
	}

	// Case-insensitive second pass — preset names are authored by hand.
	for (const TPair<FString, FMonolithJsonPreset>& Pair : Presets)
	{
		if (Pair.Key.Equals(Name, ESearchCase::IgnoreCase))
		{
			OutPreset = Pair.Value;
			OutError.Reset();
			return true;
		}
	}

	TArray<FString> Available;
	Presets.GetKeys(Available);
	Available.Sort();
	OutError = FString::Printf(
		TEXT("Unknown %s '%s'. Available presets (%d): %s. Add your own JSON to %s."),
		*KindLabel, *Name, Available.Num(),
		Available.Num() > 0 ? *FString::Join(Available, TEXT(", ")) : TEXT("<none loaded>"),
		*GetUserDirectory());
	if (Warnings.Num() > 0)
	{
		OutError += FString::Printf(TEXT(" Preset load warnings: %s"), *FString::Join(Warnings, TEXT("; ")));
	}
	return false;
}
