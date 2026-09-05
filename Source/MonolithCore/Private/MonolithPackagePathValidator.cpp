#include "MonolithPackagePathValidator.h"
#include "MonolithSettings.h"
#include "MonolithJsonUtils.h"

namespace MonolithWritablePathDetail
{
	static FString NormalizeRoot(FString Root)
	{
		while (Root.EndsWith(TEXT("/"))) Root.LeftChopInline(1);
		return Root;
	}

	static bool IsUnder(const FString& Path, const FString& Root)
	{
		return Path.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase);
	}

	static bool IsProtected(const FString& Path)
	{
		return Path.Equals(TEXT("/Engine"), ESearchCase::IgnoreCase)
			|| Path.Equals(TEXT("/Script"), ESearchCase::IgnoreCase)
			|| IsUnder(Path, TEXT("/Engine")) || IsUnder(Path, TEXT("/Script"));
	}
}

TArray<FString> MonolithCore::GetWritablePackageRoots()
{
	using namespace MonolithWritablePathDetail;
	TArray<FString> Roots{TEXT("/Game")};
	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (!Settings) return Roots;
	for (const FString& ConfigRoot : Settings->WritablePluginContentRoots)
	{
		const FString Root = NormalizeRoot(ConfigRoot);
		FString Normalized;
		if (!IsProtected(Root) && ValidatePackagePath(Root + TEXT("/MonolithWriteProbe"), Normalized).IsEmpty())
		{
			Roots.AddUnique(Root);
		}
	}
	// Additional indexed content is accepted only inside an authorized mount.
	// An index/search setting must never override Engine/Script protection or
	// the explicit opt-in required for writing plugin content.
	for (const FString& ConfigRoot : Settings->AdditionalContentPaths)
	{
		const FString Root = NormalizeRoot(ConfigRoot);
		FString Normalized;
		if (!IsProtected(Root) && ValidatePackagePath(Root + TEXT("/MonolithWriteProbe"), Normalized).IsEmpty()
			&& Roots.ContainsByPredicate([&](const FString& Allowed)
			{ return Root.Equals(Allowed, ESearchCase::IgnoreCase) || IsUnder(Root, Allowed); }))
		{
			Roots.AddUnique(Root);
		}
	}
	return Roots;
}

bool MonolithCore::EnsureWritablePackagePath(const FString& PackagePath, FString& OutError)
{
	FString Normalized;
	OutError = ValidatePackagePath(PackagePath, Normalized);
	if (!OutError.IsEmpty()) return false;
	if (!MonolithWritablePathDetail::IsProtected(Normalized))
	{
		for (const FString& Root : GetWritablePackageRoots())
		{
			if (MonolithWritablePathDetail::IsUnder(Normalized, Root)) return true;
		}
	}
	OutError = FString::Printf(TEXT("Package path '%s' is not writable. Accepted roots: %s. Plugin writes require WritablePluginContentRoots."),
		*PackagePath, *FString::Join(GetWritablePackageRoots(), TEXT(", ")));
	return false;
}

FMonolithActionResult MonolithCore::WritablePathError(const FString& PackagePath, const FString& Error)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("reason"), TEXT("path_not_writable"));
	Data->SetStringField(TEXT("path"), PackagePath);
	Data->SetBoolField(TEXT("executed"), false);
	TArray<TSharedPtr<FJsonValue>> Roots;
	for (const FString& Root : GetWritablePackageRoots()) Roots.Add(MakeShared<FJsonValueString>(Root));
	Data->SetArrayField(TEXT("accepted_roots"), Roots);
	return FMonolithActionResult::Error(Error, FMonolithJsonUtils::ErrInvalidParams).WithErrorData(Data);
}
