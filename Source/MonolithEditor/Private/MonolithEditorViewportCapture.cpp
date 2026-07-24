// Copyright tumourlove. All Rights Reserved.

// =============================================================================
// MonolithEditorViewportCapture.cpp
//
// editor::capture_viewport — a screenshot of the LIVE editor level viewport, i.e.
// the open map exactly as the user is currently looking at it.
//
// WHY THIS EXISTS (ground truth established 2026-07-24, Faz 2 slice 3):
// the pre-existing capture_* family never touched the editor's own viewport.
//   editor::capture_scene_preview / capture_material_grid / capture_with_overlay
//   / capture_sequence_frames / capture_anim_frames   -> isolated FPreviewScene,
//        one asset, synthetic camera + lighting. Nothing to do with the open map.
//   editor::capture_pie_movement_clip                 -> the PIE viewport, and
//        only while a PIE session is running.
//   mesh::capture_floor_plan / capture_building_views -> the open map, but via a
//        transient USceneCaptureComponent2D at a computed camera, keyed off the
//        spatial registry (building_id). Scene capture bypasses editor show
//        flags, selection, gizmos, grid and the user's own camera.
//   mesh::sample_light_levels ("scene capture w/ Lumen GI")               -> a
//        1x1-ish luminance probe, not an image.
//   editor::get_viewport_info                         -> the closest relative:
//        it reads the level viewport's camera/resolution but produces no pixels.
// So "what the user currently sees" had no action at all. This is it.
//
// Approach mirrors the engine's own FEditorViewportClient::TakeScreenshot
// (EditorViewportClient.cpp:6414): Invalidate -> FViewport::Draw -> ReadPixels
// over GetRenderTargetTextureSizeXY, then force alpha opaque. Output convention
// matches the neighbouring capture actions exactly: an image file on disk plus
// {output_file, resolution{width,height}, capture_time_ms} in the JSON result —
// the MCP envelope carries no image payload, so a path is what callers get.
//
// NO JOB. The whole thing is one Draw + one blocking readback on the game thread
// (single-digit to low-tens of milliseconds at 4K). FMonolithJobManager exists for
// work that would outlive the proxy's timeout; a single frame grab does not.
// =============================================================================

#include "MonolithEditorViewportCapture.h"

#include "MonolithEditorActions.h"
#include "MonolithJsonUtils.h"
#include "MonolithSettings.h"
#include "MonolithToolRegistry.h" // FMonolithActionResult

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "UnrealClient.h"

#include "Editor.h"                 // GEditor, GCurrentLevelEditingViewportClient
#include "Editor/UnrealEdTypes.h"   // ELevelViewportType
#include "LevelEditorViewport.h"    // FLevelEditorViewportClient

// ============================================================================
// Pure helpers (declared in MonolithEditorViewportCapture.h)
// ============================================================================

namespace MonolithViewportCapture
{
	const TArray<FString>& SupportedExtensions()
	{
		// The set FImageUtils::SaveImageAutoFormat can actually encode. This is a
		// property of the engine encoder, not a user-tunable, so it lives in code.
		static const TArray<FString> Extensions = {
			TEXT("png"), TEXT("jpg"), TEXT("jpeg"), TEXT("bmp"), TEXT("exr"), TEXT("hdr")
		};
		return Extensions;
	}

	FString SupportedExtensionList()
	{
		return FString::Join(SupportedExtensions(), TEXT(", "));
	}

	bool IsSupportedExtension(const FString& Ext)
	{
		return SupportedExtensions().Contains(Ext.ToLower());
	}

	FString BuildDefaultOutputPath(const FString& Stamp)
	{
		FString Dir;
		if (const UMonolithSettings* Settings = UMonolithSettings::Get())
		{
			Dir = Settings->ViewportCaptureDirectory;
		}
		if (Dir.IsEmpty())
		{
			// Settings object unavailable (very early startup / cooked-out CDO).
			// Fall back to the same bucket the CDO declares so behaviour matches.
			Dir = TEXT("Saved/Screenshots/Monolith/Viewport");
		}
		// Fully-qualified: FPaths::ProjectDir() is itself relative to the engine
		// binaries dir, and the path is handed to a client that has no idea what
		// the editor's CWD is.
		return FPaths::ConvertRelativePathToFull(
			FPaths::ProjectDir() / Dir / FString::Printf(TEXT("%s.png"), *Stamp));
	}

	bool PixelsAreUniform(const TArray<FColor>& Pixels)
	{
		if (Pixels.Num() == 0)
		{
			return true; // no pixels => certainly not a real render
		}
		const FColor First = Pixels[0];
		const int32 Stride = FMath::Max(1, Pixels.Num() / 4096); // sample up to ~4k pixels
		for (int32 i = 0; i < Pixels.Num(); i += Stride)
		{
			const FColor& Px = Pixels[i];
			if (Px.R != First.R || Px.G != First.G || Px.B != First.B)
			{
				return false;
			}
		}
		return true;
	}

	FIntPoint FitToMaxDimension(FIntPoint Size, int32 MaxDimension)
	{
		if (MaxDimension <= 0 || Size.X <= 0 || Size.Y <= 0)
		{
			return Size;
		}
		const int32 Longest = FMath::Max(Size.X, Size.Y);
		if (Longest <= MaxDimension)
		{
			return Size;
		}
		const double Scale = (double)MaxDimension / (double)Longest;
		return FIntPoint(
			FMath::Max(1, FMath::RoundToInt(Size.X * Scale)),
			FMath::Max(1, FMath::RoundToInt(Size.Y * Scale)));
	}

	namespace
	{
		/** Read an optional integer field, rejecting non-numeric JSON with a typed message. */
		bool ReadOptionalInt(
			const TSharedPtr<FJsonObject>& Params, const TCHAR* Field,
			int32& OutValue, bool& bOutPresent, FString& OutError)
		{
			bOutPresent = false;
			if (!Params->HasField(Field))
			{
				return true;
			}
			double Number = 0.0;
			if (!Params->TryGetNumberField(Field, Number))
			{
				OutError = FString::Printf(TEXT("'%s' must be a number."), Field);
				return false;
			}
			OutValue = (int32)Number;
			bOutPresent = true;
			return true;
		}

		/**
		 * Read an optional bool field, rejecting non-boolean JSON with a typed message.
		 * Uses the engine's TryGetBoolField, so the same coercions every other Monolith
		 * action accepts apply here (FJsonValueNumber::TryGetBool maps 1/0, and a
		 * "true"/"false" string parses) — only a value with no boolean reading at all
		 * (array / object / arbitrary string) is refused.
		 */
		bool ReadOptionalBool(
			const TSharedPtr<FJsonObject>& Params, const TCHAR* Field,
			bool& OutValue, FString& OutError)
		{
			if (!Params->HasField(Field))
			{
				return true;
			}
			bool Value = false;
			if (!Params->TryGetBoolField(Field, Value))
			{
				OutError = FString::Printf(TEXT("'%s' must be a boolean."), Field);
				return false;
			}
			OutValue = Value;
			return true;
		}
	}

	bool ParseRequest(const TSharedPtr<FJsonObject>& Params, FRequest& OutRequest, FString& OutError)
	{
		OutError.Reset();
		OutRequest = FRequest();

		const TSharedPtr<FJsonObject> Safe = Params.IsValid() ? Params : MakeShared<FJsonObject>();

		// --- Data-driven defaults (UMonolithSettings, "Capture" category) ---
		if (const UMonolithSettings* Settings = UMonolithSettings::Get())
		{
			OutRequest.MaxDimension = Settings->ViewportCaptureMaxDimension;
			OutRequest.Quality = Settings->ViewportCaptureQuality;
		}

		// --- viewport_index ---
		{
			int32 Index = 0;
			bool bPresent = false;
			if (!ReadOptionalInt(Safe, TEXT("viewport_index"), Index, bPresent, OutError))
			{
				return false;
			}
			if (bPresent)
			{
				if (Index < 0)
				{
					OutError = TEXT("'viewport_index' must be >= 0. Omit it to capture the active level viewport.");
					return false;
				}
				OutRequest.ViewportIndex = Index;
			}
		}

		// --- max_dimension ---
		{
			int32 MaxDim = OutRequest.MaxDimension;
			bool bPresent = false;
			if (!ReadOptionalInt(Safe, TEXT("max_dimension"), MaxDim, bPresent, OutError))
			{
				return false;
			}
			if (bPresent)
			{
				if (MaxDim < 0)
				{
					OutError = TEXT("'max_dimension' must be >= 0 (0 = capture at the viewport's native size, no downscale).");
					return false;
				}
				OutRequest.MaxDimension = MaxDim;
			}
		}

		// --- quality ---
		{
			int32 Quality = OutRequest.Quality;
			bool bPresent = false;
			if (!ReadOptionalInt(Safe, TEXT("quality"), Quality, bPresent, OutError))
			{
				return false;
			}
			if (bPresent)
			{
				if (Quality < 0 || Quality > 100)
				{
					OutError = TEXT("'quality' must be between 0 and 100 (0 = encoder default). Only lossy formats (jpg/jpeg) use it.");
					return false;
				}
				OutRequest.Quality = Quality;
			}
		}

		// --- redraw / allow_uniform / restore_camera ---
		if (!ReadOptionalBool(Safe, TEXT("redraw"), OutRequest.bRedraw, OutError)) { return false; }
		if (!ReadOptionalBool(Safe, TEXT("allow_uniform"), OutRequest.bAllowUniform, OutError)) { return false; }
		if (!ReadOptionalBool(Safe, TEXT("restore_camera"), OutRequest.bRestoreCamera, OutError)) { return false; }

		// --- camera override ---
		if (Safe->HasField(TEXT("camera")))
		{
			const TSharedPtr<FJsonObject>* CameraObj = nullptr;
			TSharedPtr<FJsonObject> ParsedCamera;

			// Accept both a real object and a string-serialized object (same client
			// quirk the sibling capture actions tolerate).
			if (!Safe->TryGetObjectField(TEXT("camera"), CameraObj))
			{
				FString CameraStr;
				if (Safe->TryGetStringField(TEXT("camera"), CameraStr) && !CameraStr.IsEmpty())
				{
					ParsedCamera = FMonolithJsonUtils::Parse(CameraStr);
					CameraObj = &ParsedCamera;
				}
			}

			if (!CameraObj || !(*CameraObj).IsValid())
			{
				OutError = TEXT("'camera' must be an object like {location:[x,y,z], rotation:[pitch,yaw,roll], fov:90}.");
				return false;
			}

			const TArray<TSharedPtr<FJsonValue>>* Loc = nullptr;
			if (!(*CameraObj)->TryGetArrayField(TEXT("location"), Loc) || !Loc || Loc->Num() < 3)
			{
				OutError = TEXT("'camera.location' is required and must be [x, y, z].");
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Rot = nullptr;
			if (!(*CameraObj)->TryGetArrayField(TEXT("rotation"), Rot) || !Rot || Rot->Num() < 3)
			{
				OutError = TEXT("'camera.rotation' is required and must be [pitch, yaw, roll].");
				return false;
			}

			OutRequest.bHasCamera = true;
			OutRequest.CameraLocation = FVector(
				(*Loc)[0]->AsNumber(), (*Loc)[1]->AsNumber(), (*Loc)[2]->AsNumber());
			OutRequest.CameraRotation = FRotator(
				(*Rot)[0]->AsNumber(), (*Rot)[1]->AsNumber(), (*Rot)[2]->AsNumber());

			double Fov = 0.0;
			if ((*CameraObj)->TryGetNumberField(TEXT("fov"), Fov))
			{
				if (Fov <= 0.0 || Fov >= 180.0)
				{
					OutError = TEXT("'camera.fov' must be greater than 0 and less than 180.");
					return false;
				}
				OutRequest.bHasFov = true;
				OutRequest.CameraFov = (float)Fov;
			}
		}

		// --- output_path ---
		{
			FString OutputPath;
			if (Safe->TryGetStringField(TEXT("output_path"), OutputPath) && !OutputPath.TrimStartAndEnd().IsEmpty())
			{
				OutputPath = OutputPath.TrimStartAndEnd();
				if (FPaths::IsRelative(OutputPath))
				{
					OutputPath = FPaths::ProjectDir() / OutputPath;
				}
				const FString Ext = FPaths::GetExtension(OutputPath);
				if (Ext.IsEmpty())
				{
					OutError = FString::Printf(
						TEXT("'output_path' needs a file extension — the encoder is chosen from it. Supported: %s."),
						*SupportedExtensionList());
					return false;
				}
				if (!IsSupportedExtension(Ext))
				{
					OutError = FString::Printf(
						TEXT("Unsupported output extension '.%s'. Supported: %s."),
						*Ext, *SupportedExtensionList());
					return false;
				}
				OutRequest.OutputPath = FPaths::ConvertRelativePathToFull(OutputPath);
			}
			else
			{
				OutRequest.OutputPath = BuildDefaultOutputPath(
					FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
			}
		}

		return true;
	}
}

// ============================================================================
// Local (engine-bound) helpers
// ============================================================================

namespace
{
	/** Stringify ELevelViewportType so the caller knows what it actually got. */
	const TCHAR* ViewportTypeToString(ELevelViewportType Type)
	{
		switch (Type)
		{
		case LVT_Perspective:      return TEXT("perspective");
		case LVT_OrthoXY:          return TEXT("ortho_top");
		case LVT_OrthoNegativeXY:  return TEXT("ortho_bottom");
		case LVT_OrthoXZ:          return TEXT("ortho_right");
		case LVT_OrthoNegativeXZ:  return TEXT("ortho_left");
		case LVT_OrthoYZ:          return TEXT("ortho_back");
		case LVT_OrthoNegativeYZ:  return TEXT("ortho_front");
		case LVT_OrthoFreelook:    return TEXT("ortho_freelook");
		default:                   return TEXT("unknown");
		}
	}

	TArray<TSharedPtr<FJsonValue>> VectorToJson(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Add(MakeShared<FJsonValueNumber>(V.X));
		Out.Add(MakeShared<FJsonValueNumber>(V.Y));
		Out.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> RotatorToJson(const FRotator& R)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Add(MakeShared<FJsonValueNumber>(R.Pitch));
		Out.Add(MakeShared<FJsonValueNumber>(R.Yaw));
		Out.Add(MakeShared<FJsonValueNumber>(R.Roll));
		return Out;
	}
}

// ============================================================================
// editor::capture_viewport
// ============================================================================

FMonolithActionResult FMonolithEditorActions::HandleCaptureViewport(
	const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithViewportCapture;

	// --- 1. Parameters first, environment second ---------------------------
	// A malformed call is reported as malformed even in a process that could
	// never render, so the caller fixes the real problem.
	FRequest Request;
	FString ParseError;
	if (!ParseRequest(Params, Request, ParseError))
	{
		return FMonolithActionResult::Error(ParseError, -32602);
	}

	// --- 2. Environment gates ---------------------------------------------
	if (!GEditor)
	{
		return FMonolithActionResult::Error(
			TEXT("capture_viewport requires an editor context (GEditor is null)."), -32603);
	}

	// -nullrhi / commandlet / dedicated server: the level viewport is never
	// rendered into, so a readback would hand back an uninitialised buffer.
	// Fail honestly instead of writing a blank image. Same -32603 convention the
	// widget branch of capture_scene_preview uses for "no rendering path".
	if (!FApp::CanEverRender())
	{
		return FMonolithActionResult::Error(
			TEXT("Cannot capture the editor viewport: this process has no rendering path "
				 "(-nullrhi / -nohumanverify commandlet / server). The level viewport is never "
				 "rendered in this mode, so any image would be blank. Run the editor with "
				 "rendering enabled and retry."),
			-32603);
	}

	const TArray<FLevelEditorViewportClient*>& Clients = GEditor->GetLevelViewportClients();
	if (Clients.Num() == 0)
	{
		return FMonolithActionResult::Error(
			TEXT("No level editor viewport is open. capture_viewport photographs the level "
				 "viewport of the open map; open one (Window > Viewports > Viewport 1) and retry. "
				 "For an asset rendered in isolation use editor::capture_scene_preview instead."),
			-32603);
	}

	// --- 3. Resolve which viewport -----------------------------------------
	int32 Index = Request.ViewportIndex;
	bool bUsedActive = false;
	if (Index == INDEX_NONE)
	{
		Index = Clients.IndexOfByKey(GCurrentLevelEditingViewportClient);
		if (Index == INDEX_NONE)
		{
			Index = 0; // no viewport has focus yet — first one is the editor's own default
		}
		else
		{
			bUsedActive = true;
		}
	}
	else if (!Clients.IsValidIndex(Index))
	{
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("'viewport_index' %d is out of range — %d level viewport(s) are open (valid 0..%d). "
					 "Omit viewport_index to capture the active one."),
				Index, Clients.Num(), Clients.Num() - 1),
			-32602);
	}

	FLevelEditorViewportClient* ViewportClient = Clients[Index];
	if (!ViewportClient)
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Level viewport %d is registered but null."), Index), -32603);
	}

	FViewport* Viewport = ViewportClient->Viewport;
	if (!Viewport)
	{
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Level viewport %d has no render surface yet (FViewport is null) — it is registered "
					 "but has not been realised on screen. Bring the viewport tab to the front and retry."),
				Index),
			-32603);
	}

	const FIntPoint Size = Viewport->GetRenderTargetTextureSizeXY();
	if (Size.X <= 0 || Size.Y <= 0)
	{
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Level viewport %d reports a zero-sized render target (%dx%d) — it is collapsed or "
					 "hidden. Make the viewport visible and retry."),
				Index, Size.X, Size.Y),
			-32603);
	}

	const double StartTime = FPlatformTime::Seconds();

	// --- 4. Optional camera override (restored afterwards) ------------------
	const FVector PrevLocation = ViewportClient->GetViewLocation();
	const FRotator PrevRotation = ViewportClient->GetViewRotation();
	const float PrevFov = ViewportClient->ViewFOV;
	bool bCameraMoved = false;

	if (Request.bHasCamera)
	{
		ViewportClient->SetViewLocation(Request.CameraLocation);
		ViewportClient->SetViewRotation(Request.CameraRotation);
		if (Request.bHasFov)
		{
			ViewportClient->ViewFOV = Request.CameraFov;
		}
		bCameraMoved = true;
	}

	// --- 5. Draw + read back ------------------------------------------------
	// Mirrors FEditorViewportClient::TakeScreenshot: invalidate, redraw so the
	// buffer is this viewport's (viewports share a frame buffer), flush the
	// render thread, then read.
	if (Request.bRedraw || bCameraMoved)
	{
		ViewportClient->Invalidate(/*bInvalidateChildViews=*/false, /*bInvalidateHitProxies=*/true);
		Viewport->Draw(/*bShouldPresent=*/true);
	}
	FlushRenderingCommands();

	const FIntRect CaptureRect(0, 0, Size.X, Size.Y);
	TArray<FColor> Pixels;
	const bool bReadOk = Viewport->ReadPixels(
		Pixels, FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX), CaptureRect);

	// Put the camera back before any early return so a failed capture never
	// leaves the user's viewport somewhere else.
	if (bCameraMoved && Request.bRestoreCamera)
	{
		ViewportClient->SetViewLocation(PrevLocation);
		ViewportClient->SetViewRotation(PrevRotation);
		ViewportClient->ViewFOV = PrevFov;
		ViewportClient->Invalidate(/*bInvalidateChildViews=*/false, /*bInvalidateHitProxies=*/true);
	}

	if (!bReadOk || Pixels.Num() < Size.X * Size.Y)
	{
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Reading back level viewport %d failed (read_ok=%d, pixels=%d, expected=%d). "
					 "The viewport surface is unavailable this frame; retry, or check the log for RHI errors."),
				Index, bReadOk ? 1 : 0, Pixels.Num(), Size.X * Size.Y),
			-32603);
	}

	// --- 6. Honesty gate: did anything actually render? ---------------------
	const bool bUniform = PixelsAreUniform(Pixels);
	if (bUniform && !Request.bAllowUniform)
	{
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Level viewport %d read back as a single flat colour (%dx%d, every sampled pixel "
					 "identical) — the viewport was almost certainly never rendered (hidden editor window, "
					 "no RHI, or a viewport that has not drawn yet). Nothing was written. If the view really "
					 "is one flat colour on purpose, pass allow_uniform=true."),
				Index, Size.X, Size.Y),
			-32603);
	}

	for (FColor& Px : Pixels)
	{
		Px.A = 255; // viewport reads carry scene-depth alpha noise
	}

	// --- 7. Encode + write --------------------------------------------------
	FImage Image;
	Image.Init(Size.X, Size.Y, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
	FMemory::Memcpy(Image.RawData.GetData(), Pixels.GetData(), (int64)Size.X * Size.Y * sizeof(FColor));

	const FIntPoint TargetSize = FitToMaxDimension(Size, Request.MaxDimension);
	const bool bDownscaled = (TargetSize != Size);
	if (bDownscaled)
	{
		FImage Resized;
		Image.ResizeTo(Resized, TargetSize.X, TargetSize.Y, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		Image = MoveTemp(Resized);
	}

	const FString OutDir = FPaths::GetPath(Request.OutputPath);
	if (!OutDir.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*OutDir, /*Tree=*/true);
	}

	// SaveImageByExtension, NOT SaveImageAutoFormat. AutoFormat ignores the caller's
	// extension whenever it disagrees with GetDefaultOutputFormat(BGRA8) — it silently
	// rewrites "shot.jpg" to "shot.png" (ImageUtils.cpp:79-87), which would make the
	// output_file we report a lie. ByExtension honours the extension we validated.
	if (!FImageUtils::SaveImageByExtension(*Request.OutputPath, Image, Request.Quality))
	{
		return FMonolithActionResult::Error(
			FString::Printf(
				TEXT("Captured level viewport %d but failed to write '%s'. Check the path exists, is "
					 "writable, and that the disk is not full."),
				Index, *Request.OutputPath),
			-32603);
	}

	// --- 8. Result (same shape as the sibling capture actions) --------------
	const double ElapsedMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("success"), true);
	Result->SetStringField(TEXT("output_file"), Request.OutputPath);

	TSharedPtr<FJsonObject> ResObj = MakeShared<FJsonObject>();
	ResObj->SetNumberField(TEXT("width"), TargetSize.X);
	ResObj->SetNumberField(TEXT("height"), TargetSize.Y);
	Result->SetObjectField(TEXT("resolution"), ResObj);

	TSharedPtr<FJsonObject> SrcResObj = MakeShared<FJsonObject>();
	SrcResObj->SetNumberField(TEXT("width"), Size.X);
	SrcResObj->SetNumberField(TEXT("height"), Size.Y);
	Result->SetObjectField(TEXT("viewport_resolution"), SrcResObj);
	Result->SetBoolField(TEXT("downscaled"), bDownscaled);

	Result->SetNumberField(TEXT("viewport_index"), Index);
	Result->SetNumberField(TEXT("viewport_count"), Clients.Num());
	Result->SetBoolField(TEXT("was_active_viewport"), bUsedActive);
	Result->SetStringField(TEXT("viewport_type"), ViewportTypeToString(ViewportClient->GetViewportType()));
	Result->SetBoolField(TEXT("realtime"), ViewportClient->IsRealtime());
	Result->SetBoolField(TEXT("uniform"), bUniform);

	if (const UWorld* World = ViewportClient->GetWorld())
	{
		Result->SetStringField(TEXT("level"), World->GetOutermost()->GetName());
	}

	// The camera the frame was actually taken with (the override when one was
	// given, otherwise the user's own camera).
	Result->SetArrayField(TEXT("camera_location"),
		VectorToJson(Request.bHasCamera ? Request.CameraLocation : PrevLocation));
	Result->SetArrayField(TEXT("camera_rotation"),
		RotatorToJson(Request.bHasCamera ? Request.CameraRotation : PrevRotation));
	Result->SetNumberField(TEXT("fov"),
		(Request.bHasCamera && Request.bHasFov) ? Request.CameraFov : PrevFov);
	Result->SetBoolField(TEXT("camera_overridden"), Request.bHasCamera);
	Result->SetBoolField(TEXT("camera_restored"), bCameraMoved && Request.bRestoreCamera);

	if (Viewport->GetSceneHDREnabled())
	{
		// Read back through the LDR path (same as the engine's own non-HDR
		// branch); colours in an HDR-output viewport can differ from screen.
		Result->SetBoolField(TEXT("scene_hdr"), true);
	}

	Result->SetNumberField(TEXT("capture_time_ms"), ElapsedMs);

	return FMonolithActionResult::Success(Result);
}
