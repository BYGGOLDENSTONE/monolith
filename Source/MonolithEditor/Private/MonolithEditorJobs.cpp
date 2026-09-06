#include "MonolithEditorJobs.h"
#include "MonolithParamSchema.h"
#include "MonolithJsonUtils.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"

namespace
{
	struct FEncoderJob
	{
		FString Id, State = TEXT("running"), Output, Encoder;
		FProcHandle Process;
		double Started = 0;
		int32 ExitCode = -1;
		TSharedPtr<FJsonObject> Capture;
		TArray<FString> TemporaryFiles;
	};
	TMap<FString, TSharedPtr<FEncoderJob>> Jobs;
	FTSTicker::FDelegateHandle TickHandle;

	void Finish(FEncoderJob& Job, const TCHAR* State, bool bTerminate)
	{
		if (Job.Process.IsValid())
		{
			if (bTerminate) FPlatformProcess::TerminateProc(Job.Process, true);
			FPlatformProcess::GetProcReturnCode(Job.Process, &Job.ExitCode);
			FPlatformProcess::CloseProc(Job.Process);
			Job.Process.Reset();
		}
		Job.State = State;
		for (const FString& File : Job.TemporaryFiles) IFileManager::Get().Delete(*File);
		Job.TemporaryFiles.Empty();
	}

	bool TickJobs(float)
	{
		for (auto& Pair : Jobs)
		{
			FEncoderJob& Job = *Pair.Value;
			if (Job.State != TEXT("running")) continue;
			if (!FPlatformProcess::IsProcRunning(Job.Process))
			{
				FPlatformProcess::GetProcReturnCode(Job.Process, &Job.ExitCode);
				const bool bOK = Job.ExitCode == 0 && IFileManager::Get().FileSize(*Job.Output) > 0;
				Finish(Job, bOK ? TEXT("completed") : TEXT("failed"), false);
			}
			else if (FPlatformTime::Seconds() - Job.Started > 300.0)
				Finish(Job, TEXT("timed_out"), true);
		}
		return true;
	}

	TSharedPtr<FJsonObject> JobJson(const FEncoderJob& Job, bool bIncludeResult)
	{
		auto Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("job_id"), Job.Id);
		Json->SetStringField(TEXT("state"), Job.State);
		Json->SetStringField(TEXT("encoder"), Job.Encoder);
		Json->SetBoolField(TEXT("done"), Job.State != TEXT("running"));
		Json->SetNumberField(TEXT("exit_code"), Job.ExitCode);
		Json->SetStringField(TEXT("output_path"), Job.Output);
		if (Job.State == TEXT("failed"))
			Json->SetStringField(TEXT("error"), TEXT("Encoder exited unsuccessfully or produced no GIF. Check encoder installation/dependencies; captured PNGs remain available."));
		if (bIncludeResult)
		{
			Json->SetObjectField(TEXT("capture"), Job.Capture);
			if (Job.State == TEXT("completed")) Json->SetStringField(TEXT("gif_path"), Job.Output);
		}
		return Json;
	}

	TSharedPtr<FEncoderJob> FindJob(const TSharedPtr<FJsonObject>& Params)
	{
		FString Id;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("job_id"), Id)) return nullptr;
		const TSharedPtr<FEncoderJob>* Found = Jobs.Find(Id);
		return Found ? *Found : nullptr;
	}

	FMonolithActionResult TransactionAction(bool bRedo)
	{
		if (!GEditor || !GEditor->Trans) return FMonolithActionResult::Error(TEXT("Editor transaction buffer unavailable"));
		if (GEditor->PlayWorld || GEditor->Trans->IsActive()) return FMonolithActionResult::Error(TEXT("Undo/redo is unavailable during PIE or an active transaction"));
		FText Reason;
		if (!(bRedo ? GEditor->Trans->CanRedo(&Reason) : GEditor->Trans->CanUndo(&Reason)))
			return FMonolithActionResult::Error(Reason.IsEmpty() ? TEXT("No transaction available") : Reason.ToString());
		const bool bOK = bRedo ? GEditor->RedoTransaction() : GEditor->UndoTransaction();
		if (!bOK) return FMonolithActionResult::Error(TEXT("Editor could not apply the transaction"));
		auto Json = MakeShared<FJsonObject>();
		Json->SetBoolField(TEXT("success"), true);
		Json->SetBoolField(TEXT("can_undo"), GEditor->Trans->CanUndo());
		Json->SetBoolField(TEXT("can_redo"), GEditor->Trans->CanRedo());
		return FMonolithActionResult::Success(Json);
	}
}

void FMonolithEditorJobs::RegisterActions(FMonolithToolRegistry& Registry)
{
	const auto Schema = FParamSchemaBuilder().Required(TEXT("job_id"), TEXT("string"), TEXT("Encoder job ID returned by capture_system_gif")).Build();
	Registry.RegisterAction(TEXT("editor"), TEXT("get_job_status"), TEXT("Poll a GIF encoder job without waiting. Jobs expire when the editor closes; retain at most 64."), FMonolithActionHandler::CreateStatic(&Status), Schema);
	Registry.RegisterAction(TEXT("editor"), TEXT("cancel_job"), TEXT("Cancel a running GIF encoder process; captured PNGs remain on disk. Repeating cancellation is safe."), FMonolithActionHandler::CreateStatic(&Cancel), Schema);
	Registry.RegisterAction(TEXT("editor"), TEXT("get_job_result"), TEXT("Read a terminal encoder job result, including capture frame paths. Returns an error while running."), FMonolithActionHandler::CreateStatic(&Result), Schema);
	Registry.RegisterAction(TEXT("editor"), TEXT("undo"), TEXT("Undo the latest editor transaction (global history, including manual edits). Refuses during PIE or an active transaction."),
		FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>&) { return TransactionAction(false); }), MakeShared<FJsonObject>());
	Registry.RegisterAction(TEXT("editor"), TEXT("redo"), TEXT("Redo the latest undone editor transaction (global history). Refuses during PIE or an active transaction."),
		FMonolithActionHandler::CreateLambda([](const TSharedPtr<FJsonObject>&) { return TransactionAction(true); }), MakeShared<FJsonObject>());
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickJobs), 0.1f);
}

void FMonolithEditorJobs::Shutdown()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	TickHandle.Reset();
	for (auto& Pair : Jobs) if (Pair.Value->State == TEXT("running")) Finish(*Pair.Value, TEXT("cancelled"), true);
	Jobs.Empty();
}

FMonolithActionResult FMonolithEditorJobs::StartEncoder(const FString& Encoder, const FString& OutputDir,
	const TArray<FString>& Frames, int32 FPS, int32 Resolution, const TSharedPtr<FJsonObject>& CaptureResult)
{
	if (Encoder != TEXT("ffmpeg") && Encoder != TEXT("python")) return FMonolithActionResult::Error(TEXT("Encoder must be ffmpeg or python"));
	int32 Running = 0;
	for (const auto& Pair : Jobs) Running += Pair.Value->State == TEXT("running") ? 1 : 0;
	if (Running >= 4) return FMonolithActionResult::Error(TEXT("Four encoder jobs already running; poll or cancel one first"));
	if (Frames.IsEmpty()) return FMonolithActionResult::Error(TEXT("No captured frames to encode"));
	// Keep completed records bounded, evicting the oldest terminal record only.
	if (Jobs.Num() >= 64)
	{
		TSharedPtr<FEncoderJob> Oldest;
		for (const auto& Pair : Jobs)
			if (Pair.Value->State != TEXT("running") && (!Oldest || Pair.Value->Started < Oldest->Started)) Oldest = Pair.Value;
		if (Oldest) Jobs.Remove(Oldest->Id);
	}
	auto Job = MakeShared<FEncoderJob>();
	Job->Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	Job->Started = FPlatformTime::Seconds();
	Job->Encoder = Encoder;
	Job->Capture = MakeShared<FJsonObject>();
	Job->Capture->Values = CaptureResult->Values;
	// Unique output also prevents concurrent requests from overwriting each other's GIF.
	Job->Output = FPaths::ConvertRelativePathToFull(OutputDir / (TEXT("output_") + Job->Id + TEXT(".gif")));
	FString Args;
	if (Encoder == TEXT("ffmpeg"))
	{
		// Capture filenames include timestamps; the old %04d pattern never matched.
		// A concat manifest consumes exactly the returned filenames in order.
		FString List = TEXT("ffconcat version 1.0\n");
		for (const FString& Frame : Frames)
		{
			FString Escaped = FPaths::ConvertRelativePathToFull(Frame);
			Escaped.ReplaceInline(TEXT("\\"), TEXT("/"));
			Escaped.ReplaceInline(TEXT("'"), TEXT("'\\''"));
			List += FString::Printf(TEXT("file '%s'\nduration %.9f\n"), *Escaped, 1.0 / FPS);
		}
		const FString ManifestPath = FPaths::ConvertRelativePathToFull(OutputDir / (TEXT(".monolith_encode_") + Job->Id + TEXT(".ffconcat")));
		Job->TemporaryFiles.Add(ManifestPath);
		if (!FFileHelper::SaveStringToFile(List, *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Finish(*Job, TEXT("failed"), false);
			return FMonolithActionResult::Error(TEXT("Could not write ffmpeg frame manifest"));
		}
		Args = FString::Printf(TEXT("-y -f concat -safe 0 -i \"%s\" -vf \"scale=%d:-1:flags=lanczos\" -r %d -frames:v %d -loop 0 \"%s\""),
			*ManifestPath, Resolution, FPS, Frames.Num(), *Job->Output);
	}
	else
	{
		// Filenames are data, never interpolated Python source (apostrophes and Unicode are valid).
		auto Manifest = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& Frame : Frames) Values.Add(MakeShared<FJsonValueString>(FPaths::ConvertRelativePathToFull(Frame)));
		Manifest->SetArrayField(TEXT("frames"), Values);
		Manifest->SetStringField(TEXT("output"), Job->Output);
		Manifest->SetNumberField(TEXT("duration_ms"), FMath::Max(10, FMath::RoundToInt(1000.0 / FPS)));
		const FString Base = FPaths::ConvertRelativePathToFull(OutputDir / (TEXT(".monolith_encode_") + Job->Id));
		const FString ManifestPath = Base + TEXT(".json"), ScriptPath = Base + TEXT(".py");
		Job->TemporaryFiles = { ManifestPath, ScriptPath };
		const FString Script = TEXT("import json, sys\nfrom PIL import Image\nwith open(sys.argv[1], encoding='utf-8-sig') as f: m=json.load(f)\nframes=[]\nfor p in m['frames']:\n    with Image.open(p) as im: frames.append(im.convert('RGBA'))\nframes[0].save(m['output'], format='GIF', save_all=True, append_images=frames[1:], duration=m['duration_ms'], loop=0)\n");
		if (!FFileHelper::SaveStringToFile(FMonolithJsonUtils::Serialize(Manifest), *ManifestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
			|| !FFileHelper::SaveStringToFile(Script, *ScriptPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Finish(*Job, TEXT("failed"), false);
			return FMonolithActionResult::Error(TEXT("Could not write encoder manifest/script"));
		}
		Args = FString::Printf(TEXT("\"%s\" \"%s\""), *ScriptPath, *ManifestPath);
	}
	Job->Process = FPlatformProcess::CreateProc(*Encoder, *Args, false, true, true, nullptr, 0, nullptr, nullptr);
	if (!Job->Process.IsValid())
	{
		Finish(*Job, TEXT("failed"), false);
		return FMonolithActionResult::Error(FString::Printf(TEXT("Could not launch %s; ensure executable is in PATH. Captured PNGs are retained."), *Encoder));
	}
	Jobs.Add(Job->Id, Job);
	return FMonolithActionResult::Success(JobJson(*Job, true));
}

FMonolithActionResult FMonolithEditorJobs::Status(const TSharedPtr<FJsonObject>& Params)
{
	TickJobs(0);
	auto Job = FindJob(Params);
	return Job ? FMonolithActionResult::Success(JobJson(*Job, false)) : FMonolithActionResult::Error(TEXT("Unknown or expired job_id"));
}
FMonolithActionResult FMonolithEditorJobs::Cancel(const TSharedPtr<FJsonObject>& Params)
{
	TickJobs(0);
	auto Job = FindJob(Params);
	if (!Job) return FMonolithActionResult::Error(TEXT("Unknown or expired job_id"));
	if (Job->State == TEXT("running")) Finish(*Job, TEXT("cancelled"), true);
	return FMonolithActionResult::Success(JobJson(*Job, false));
}
FMonolithActionResult FMonolithEditorJobs::Result(const TSharedPtr<FJsonObject>& Params)
{
	TickJobs(0);
	auto Job = FindJob(Params);
	if (!Job) return FMonolithActionResult::Error(TEXT("Unknown or expired job_id"));
	if (Job->State == TEXT("running")) return FMonolithActionResult::Error(TEXT("Job is still running; poll get_job_status"));
	return FMonolithActionResult::Success(JobJson(*Job, true));
}
