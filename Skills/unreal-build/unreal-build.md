---
name: unreal-build
description: Build Unreal projects and diagnose compilation failures using the installed engine, Live Coding status, and reproducible full builds.
---

# Unreal build workflow

Discover the actual engine root, `.uproject`, target and platform before constructing a build command. Use `Engine/Build/Build.version` to confirm the installed patch; a folder named UE_5.7 alone does not establish 5.7.4. Do not hardcode another developer's engine path.

## Choose a build

Use Live Coding for a small implementation change when the editor and Live Coding are available. Prefer a saved, closed-editor full build for module/dependency changes, plugin descriptors, or structural changes whose reinstancing behavior has not been validated. This is a reliability choice: UE supports object reinstancing for reflected classes, so headers are not categorically unsupported. Constructor defaults in `.cpp` do not update existing instances automatically. See [Epic's UE 5.7 Live Coding documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-live-coding-to-recompile-unreal-engine-applications-at-runtime?application_version=5.7).

An MCP timeout can mean a wrong URL, unavailable plugin, busy game thread or proxy problem. It does **not** confirm the editor is closed. Verify process/project identity through local process inspection and logs before rebuilding binaries.

For shared-editor work, retain the coordination lease during compilation and status polling. Serialize build/PIE/restart operations across agents. Coordinate editor closure with unsaved work and the user's existing authorization.

## Live Coding

Discover `editor` and inspect schemas for `trigger_build`, `get_build_status`, `get_compile_output` and `get_build_errors`. Read baseline state, trigger once, then poll at a bounded interval until the requested build reaches a terminal result. Do not substitute a fixed ten-second delay for completion or interpret an old successful build as the new result. A timeout after triggering leaves the outcome unknown; inspect status and logs before retrying.

## Full project build (PowerShell)

Replace these example values with discovered paths and the actual target:

```powershell
$engineRoot = 'C:\Program Files\Epic Games\UE_5.7'
$projectFile = 'D:\Projects\MyGame\MyGame.uproject'
& "$engineRoot\Engine\Build\BatchFiles\Build.bat" MyGameEditor Win64 Development "-Project=$projectFile" -WaitMutex
if ($LASTEXITCODE -ne 0) { throw 'Unreal build failed; inspect the build output.' }
```

The engine's Build.bat wrapper or a verified direct UBT invocation may be used. A successful Editor build is one gate; asset compile/save/readback, runtime behavior, and a cooked packaged game remain separate checks.
