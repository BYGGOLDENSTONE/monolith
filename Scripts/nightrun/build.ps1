# Unattended UBT build wrapper. Kills stale processes that would deadlock
# -WaitMutex, enforces a hard timeout, and emits results/build.json with a
# green/red verdict plus an error summary the orchestrator can trust blindly.
# Exit code: 0 = green, 1 = red/timeout.

param(
    [int]$TimeoutMinutes = 45,
    [string]$Target = 'MonolithDevEditor',
    [string]$Configuration = 'Development'
)

. "$PSScriptRoot\nightrun_common.ps1"

$killed = Stop-MonolithProcesses
$log = Join-Path $NightrunResults 'build.log'
if (Test-Path $log) { Remove-Item $log -Force }

$buildBat = Join-Path $NightrunEngine 'Engine\Build\BatchFiles\Build.bat'
$args = @($Target, 'Win64', $Configuration, "-project=`"$NightrunProject`"", '-WaitMutex')

$start = Get-Date
$proc = Start-Process -FilePath $buildBat -ArgumentList $args `
    -RedirectStandardOutput $log -RedirectStandardError (Join-Path $NightrunResults 'build.err.log') `
    -NoNewWindow -PassThru

$finished = $proc.WaitForExit($TimeoutMinutes * 60 * 1000)
$duration = [int]((Get-Date) - $start).TotalSeconds

if (-not $finished) {
    # Kill the whole tree; a wedged UBT would otherwise block every later step.
    & taskkill /PID $proc.Id /T /F 2>$null | Out-Null
    Stop-MonolithProcesses | Out-Null
    $resultPath = Write-ResultJson 'build' @{
        status = 'red'; reason = "timeout after $TimeoutMinutes min"
        durationSec = $duration; staleKilled = $killed
        errorSummary = @(); logTail = Get-LogTail $log; log = $log
    }
    Write-Host "BUILD RED (timeout) — $resultPath"
    exit 1
}

$content = if (Test-Path $log) { Get-Content $log } else { @() }
$succeeded = ($proc.ExitCode -eq 0) -and ($content -match 'Result:\s*Succeeded')
$errors = @($content | Where-Object { $_ -match '(:\s*(fatal )?error\b|^\s*ERROR:|error [A-Z]+\d+)' } | Select-Object -First 15)

$resultPath = Write-ResultJson 'build' @{
    status = if ($succeeded) { 'green' } else { 'red' }
    exitCode = $proc.ExitCode; durationSec = $duration
    staleKilled = $killed; errorSummary = $errors; log = $log
    logTail = if ($succeeded) { @() } else { Get-LogTail $log }
}

if ($succeeded) {
    Write-Host "BUILD GREEN (${duration}s) — $resultPath"
    exit 0
} else {
    Write-Host "BUILD RED (exit $($proc.ExitCode)) — $resultPath"
    $errors | ForEach-Object { Write-Host "  $_" }
    exit 1
}
