#include "Misc/AutomationTest.h"
#include "MonolithMaterialActions.h"
#include "MonolithPackagePathValidator.h"
#include "MonolithJsonUtils.h"
#include "Materials/Material.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithMaterialWriteSafetyTest,
	"Monolith.Material.WriteSafety", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithMaterialWriteSafetyTest::RunTest(const FString& Parameters)
{
	const FString EnginePath = TEXT("/Engine/EngineMaterials/DefaultMaterial");
	UMaterial* Material = LoadObject<UMaterial>(nullptr, *(EnginePath + TEXT(".DefaultMaterial")));
	if (!TestNotNull(TEXT("Engine default material is available"), Material))
	{
		return false;
	}
	const bool bWasTwoSided = Material->TwoSided;
	const bool bWasDirty = Material->GetPackage()->IsDirty();

	auto CheckRejection = [&](const FMonolithActionResult& Result)
	{
		TestFalse(TEXT("Engine material write is rejected"), Result.bSuccess);
		TestEqual(TEXT("Write rejection uses invalid params"), Result.ErrorCode, FMonolithJsonUtils::ErrInvalidParams);
		if (TestTrue(TEXT("Write rejection carries data"), Result.ErrorData.IsValid()))
		{
			const TSharedPtr<FJsonObject> Data = Result.ErrorData->AsObject();
			if (TestTrue(TEXT("Write rejection data is an object"), Data.IsValid()))
			{
				TestEqual(TEXT("Write rejection reason"), Data->GetStringField(TEXT("reason")), FString(TEXT("path_not_writable")));
				TestFalse(TEXT("Write rejection was not executed"), Data->GetBoolField(TEXT("executed")));
				const TArray<TSharedPtr<FJsonValue>>* Roots = nullptr;
				TestTrue(TEXT("Write rejection lists accepted roots"), Data->TryGetArrayField(TEXT("accepted_roots"), Roots) && Roots->Num() > 0);
			}
		}
		TestEqual(TEXT("Engine material property remains unchanged"), static_cast<bool>(Material->TwoSided), bWasTwoSided);
		TestEqual(TEXT("Engine package dirty state remains unchanged"), Material->GetPackage()->IsDirty(), bWasDirty);
	};

	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), EnginePath);
	Params->SetBoolField(TEXT("two_sided"), !bWasTwoSided);
	CheckRejection(FMonolithMaterialActions::SetMaterialProperty(Params));

	auto BatchParams = MakeShared<FJsonObject>();
	BatchParams->SetArrayField(TEXT("asset_paths"), { MakeShared<FJsonValueString>(EnginePath) });
	BatchParams->SetBoolField(TEXT("two_sided"), !bWasTwoSided);
	CheckRejection(FMonolithMaterialActions::BatchSetMaterialProperty(BatchParams));

	FString Error;
	TestTrue(TEXT("Disposable material test path is writable"),
		MonolithCore::EnsureWritablePackagePath(TEXT("/Game/Tests/Monolith/Material/M_WriteSafety"), Error));
	TestTrue(TEXT("Writable test path has no error"), Error.IsEmpty());
	return true;
}

#endif
