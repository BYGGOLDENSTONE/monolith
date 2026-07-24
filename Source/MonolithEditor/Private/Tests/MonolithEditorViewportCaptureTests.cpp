// Copyright tumourlove. All Rights Reserved.

// =============================================================================
// MonolithEditorViewportCaptureTests.cpp
//
// Automation coverage for editor::capture_viewport (Faz 2 slice 3 — live editor
// level-viewport screenshot).
//
// HEADLESS CEILING — READ THIS BEFORE TRUSTING THE GREEN.
// The night harness runs the editor with -nullrhi. In that mode FApp::CanEverRender()
// is false, nothing is ever rendered into a level viewport, and no pixel can be
// produced. Therefore NOTHING HERE PROVES IMAGE CONTENT. What is proven:
//   * every parameter is validated, with the right message, before any environment
//     probe runs (so a malformed call is reported as malformed even under -nullrhi);
//   * defaults come from UMonolithSettings ("Capture" category), not from constants
//     baked into the action body;
//   * output-path construction: relative -> project-absolute, extension gating,
//     settings-driven default directory;
//   * the "nothing was rendered" detector (uniform-pixel heuristic) and the
//     longest-side downscale math, over synthetic buffers;
//   * the no-rendering-path branch produces ONE specific, actionable error and
//     writes no file.
// Actual pixels — that the PNG shows the open map as the user sees it — is
// verified only by a human (or by a windowed editor run), never by this file.
// =============================================================================

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "MonolithEditorActions.h"
#include "MonolithEditorViewportCapture.h"
#include "MonolithSettings.h"
#include "MonolithToolRegistry.h"

namespace MonolithViewportCaptureTests
{
	/** Empty param object — capture_viewport takes no required parameters. */
	static TSharedPtr<FJsonObject> NoParams()
	{
		return MakeShared<FJsonObject>();
	}

	/** Params carrying exactly one field. */
	static TSharedPtr<FJsonObject> WithNumber(const TCHAR* Field, double Value)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetNumberField(Field, Value);
		return P;
	}

	static TSharedPtr<FJsonObject> WithString(const TCHAR* Field, const FString& Value)
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(Field, Value);
		return P;
	}

	/** Build a uniform pixel buffer of Count entries. */
	static TArray<FColor> UniformPixels(int32 Count, FColor Colour)
	{
		TArray<FColor> Pixels;
		Pixels.Init(Colour, Count);
		return Pixels;
	}
}

// ============================================================================
// Test 1 — parameter validation runs first and is precise
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithViewportCaptureParamValidationTest,
	"Monolith.Editor.ViewportCapture.ParamValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithViewportCaptureParamValidationTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithViewportCapture;
	using namespace MonolithViewportCaptureTests;

	FRequest Request;
	FString Error;

	// Negative viewport index.
	TestFalse(TEXT("Negative viewport_index rejected"),
		ParseRequest(WithNumber(TEXT("viewport_index"), -1.0), Request, Error));
	TestTrue(TEXT("viewport_index error names the field"), Error.Contains(TEXT("viewport_index")));

	// Non-numeric viewport index.
	TestFalse(TEXT("String viewport_index rejected"),
		ParseRequest(WithString(TEXT("viewport_index"), TEXT("first")), Request, Error));
	TestTrue(TEXT("Type error says 'number'"), Error.Contains(TEXT("number")));

	// Out-of-range quality.
	TestFalse(TEXT("quality=101 rejected"),
		ParseRequest(WithNumber(TEXT("quality"), 101.0), Request, Error));
	TestTrue(TEXT("quality error states the range"), Error.Contains(TEXT("0 and 100")));

	// Negative max_dimension.
	TestFalse(TEXT("Negative max_dimension rejected"),
		ParseRequest(WithNumber(TEXT("max_dimension"), -8.0), Request, Error));
	TestTrue(TEXT("max_dimension error explains 0"), Error.Contains(TEXT("max_dimension")));

	// Boolean fields go through the engine's TryGetBoolField, so 1/0 coerce (the
	// same leniency every other Monolith action inherits) ...
	TestTrue(TEXT("redraw=1 coerces to true"),
		ParseRequest(WithNumber(TEXT("redraw"), 1.0), Request, Error));
	TestTrue(TEXT("redraw=1 really means true"), Request.bRedraw);
	TestTrue(TEXT("redraw=0 coerces to false"),
		ParseRequest(WithNumber(TEXT("redraw"), 0.0), Request, Error));
	TestFalse(TEXT("redraw=0 really means false"), Request.bRedraw);

	// ... but a value with no boolean reading at all is a typed error.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Junk;
		Junk.Add(MakeShared<FJsonValueNumber>(1.0));
		Params->SetArrayField(TEXT("redraw"), Junk);

		TestFalse(TEXT("Array redraw rejected"), ParseRequest(Params, Request, Error));
		TestTrue(TEXT("Bool type error says 'boolean'"), Error.Contains(TEXT("boolean")));
	}

	// Unsupported output extension.
	TestFalse(TEXT("output_path with .txt rejected"),
		ParseRequest(WithString(TEXT("output_path"), TEXT("Saved/shot.txt")), Request, Error));
	TestTrue(TEXT("Extension error lists supported formats"), Error.Contains(TEXT("png")));

	// output_path with no extension at all.
	TestFalse(TEXT("Extension-less output_path rejected"),
		ParseRequest(WithString(TEXT("output_path"), TEXT("Saved/shot")), Request, Error));
	TestTrue(TEXT("Missing-extension error mentions extension"), Error.Contains(TEXT("extension")));

	// camera object missing rotation.
	{
		TSharedPtr<FJsonObject> Camera = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Loc;
		Loc.Add(MakeShared<FJsonValueNumber>(0.0));
		Loc.Add(MakeShared<FJsonValueNumber>(0.0));
		Loc.Add(MakeShared<FJsonValueNumber>(0.0));
		Camera->SetArrayField(TEXT("location"), Loc);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("camera"), Camera);

		TestFalse(TEXT("camera without rotation rejected"), ParseRequest(Params, Request, Error));
		TestTrue(TEXT("camera error names rotation"), Error.Contains(TEXT("camera.rotation")));
	}

	// camera with an impossible FOV.
	{
		TSharedPtr<FJsonObject> Camera = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Loc, Rot;
		for (int32 i = 0; i < 3; ++i)
		{
			Loc.Add(MakeShared<FJsonValueNumber>(0.0));
			Rot.Add(MakeShared<FJsonValueNumber>(0.0));
		}
		Camera->SetArrayField(TEXT("location"), Loc);
		Camera->SetArrayField(TEXT("rotation"), Rot);
		Camera->SetNumberField(TEXT("fov"), 200.0);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetObjectField(TEXT("camera"), Camera);

		TestFalse(TEXT("camera.fov=200 rejected"), ParseRequest(Params, Request, Error));
		TestTrue(TEXT("fov error names the field"), Error.Contains(TEXT("camera.fov")));
	}

	// A well-formed camera (including the string-serialized variant clients emit).
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("camera"),
			TEXT("{\"location\":[100,200,300],\"rotation\":[-10,90,0],\"fov\":75}"));

		TestTrue(TEXT("String-serialized camera accepted"), ParseRequest(Params, Request, Error));
		TestTrue(TEXT("camera flagged present"), Request.bHasCamera);
		TestEqual(TEXT("camera location X"), Request.CameraLocation.X, 100.0);
		TestEqual(TEXT("camera rotation Yaw"), Request.CameraRotation.Yaw, 90.0);
		TestTrue(TEXT("fov flagged present"), Request.bHasFov);
		TestEqual(TEXT("camera fov"), Request.CameraFov, 75.0f);
	}

	// Validation happens BEFORE any environment probe: the same bad call through
	// the real action must come back as an invalid-params error (-32602), not as
	// the no-rendering-path error, even under -nullrhi.
	{
		FMonolithActionResult Result =
			FMonolithEditorActions::HandleCaptureViewport(WithNumber(TEXT("quality"), 101.0));
		TestFalse(TEXT("Bad quality fails through the action"), Result.bSuccess);
		TestEqual(TEXT("Bad params reported as -32602"), Result.ErrorCode, -32602);
		TestTrue(TEXT("Param error, not an environment error"),
			Result.ErrorMessage.Contains(TEXT("quality")));
	}

	return true;
}

// ============================================================================
// Test 2 — defaults come from UMonolithSettings, not from the action body
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithViewportCaptureSettingsDefaultsTest,
	"Monolith.Editor.ViewportCapture.SettingsDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithViewportCaptureSettingsDefaultsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithViewportCapture;
	using namespace MonolithViewportCaptureTests;

	const UMonolithSettings* Settings = UMonolithSettings::Get();
	if (!Settings)
	{
		AddError(TEXT("UMonolithSettings::Get() returned null — capture defaults are unreachable."));
		return false;
	}

	FRequest Request;
	FString Error;
	TestTrue(TEXT("Empty params parse cleanly"), ParseRequest(NoParams(), Request, Error));

	TestEqual(TEXT("max_dimension default is the setting"),
		Request.MaxDimension, Settings->ViewportCaptureMaxDimension);
	TestEqual(TEXT("quality default is the setting"),
		Request.Quality, Settings->ViewportCaptureQuality);
	TestEqual(TEXT("No viewport_index means 'active viewport'"),
		Request.ViewportIndex, (int32)INDEX_NONE);
	TestTrue(TEXT("redraw defaults on"), Request.bRedraw);
	TestFalse(TEXT("allow_uniform defaults off"), Request.bAllowUniform);
	TestTrue(TEXT("restore_camera defaults on"), Request.bRestoreCamera);
	TestFalse(TEXT("No camera override by default"), Request.bHasCamera);

	// The default output path must live under the settings-declared directory.
	TestTrue(TEXT("Default output path honours the settings directory"),
		Request.OutputPath.Replace(TEXT("\\"), TEXT("/"))
			.Contains(Settings->ViewportCaptureDirectory.Replace(TEXT("\\"), TEXT("/"))));

	// Per-request overrides beat the settings.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("max_dimension"), 640.0);
		Params->SetNumberField(TEXT("quality"), 55.0);
		Params->SetBoolField(TEXT("allow_uniform"), true);

		FRequest Override;
		TestTrue(TEXT("Override params parse"), ParseRequest(Params, Override, Error));
		TestEqual(TEXT("max_dimension override applied"), Override.MaxDimension, 640);
		TestEqual(TEXT("quality override applied"), Override.Quality, 55);
		TestTrue(TEXT("allow_uniform override applied"), Override.bAllowUniform);
	}

	return true;
}

// ============================================================================
// Test 3 — output-path construction
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithViewportCaptureOutputPathTest,
	"Monolith.Editor.ViewportCapture.OutputPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithViewportCaptureOutputPathTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithViewportCapture;
	using namespace MonolithViewportCaptureTests;

	FRequest Request;
	FString Error;

	// FPaths::ProjectDir() is itself engine-relative, so every path we hand back is
	// fully qualified first — a client on the other end of MCP cannot resolve the
	// editor's working directory.
	const FString ProjectFull =
		FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()).Replace(TEXT("\\"), TEXT("/"));

	// Deterministic default path.
	const FString Stamped = BuildDefaultOutputPath(TEXT("19991231_235959"));
	TestTrue(TEXT("Default path is fully qualified"), !FPaths::IsRelative(Stamped));
	TestTrue(TEXT("Default path uses the stamp"), Stamped.Contains(TEXT("19991231_235959")));
	TestEqual(TEXT("Default path is a PNG"), FPaths::GetExtension(Stamped).ToLower(), FString(TEXT("png")));
	TestTrue(TEXT("Default path sits under the project dir"),
		Stamped.Replace(TEXT("\\"), TEXT("/")).StartsWith(ProjectFull));

	// Relative output_path is anchored to the project directory.
	TestTrue(TEXT("Relative output_path parses"),
		ParseRequest(WithString(TEXT("output_path"), TEXT("Saved/Tests/Monolith/shot.png")), Request, Error));
	TestTrue(TEXT("Relative output_path became fully qualified"), !FPaths::IsRelative(Request.OutputPath));
	TestTrue(TEXT("Relative output_path anchored to the project"),
		Request.OutputPath.Replace(TEXT("\\"), TEXT("/")).StartsWith(ProjectFull));

	// Absolute output_path is left alone.
	{
		const FString Absolute = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectDir() / TEXT("Saved/Tests/Monolith/absolute_shot.jpg"));
		TestTrue(TEXT("Absolute output_path parses"),
			ParseRequest(WithString(TEXT("output_path"), Absolute), Request, Error));
		TestEqual(TEXT("Absolute output_path preserved"),
			Request.OutputPath.Replace(TEXT("\\"), TEXT("/")),
			Absolute.Replace(TEXT("\\"), TEXT("/")));
	}

	// Extension gating is case-insensitive and covers the whole supported set.
	TestTrue(TEXT(".PNG accepted"), IsSupportedExtension(TEXT("PNG")));
	TestTrue(TEXT(".jpeg accepted"), IsSupportedExtension(TEXT("jpeg")));
	TestTrue(TEXT(".exr accepted"), IsSupportedExtension(TEXT("exr")));
	TestFalse(TEXT(".gif rejected"), IsSupportedExtension(TEXT("gif")));
	TestFalse(TEXT("Empty extension rejected"), IsSupportedExtension(FString()));
	TestTrue(TEXT("Supported list is non-empty"), !SupportedExtensionList().IsEmpty());

	return true;
}

// ============================================================================
// Test 4 — "nothing was rendered" detector
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithViewportCaptureUniformDetectionTest,
	"Monolith.Editor.ViewportCapture.UniformDetection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithViewportCaptureUniformDetectionTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithViewportCapture;
	using namespace MonolithViewportCaptureTests;

	TestTrue(TEXT("Empty buffer counts as uniform"), PixelsAreUniform(TArray<FColor>()));

	TArray<FColor> Black = UniformPixels(64, FColor(0, 0, 0, 255));
	TestTrue(TEXT("All-black buffer is uniform"), PixelsAreUniform(Black));

	TArray<FColor> Grey = UniformPixels(64, FColor(64, 64, 64, 255));
	TestTrue(TEXT("Flat clear-colour buffer is uniform"), PixelsAreUniform(Grey));

	// Alpha-only variation must NOT count as content — viewport reads carry
	// scene-depth alpha noise, which would otherwise mask a blank frame.
	TArray<FColor> AlphaNoise = UniformPixels(64, FColor(0, 0, 0, 255));
	AlphaNoise[17].A = 3;
	TestTrue(TEXT("Alpha-only variation is still uniform"), PixelsAreUniform(AlphaNoise));

	// A single differing RGB pixel is enough to call it a render.
	TArray<FColor> OnePixel = UniformPixels(64, FColor(0, 0, 0, 255));
	OnePixel[33] = FColor(0, 0, 1, 255);
	TestFalse(TEXT("One differing RGB pixel means rendered"), PixelsAreUniform(OnePixel));

	return true;
}

// ============================================================================
// Test 5 — longest-side downscale math
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithViewportCaptureFitTest,
	"Monolith.Editor.ViewportCapture.MaxDimensionFit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithViewportCaptureFitTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithViewportCapture;

	// 0 = passthrough.
	TestEqual(TEXT("max_dimension 0 keeps native size"),
		FitToMaxDimension(FIntPoint(3840, 2160), 0), FIntPoint(3840, 2160));

	// Already small enough — never upscaled.
	TestEqual(TEXT("Small frame is not upscaled"),
		FitToMaxDimension(FIntPoint(800, 600), 1920), FIntPoint(800, 600));

	// Landscape: width is the longest side.
	TestEqual(TEXT("4K landscape fits to 1920 wide"),
		FitToMaxDimension(FIntPoint(3840, 2160), 1920), FIntPoint(1920, 1080));

	// Portrait: height is the longest side.
	TestEqual(TEXT("Portrait fits to 1920 tall"),
		FitToMaxDimension(FIntPoint(1080, 2160), 1920), FIntPoint(960, 1920));

	// Degenerate sizes never produce a zero dimension.
	{
		const FIntPoint Thin = FitToMaxDimension(FIntPoint(4000, 1), 100);
		TestEqual(TEXT("Extreme aspect fits the long side"), Thin.X, 100);
		TestTrue(TEXT("Extreme aspect keeps at least one pixel"), Thin.Y >= 1);
	}

	return true;
}

// ============================================================================
// Test 6 — the no-rendering-path branch is a clean error, and writes nothing
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithViewportCaptureHeadlessTest,
	"Monolith.Editor.ViewportCapture.HeadlessIsCleanError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithViewportCaptureHeadlessTest::RunTest(const FString& /*Parameters*/)
{
	using namespace MonolithViewportCaptureTests;

	const FString OutputPath =
		FPaths::ProjectDir() / TEXT("Saved/Tests/Monolith/ViewportCapture/headless_probe.png");
	if (FPaths::FileExists(OutputPath))
	{
		IFileManager::Get().Delete(*OutputPath, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);
	}

	FMonolithActionResult Result =
		FMonolithEditorActions::HandleCaptureViewport(WithString(TEXT("output_path"), OutputPath));

	if (!FApp::CanEverRender())
	{
		// The harness case. This is the whole contract under -nullrhi: refuse,
		// say why, and leave no file behind.
		TestFalse(TEXT("capture_viewport fails with no rendering path"), Result.bSuccess);
		TestEqual(TEXT("Internal-error code used for environment refusal"), Result.ErrorCode, -32603);
		TestTrue(TEXT("Error explains there is no rendering path"),
			Result.ErrorMessage.Contains(TEXT("rendering path")));
		TestTrue(TEXT("Error names nullrhi so the cause is obvious"),
			Result.ErrorMessage.Contains(TEXT("nullrhi")));
		TestFalse(TEXT("No image file was written"), FPaths::FileExists(OutputPath));
	}
	else
	{
		// A windowed editor. We cannot assert success from automation (the level
		// viewport may legitimately be absent or unrendered while tests run), but
		// the no-rendering-path branch must NOT be the one that fires.
		AddInfo(TEXT("Rendering is available — the -nullrhi refusal branch is not exercised here."));
		if (!Result.bSuccess)
		{
			TestFalse(TEXT("A rendering-capable process must not claim it cannot render"),
				Result.ErrorMessage.Contains(TEXT("no rendering path")));
			AddInfo(FString::Printf(TEXT("capture_viewport declined: %s"), *Result.ErrorMessage));
		}
		else
		{
			TestTrue(TEXT("Successful capture wrote its file"), FPaths::FileExists(OutputPath));
		}
	}

	if (FPaths::FileExists(OutputPath))
	{
		IFileManager::Get().Delete(*OutputPath, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);
	}
	return true;
}

// ============================================================================
// Test 7 — the action is discoverable under the `editor` namespace
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMonolithViewportCaptureRegistrationTest,
	"Monolith.Editor.ViewportCapture.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMonolithViewportCaptureRegistrationTest::RunTest(const FString& /*Parameters*/)
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	// The editor namespace is not registered in commandlet-style runs. Use a
	// long-standing sibling as the probe rather than force-registering (which
	// would need the log-capture singleton and could double-register).
	if (!Registry.HasAction(TEXT("editor"), TEXT("get_viewport_info")))
	{
		AddInfo(TEXT("Skipped — the `editor` namespace is not registered in this process."));
		return true;
	}

	TestTrue(TEXT("editor::capture_viewport is registered"),
		Registry.HasAction(TEXT("editor"), TEXT("capture_viewport")));

	bool bFound = false;
	for (const FMonolithActionInfo& Info : Registry.GetActions(TEXT("editor")))
	{
		if (Info.Action == TEXT("capture_viewport"))
		{
			bFound = true;
			TestTrue(TEXT("capture_viewport has a description"), !Info.Description.IsEmpty());
			TestTrue(TEXT("capture_viewport publishes a param schema"), Info.ParamSchema.IsValid());
			break;
		}
	}
	TestTrue(TEXT("capture_viewport appears in the editor action list"), bFound);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
