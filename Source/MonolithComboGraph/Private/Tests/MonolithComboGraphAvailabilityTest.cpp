// SPDX-License-Identifier: MIT
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithComboGraphAvailabilityTest,
	"Monolith.ComboGraph.OptionalAvailability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithComboGraphAvailabilityTest::RunTest(const FString& Parameters)
{
	if (!GetDefault<UMonolithSettings>()->bEnableComboGraph)
	{
		AddInfo(TEXT("SKIP: ComboGraph integration is explicitly disabled in settings"));
		return true;
	}
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const TArray<FMonolithActionInfo> Actions = Registry.GetActions(TEXT("combograph"));
	TestEqual(TEXT("All ComboGraph action registrations are retained"), Actions.Num(), 13);
	for (const FMonolithActionInfo& Action : Actions)
	{
		TestTrue(TEXT("Each retained action exposes its schema"), Action.ParamSchema.IsValid());
	}

	const FMonolithActionResult Inventory = Registry.ExecuteAction(TEXT("monolith"), TEXT("discover"), MakeShared<FJsonObject>());
	if (!TestTrue(TEXT("Namespace inventory succeeds"), Inventory.bSuccess && Inventory.Result.IsValid())) return false;
	const TArray<TSharedPtr<FJsonValue>>* Namespaces = nullptr;
	if (!TestTrue(TEXT("Inventory contains namespaces"), Inventory.Result->TryGetArrayField(TEXT("namespaces"), Namespaces))) return false;
	TSharedPtr<FJsonObject> Availability;
	for (const auto& Value : *Namespaces)
	{
		const auto Row = Value->AsObject();
		if (Row->GetStringField(TEXT("namespace")) == TEXT("combograph"))
		{
			const TSharedPtr<FJsonObject>* Found = nullptr;
			if (Row->TryGetObjectField(TEXT("availability"), Found)) Availability = *Found;
			break;
		}
	}
	if (!TestTrue(TEXT("ComboGraph discovery includes availability"), Availability.IsValid())) return false;
	TestEqual(TEXT("Availability matches compiled dependency"), Availability->GetBoolField(TEXT("available")), WITH_COMBOGRAPH != 0);
	TestEqual(TEXT("Public dependency name is stable"), Availability->GetStringField(TEXT("required_plugin")), FString(TEXT("ComboGraph")));
#if WITH_COMBOGRAPH
	AddInfo(TEXT("SKIP absent-dependency action branch: ComboGraph is compiled in this build; inventory and schemas verified"));
#else
	TestEqual(TEXT("Absent dependency reason"), Availability->GetStringField(TEXT("reason")), FString(TEXT("not_compiled")));
	const FMonolithActionResult Result = Registry.ExecuteAction(TEXT("combograph"), TEXT("list_combo_graphs"), MakeShared<FJsonObject>());
	TestFalse(TEXT("Absent dependency action fails"), Result.bSuccess);
	TestEqual(TEXT("Absent dependency error code"), Result.ErrorCode, FMonolithJsonUtils::ErrOptionalDepUnavailable);
	const TSharedPtr<FJsonObject>* Data = nullptr;
	if (!TestTrue(TEXT("Absent dependency error has data"), Result.ErrorData.IsValid() && Result.ErrorData->TryGetObject(Data))) return false;
	TestEqual(TEXT("Error class"), (*Data)->GetStringField(TEXT("class")), FString(TEXT("optional_dep_unavailable")));
	TestEqual(TEXT("Error names dependency"), (*Data)->GetStringField(TEXT("dep_name")), FString(TEXT("ComboGraph")));
	TestFalse(TEXT("No action execution"), (*Data)->GetBoolField(TEXT("executed")));
	TestFalse(TEXT("Missing plugin cannot be retried unchanged"), (*Data)->GetBoolField(TEXT("retryable")));
#endif
	return true;
}
#endif
