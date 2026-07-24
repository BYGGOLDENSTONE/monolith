// Copyright tumourlove. All Rights Reserved.

#pragma once

// =============================================================================
// MonolithEditorViewportCapture.h
//
// Pure (engine-free) helpers behind editor::capture_viewport — the LIVE editor
// level-viewport screenshot (Faz 2, roadmap item "canli editor viewport ekran
// goruntusu").
//
// Everything here EXCEPT ResolveLevelViewport (bottom of the file) is deliberately
// free of GEditor / FViewport / RHI so it can be exercised by automation under
// -nullrhi, where no pixel can ever be produced. The rendering half lives in
// MonolithEditorViewportCapture.cpp next to the action body.
// =============================================================================

#include "CoreMinimal.h"

class FJsonObject;
class FLevelEditorViewportClient;

namespace MonolithViewportCapture
{
	/**
	 * Parsed + validated form of the editor::capture_viewport parameter block.
	 * Populated by ParseRequest; every field is already range-checked, so the
	 * action body never has to re-validate.
	 */
	struct FRequest
	{
		/** Index into GEditor->GetLevelViewportClients(). INDEX_NONE = use the active viewport. */
		int32 ViewportIndex = INDEX_NONE;

		/** Absolute output file path (extension decides the encoder). */
		FString OutputPath;

		/** Longest-side pixel cap; the image is downscaled to fit. 0 = never downscale. */
		int32 MaxDimension = 0;

		/** Encoder quality for lossy formats. 0 = encoder default. */
		int32 Quality = 0;

		/** Force a fresh Invalidate + Draw before reading back. */
		bool bRedraw = true;

		/** Accept a frame whose pixels are all one colour (normally treated as "nothing was rendered"). */
		bool bAllowUniform = false;

		/** True when the caller supplied a `camera` override object. */
		bool bHasCamera = false;
		FVector CameraLocation = FVector::ZeroVector;
		FRotator CameraRotation = FRotator::ZeroRotator;

		/** True when the `camera` object carried an `fov` field. */
		bool bHasFov = false;
		float CameraFov = 0.0f;

		/** Put the viewport camera back where it was after the capture. */
		bool bRestoreCamera = true;
	};

	/** File extensions FImageUtils::SaveImageAutoFormat can encode, lower-case, no dot. */
	const TArray<FString>& SupportedExtensions();

	/** Human-readable "png, jpg, ..." list for error messages. */
	FString SupportedExtensionList();

	/** True when Ext (lower- or upper-case, no dot) is an encodable output format. */
	bool IsSupportedExtension(const FString& Ext);

	/**
	 * Auto-generated output path: <ProjectDir>/<Dir>/<timestamp>.png, where Dir
	 * comes from UMonolithSettings::ViewportCaptureDirectory (data-driven).
	 * Stamp is caller-supplied so tests are deterministic.
	 */
	FString BuildDefaultOutputPath(const FString& Stamp);

	/**
	 * Parse + validate every parameter. Returns false with an actionable
	 * OutError on the first problem; OutRequest is then meaningless.
	 *
	 * Deliberately runs BEFORE any environment probe in the action body so a
	 * malformed call is reported as a malformed call even in a process that
	 * could never render anyway.
	 */
	bool ParseRequest(const TSharedPtr<FJsonObject>& Params, FRequest& OutRequest, FString& OutError);

	/**
	 * True when every sampled pixel shares one RGB triple. A viewport that was
	 * never rendered into reads back as a single clear colour (usually black),
	 * so this is the "we produced garbage" detector. Sampled with a stride for
	 * speed — same heuristic the PIE clip capture uses.
	 */
	bool PixelsAreUniform(const TArray<FColor>& Pixels);

	/** Longest-side fit: returns the (W,H) Size scaled down so max(W,H) <= MaxDimension. */
	FIntPoint FitToMaxDimension(FIntPoint Size, int32 MaxDimension);

	// ------------------------------------------------------------------------
	// Engine-bound (the ONLY part of this header that touches GEditor).
	// ------------------------------------------------------------------------

	/** The level viewport an action resolved to, plus the facts callers report. */
	struct FResolvedViewport
	{
		/** Never null on success. */
		FLevelEditorViewportClient* Client = nullptr;

		/** Index into GEditor->GetLevelViewportClients(). */
		int32 Index = INDEX_NONE;

		/** How many level viewports are open. */
		int32 Count = 0;

		/**
		 * Render-target size in pixels — the same figure capture_viewport reads
		 * back over. Guaranteed > 0 in both axes on success.
		 */
		FIntPoint Size = FIntPoint::ZeroValue;

		/**
		 * True only when the caller left the index open AND the editor's focused
		 * viewport (GCurrentLevelEditingViewportClient) is the one we resolved to,
		 * i.e. "you got the active viewport because you did not pick one".
		 */
		bool bWasActive = false;

		/** ELevelViewportType as a stable public string ("perspective", "ortho_top", ...). */
		FString TypeName;
	};

	/**
	 * Resolve which open level viewport an action should act on, and prove it is
	 * usable — registered, realised on screen, and non-zero-sized.
	 *
	 * Shared by editor::capture_viewport and editor::get_viewport_info so the two
	 * can never disagree about "the viewport". Reading GetLevelViewportClients()[0]
	 * directly is the bug this exists to prevent: in a stock editor layout index 0
	 * is frequently a hidden 0x0 client, and reporting its camera as the truth is a
	 * silent lie.
	 *
	 * @param RequestedIndex        Explicit viewport index, or INDEX_NONE to prefer
	 *                              the focused viewport (falling back to index 0).
	 * @param ActionName            Named in every error message, e.g. "capture_viewport".
	 * @param ExtraHintWhenNoneOpen Optional sentence appended to the "no viewport is
	 *                              open" message; use it to point at an alternative action.
	 * @return true on success (Out fully populated). On false, OutError carries an
	 *         actionable plain message and OutErrorCode the JSON-RPC code (-32602 for
	 *         a bad caller index, -32603 for an environment problem).
	 */
	bool ResolveLevelViewport(
		int32 RequestedIndex,
		const TCHAR* ActionName,
		FResolvedViewport& Out,
		FString& OutError,
		int32& OutErrorCode,
		const TCHAR* ExtraHintWhenNoneOpen = nullptr);
}
