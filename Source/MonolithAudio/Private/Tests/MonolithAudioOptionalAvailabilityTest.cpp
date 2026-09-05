#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "MonolithJsonUtils.h"
#include "MonolithToolRegistry.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMonolithAudioOptionalAvailabilityTest, "Monolith.Audio.OptionalAvailability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithAudioOptionalAvailabilityTest::RunTest(const FString& Parameters)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
	const auto Empty = MakeShared<FJsonObject>();
	const auto BaseParams = MakeShared<FJsonObject>();
	BaseParams->SetStringField(TEXT("type"), TEXT("SoundCue"));
	BaseParams->SetStringField(TEXT("path_filter"), TEXT("/Game/Tests/Monolith/Audio/OptionalAvailability"));
	const auto Base = Registry.ExecuteAction(TEXT("audio"), TEXT("list_audio_assets"), BaseParams);
	if (!TestTrue(TEXT("SoundCue queries remain usable"), Base.bSuccess && Base.Result.IsValid())) return false;
	TestTrue(TEXT("Base audio returns its asset list"), Base.Result->HasField(TEXT("assets")));
	const auto List = Registry.ExecuteAction(TEXT("audio"), TEXT("list_metasounds"), Empty);
#if WITH_METASOUND
	TestTrue(TEXT("Compiled MetaSound introspection runs"), List.bSuccess && List.Result.IsValid());
	const auto Nodes = Registry.ExecuteAction(TEXT("audio"), TEXT("list_available_metasound_nodes"), Empty);
	TestTrue(TEXT("Compiled MetaSound graph discovery runs"), Nodes.bSuccess && Nodes.Result.IsValid());
#else
	auto CheckUnavailable = [this](const FMonolithActionResult& Result)
	{
		TestFalse(TEXT("Absent MetaSound rejects execution"), Result.bSuccess);
		TestEqual(TEXT("MetaSound dependency code"), Result.ErrorCode, FMonolithJsonUtils::ErrOptionalDepUnavailable);
		if (!TestTrue(TEXT("MetaSound error has data"), Result.ErrorData.IsValid())) return;
		const auto Data = Result.ErrorData->AsObject();
		if (!TestTrue(TEXT("MetaSound error data is an object"), Data.IsValid())) return;
		TestEqual(TEXT("MetaSound error class"), Data->GetStringField(TEXT("class")), FString(TEXT("optional_dep_unavailable")));
		TestEqual(TEXT("MetaSound dependency named"), Data->GetStringField(TEXT("dep_name")), FString(TEXT("Metasound")));
		TestFalse(TEXT("MetaSound absence causes no mutation"), Data->GetBoolField(TEXT("executed")));
	};
	CheckUnavailable(List);
	const auto GraphParams = MakeShared<FJsonObject>();
	GraphParams->SetStringField(TEXT("asset_path"), TEXT("/Game/Tests/Monolith/Audio/OptionalAvailability/MS_Unavailable"));
	CheckUnavailable(Registry.ExecuteAction(TEXT("audio"), TEXT("get_metasound_graph"), GraphParams));
#endif
	const auto Discover = Registry.ExecuteAction(TEXT("monolith"), TEXT("discover"), Empty);
	if (!TestTrue(TEXT("Top-level discovery succeeds"), Discover.bSuccess && Discover.Result.IsValid())) return false;
	for (const auto& Value : Discover.Result->GetArrayField(TEXT("namespaces")))
	{
		const auto Ns = Value->AsObject();
		if (Ns->GetStringField(TEXT("namespace")) != TEXT("audio")) continue;
		TestTrue(TEXT("Mixed audio namespace remains available"), Ns->GetObjectField(TEXT("availability"))->GetBoolField(TEXT("available")));
		for (const auto& DepValue : Ns->GetArrayField(TEXT("optional_dependencies")))
		{
			const auto Dep = DepValue->AsObject();
			if (Dep->GetStringField(TEXT("required_plugin")) != TEXT("Metasound")) continue;
			TestEqual(TEXT("MetaSound discovery matches compiled implementation"), Dep->GetBoolField(TEXT("available")), WITH_METASOUND != 0);
			TestEqual(TEXT("MetaSound availability reason"), Dep->GetStringField(TEXT("reason")), FString(WITH_METASOUND ? TEXT("") : TEXT("not_compiled")));
			return true;
		}
		AddError(TEXT("Audio discovery omitted the MetaSound optional dependency"));
		return false;
	}
	AddError(TEXT("Audio namespace missing from top-level discovery"));
	return false;
}
#endif
