#include "Misc/AutomationTest.h"
#include "MonolithEditorJobs.h"
#include "MonolithEditorActions.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithEditorJobValidationTest, "Monolith.Editor.Jobs.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FMonolithEditorJobValidationTest::RunTest(const FString&)
{
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("job_id"), FGuid::NewGuid().ToString());
	TestFalse(TEXT("unknown status rejected"), FMonolithEditorJobs::Status(Params).bSuccess);
	TestFalse(TEXT("unknown cancel rejected"), FMonolithEditorJobs::Cancel(Params).bSuccess);
	TestFalse(TEXT("unknown result rejected"), FMonolithEditorJobs::Result(Params).bSuccess);
	TestFalse(TEXT("arbitrary executable rejected"), FMonolithEditorJobs::StartEncoder(TEXT("cmd"), TEXT(""), {}, 15, 256, Params).bSuccess);
	TestFalse(TEXT("empty frames rejected before process launch"), FMonolithEditorJobs::StartEncoder(TEXT("python"), TEXT(""), {}, 15, 256, Params).bSuccess);
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotExist"));
	Params->SetStringField(TEXT("encoder"), TEXT("bad_encoder"));
	const auto Invalid = FMonolithEditorActions::HandleCaptureSystemGif(Params);
	TestFalse(TEXT("invalid encoder rejected"), Invalid.bSuccess);
	TestTrue(TEXT("encoder validation happens before asset loading"), Invalid.ErrorMessage.Contains(TEXT("Unknown encoder")));
	return true;
}
#endif
