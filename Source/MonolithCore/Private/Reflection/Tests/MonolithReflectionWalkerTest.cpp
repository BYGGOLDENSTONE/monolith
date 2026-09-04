// SPDX-License-Identifier: MIT
// Native/transient fixtures: no project test assets or saved packages required.
#include <initializer_list>
#include "Misc/AutomationTest.h"
#include "Reflection/MonolithReflectionWalker.h"
#include "MonolithBulkFillTypes.h"
#include "Components/SceneComponent.h"
#include "ComponentInstanceDataCache.h"
#include "Engine/Blueprint.h"
#include "UObject/EnumProperty.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace MonolithWalkerTest
{
	static TArray<TSharedPtr<FJsonValue>> Strings(std::initializer_list<const TCHAR*> Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const TCHAR* Value : Values) { Result.Add(MakeShared<FJsonValueString>(Value)); }
		return Result;
	}
	static TSharedPtr<FJsonObject> Containers()
	{
		auto Tree = MakeShared<FJsonObject>();
		Tree->SetArrayField(TEXT("CategorySorting"), Strings({TEXT("Combat"), TEXT("Movement")}));
		Tree->SetArrayField(TEXT("ImportedNamespaces"), Strings({TEXT("Game.Combat"), TEXT("Game.UI"), TEXT("Game.Combat")}));
		auto Map = MakeShared<FJsonObject>();
		Map->SetNumberField(TEXT("Sword"), 7);
		Map->SetNumberField(TEXT("Shield"), 11);
		Tree->SetObjectField(TEXT("ComponentTemplateNameIndex"), Map);
		return Tree;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithReflectionWalkerScalarsAndContainersTest,
	"Leviathan.Monolith.Reflection.WalkerWritesAllScalarsAndContainers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithReflectionWalkerScalarsAndContainersTest::RunTest(const FString& Parameters)
{
	FBulkFillSpec Spec;
	Spec.bStrict = true;
	TStrongObjectPtr<UBlueprint> BP(NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient));
	BP->CategorySorting.Add(TEXT("Old"));
	BP->ImportedNamespaces.Add(TEXT("Old"));
	BP->ComponentTemplateNameIndex.Add(TEXT("Old"), 99);
	const auto Report = FMonolithReflectionWalker::WriteTree(MonolithWalkerTest::Containers(), UBlueprint::StaticClass(), BP.Get(), BP.Get(), Spec);
	TestEqual(TEXT("Container writes accepted"), Report.Errors, 0);
	TestTrue(TEXT("Strict successful write would apply"), Report.bWouldApply);
	TestEqual(TEXT("Array replaced"), BP->CategorySorting.Num(), 2);
	if (BP->CategorySorting.Num() == 2)
	{
		TestEqual(TEXT("FName array value"), BP->CategorySorting[0], FName(TEXT("Combat")));
		TestEqual(TEXT("Array order preserved"), BP->CategorySorting[1], FName(TEXT("Movement")));
	}
	TestEqual(TEXT("Set deduplicates"), BP->ImportedNamespaces.Num(), 2);
	TestTrue(TEXT("Set contains inserted string"), BP->ImportedNamespaces.Contains(TEXT("Game.Combat")));
	TestFalse(TEXT("Old set entry removed"), BP->ImportedNamespaces.Contains(TEXT("Old")));
	TestEqual(TEXT("Map replaced"), BP->ComponentTemplateNameIndex.Num(), 2);
	TestEqual(TEXT("Name-key integer value"), BP->ComponentTemplateNameIndex.FindRef(TEXT("Sword")), 7);
	TestEqual(TEXT("Second map value"), BP->ComponentTemplateNameIndex.FindRef(TEXT("Shield")), 11);

	FDryRunReport Fixture;
	auto Tree = MakeShared<FJsonObject>();
	Tree->SetNumberField(TEXT("Errors"), 42);
	Tree->SetBoolField(TEXT("bWouldApply"), true);
	auto Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("Path"), TEXT("Player.Health"));
	Entry->SetBoolField(TEXT("bOk"), true);
	Tree->SetArrayField(TEXT("FieldWrites"), {MakeShared<FJsonValueObject>(Entry)});
	const auto Nested = FMonolithReflectionWalker::WriteTree(Tree, FDryRunReport::StaticStruct(), &Fixture, nullptr, Spec);
	TestEqual(TEXT("Nested writes accepted"), Nested.Errors, 0);
	TestEqual(TEXT("Integer scalar written"), Fixture.Errors, 42);
	TestTrue(TEXT("Boolean scalar written"), Fixture.bWouldApply);
	TestEqual(TEXT("Nested element allocated"), Fixture.FieldWrites.Num(), 1);
	if (Fixture.FieldWrites.Num() == 1)
	{
		TestEqual(TEXT("Nested FString written"), Fixture.FieldWrites[0].Path, FString(TEXT("Player.Health")));
		TestTrue(TEXT("Nested bool written"), Fixture.FieldWrites[0].bOk);
	}
	FLinearColor Color(0, 0, 0, 1);
	auto ColorTree = MakeShared<FJsonObject>();
	ColorTree->SetNumberField(TEXT("R"), 0.25);
	const auto Floats = FMonolithReflectionWalker::WriteTree(ColorTree, TBaseStructure<FLinearColor>::Get(), &Color, nullptr, Spec);
	TestEqual(TEXT("Float accepted"), Floats.Errors, 0);
	TestEqual(TEXT("Float written"), Color.R, 0.25f);
	TestEqual(TEXT("Unspecified field preserved"), Color.A, 1.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithReflectionWalkerRejectsTypeCoerceTrapTest,
	"Leviathan.Monolith.Reflection.WalkerRejectsTypeCoerceTrap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithReflectionWalkerRejectsTypeCoerceTrapTest::RunTest(const FString& Parameters)
{
	FBulkFillFieldWrite Fixture;
	Fixture.Path = TEXT("preserve-me");
	FBulkFillSpec Spec;
	Spec.bStrict = true;
	TArray<TSharedPtr<FJsonValue>> Invalid;
	Invalid.Add(MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
	Invalid.Add(MakeShared<FJsonValueArray>(MonolithWalkerTest::Strings({TEXT("wrong-shape")})));
	for (const auto& Value : Invalid)
	{
		auto Tree = MakeShared<FJsonObject>();
		Tree->SetField(TEXT("Path"), Value);
		const auto Result = FMonolithReflectionWalker::WriteTree(Tree, FBulkFillFieldWrite::StaticStruct(), &Fixture, nullptr, Spec);
		TestEqual(TEXT("Wrong shape reported"), Result.Errors, 1);
		TestFalse(TEXT("Strict mismatch rejected"), Result.bWouldApply);
		TestEqual(TEXT("Existing string never erased"), Fixture.Path, FString(TEXT("preserve-me")));
		TestEqual(TEXT("One field result"), Result.FieldWrites.Num(), 1);
		if (Result.FieldWrites.Num() == 1)
		{
			TestFalse(TEXT("Field reports failure"), Result.FieldWrites[0].bOk);
			TestTrue(TEXT("Actionable shape error"), Result.FieldWrites[0].Reason.Contains(TEXT("scalar")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithReflectionWalkerStrictModeBlocksUnknownKeyTest,
	"Leviathan.Monolith.Reflection.WalkerStrictModeBlocksUnknownKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithReflectionWalkerStrictModeBlocksUnknownKeyTest::RunTest(const FString& Parameters)
{
	FBulkFillFieldWrite Fixture;
	Fixture.Path = TEXT("unchanged");
	auto Tree = MakeShared<FJsonObject>();
	Tree->SetNumberField(TEXT("NotAFieldOnThisStruct"), 42);
	FBulkFillSpec Spec;
	const auto Permissive = FMonolithReflectionWalker::WriteTree(Tree, FBulkFillFieldWrite::StaticStruct(), &Fixture, nullptr, Spec);
	TestEqual(TEXT("Permissive reports unknown field"), Permissive.Errors, 1);
	TestTrue(TEXT("Permissive allows caller to proceed"), Permissive.bWouldApply);
	Spec.bStrict = true;
	const auto Strict = FMonolithReflectionWalker::WriteTree(Tree, FBulkFillFieldWrite::StaticStruct(), &Fixture, nullptr, Spec);
	TestEqual(TEXT("Strict reports unknown field"), Strict.Errors, 1);
	TestFalse(TEXT("Strict rejects report"), Strict.bWouldApply);
	TestEqual(TEXT("Unknown key changed no data"), Fixture.Path, FString(TEXT("unchanged")));
	TestEqual(TEXT("One diagnostic"), Strict.FieldWrites.Num(), 1);
	if (Strict.FieldWrites.Num() == 1)
	{
		TestEqual(TEXT("Offending field identified"), Strict.FieldWrites[0].Path, FString(TEXT("NotAFieldOnThisStruct")));
		TestTrue(TEXT("Unknown field reason"), Strict.FieldWrites[0].Reason.Contains(TEXT("unknown field")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithReflectionWalkerEnumMissReportsTypoTest,
	"Leviathan.Monolith.Reflection.WalkerEnumMissReportsTypo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithReflectionWalkerEnumMissReportsTypoTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<USceneComponent> Component(NewObject<USceneComponent>(GetTransientPackage(), NAME_None, RF_Transient));
	FProperty* Property = FMonolithReflectionWalker::FindPropertyForwarding(UActorComponent::StaticClass(), TEXT("CreationMethod"));
	if (!TestNotNull(TEXT("Native enum fixture"), CastField<FEnumProperty>(Property))) { return false; }
	const auto Before = Component->CreationMethod;
	auto Tree = MakeShared<FJsonObject>();
	Tree->SetStringField(TEXT("CreationMethod"), TEXT("Natve"));
	FBulkFillSpec Spec;
	Spec.bStrict = true;
	const auto Result = FMonolithReflectionWalker::WriteTree(Tree, UActorComponent::StaticClass(), Component.Get(), Component.Get(), Spec);
	TestEqual(TEXT("Enum typo rejected"), Result.Errors, 1);
	TestFalse(TEXT("Invalid enum cannot apply"), Result.bWouldApply);
	TestEqual(TEXT("Enum remains unchanged"), Component->CreationMethod, Before);
	TestEqual(TEXT("One enum diagnostic"), Result.FieldWrites.Num(), 1);
	if (Result.FieldWrites.Num() == 1)
	{
		TestTrue(TEXT("Typo in diagnostic"), Result.FieldWrites[0].Reason.Contains(TEXT("Natve")));
		TestTrue(TEXT("Legal example in diagnostic"), Result.FieldWrites[0].Reason.Contains(TEXT("Native")));
	}
	Tree->SetStringField(TEXT("CreationMethod"), TEXT("Instance"));
	const auto Valid = FMonolithReflectionWalker::WriteTree(Tree, UActorComponent::StaticClass(), Component.Get(), Component.Get(), Spec);
	TestEqual(TEXT("Valid enum accepted"), Valid.Errors, 0);
	TestEqual(TEXT("Valid enum written"), Component->CreationMethod, EComponentCreationMethod::Instance);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithReflectionWalkerDryRunNoSideEffectsTest,
	"Leviathan.Monolith.Reflection.DryRunNoSideEffects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithReflectionWalkerDryRunNoSideEffectsTest::RunTest(const FString& Parameters)
{
	TStrongObjectPtr<UBlueprint> BP(NewObject<UBlueprint>(GetTransientPackage(), NAME_None, RF_Transient));
	BP->CategorySorting.Add(TEXT("ExistingCategory"));
	BP->ImportedNamespaces.Add(TEXT("Existing.Namespace"));
	BP->ComponentTemplateNameIndex.Add(TEXT("ExistingComponent"), 123);
	const auto CategoriesBefore = BP->CategorySorting;
	const auto NamespacesBefore = BP->ImportedNamespaces;
	const auto MapBefore = BP->ComponentTemplateNameIndex;
	FBulkFillSpec Spec;
	Spec.bDryRun = true;
	Spec.bStrict = true;
	const auto Result = FMonolithReflectionWalker::InspectTree(MonolithWalkerTest::Containers(), UBlueprint::StaticClass(), BP.Get(), Spec);
	TestEqual(TEXT("Dry run validates containers"), Result.Errors, 0);
	TestTrue(TEXT("Nested proposals returned"), Result.FieldWrites.Num() >= 3);
	TestFalse(TEXT("Dry run does not apply"), Result.bWouldApply);
	TestTrue(TEXT("Array unchanged"), BP->CategorySorting == CategoriesBefore);
	TestTrue(TEXT("Set unchanged"), BP->ImportedNamespaces.Includes(NamespacesBefore) && BP->ImportedNamespaces.Num() == NamespacesBefore.Num());
	TestTrue(TEXT("Map unchanged"), BP->ComponentTemplateNameIndex.OrderIndependentCompareEqual(MapBefore));
	return true;
}
#endif
