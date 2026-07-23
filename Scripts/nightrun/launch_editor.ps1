# Unattended editor launch. Default mode is RENDERED (real RHI, a window may
# appear — fine overnight) so screenshots/VFX/lighting verification work.
# Pass -NullRhi for fast logic-only sessions. Waits until the Monolith MCP
# server answers on port 9316, then leaves the editor RUNNING and exits 0.
# On early process death or timeout: collects exit code, log tail and recent
# crash folders into results/launch_editor.json and exits 1.

param(
    [switch]$NullRhi,
    [int]$TimeoutMinutes = 25,
    [int]$Port = 0
)

. "$PSScriptRoot\nightrun_common.ps1"
if ($Port -eq 0) { $Port = $NightrunPort }

$killed = Stop-MonolithProcesses
$editorLog = Join-Path $NightrunResults 'editor.log'
if (Test-Path $editorLog) { Remove-Item $editorLog -Force }

$exe = Join-Path $NightrunEngine 'Engine\Binaries\Win64\UnrealEditor.exe'
$args = @("`"$NightrunProject`"", '-unattended', '-nosplash', '-NoVerifyGC', "-abslog=`"$editorLog`"")
if ($NullRhi) { $args += '-nullrhi' }

$start = Get-Date
$proc = Start-Process -FilePath $exe -ArgumentList $args -PassThru
$deadlineMs = $TimeoutMinutes * 60 * 1000

while (((Get-Date) - $start).TotalMilliseconds -lt $deadlineMs) {
    if ($proc.HasExited) {
        $resultPath = Write-ResultJson 'launch_editor' @{
            status = 'failed'; reason = "editor process exited early (code $($proc.ExitCode))"
            exitCode = $proc.ExitCode; mode = if ($NullRhi) { 'nullrhi' } else { 'rendered' }
            elapsedSec = [int]((Get-Date) - $start).TotalSeconds
            staleKilled = $killed; logTail = Get-LogTail $editorLog
            recentCrashes = Get-RecentCrashes 30; log = $editorLog
        }
        Write-Host "LAUNCH FAILED (early exit $($proc.ExitCode)) — $resultPath"
        exit 1
    }
    $health = Get-NightrunHealth -Port $Port
    if ($health -and $health.status -eq 'ok') {
        $elapsed = [int]((Get-Date) - $start).TotalSeconds
        $resultPath = Write-ResultJson 'launch_editor' @{
            status = 'up'; pid = $proc.Id; port = $Port
            mode = if ($NullRhi) { 'nullrhi' } else { 'rendered' }
            elapsedSec = $elapsed; staleKilled = $killed; log = $editorLog
            serverVersion = $health.version; toolsRegistered = $health.tools_registered
        }
        Write-Host "EDITOR UP (pid $($proc.Id), port $Port, ${elapsed}s) — $resultPath"
        exit 0
    }
    Start-Sleep -Seconds 5
}

# Timed out: the editor is wedged or still compiling shaders far beyond budget.
& taskkill /PID $proc.Id /T /F 2>$null | Out-Null
Stop-MonolithProcesses | Out-Null
$resultPath = Write-ResultJson 'launch_editor' @{
    status = 'failed'; reason = "port $Port not open after $TimeoutMinutes min; editor killed"
    mode = if ($NullRhi) { 'nullrhi' } else { 'rendered' }
    elapsedSec = [int]((Get-Date) - $start).TotalSeconds
    staleKilled = $killed; logTail = Get-LogTail $editorLog
    recentCrashes = Get-RecentCrashes 30; log = $editorLog
}
Write-Host "LAUNCH FAILED (timeout) — $resultPath"
exit 1
