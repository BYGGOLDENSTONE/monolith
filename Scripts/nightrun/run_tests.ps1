# Unattended automation-test run via UnrealEditor-Cmd (-nullrhi: logic tests
# don't need rendering). Produces the UE JSON report plus results/run_tests.json
# with pass/fail counts and failing test names — the file the verification
# agent reads. Exit code: 0 = all passed, 1 = failures/timeout/crash.

param(
    [int]$TimeoutMinutes = 30,
    [string]$Filter = 'Monolith'
)

. "$PSScriptRoot\nightrun_common.ps1"

$killed = Stop-MonolithProcesses
$testLog = Join-Path $NightrunResults 'run_tests.log'
$reportDir = Join-Path $NightrunResults 'testreport'
if (Test-Path $testLog) { Remove-Item $testLog -Force }
if (Test-Path $reportDir) { Remove-Item $reportDir -Recurse -Force }

$exe = Join-Path $NightrunEngine 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$args = @(
    "`"$NightrunProject`"",
    "-ExecCmds=`"Automation RunTests $Filter; Quit`"",
    '-unattended', '-nullrhi', '-nosplash', '-NoSound',
    "-abslog=`"$testLog`"",
    "-ReportOutputPath=`"$reportDir`""
)

$start = Get-Date
$proc = Start-Process -FilePath $exe -ArgumentList $args -PassThru
$finished = $proc.WaitForExit($TimeoutMinutes * 60 * 1000)
$duration = [int]((Get-Date) - $start).TotalSeconds

if (-not $finished) {
    & taskkill /PID $proc.Id /T /F 2>$null | Out-Null
    Stop-MonolithProcesses | Out-Null
    $resultPath = Write-ResultJson 'run_tests' @{
        status = 'red'; reason = "timeout after $TimeoutMinutes min"
        durationSec = $duration; staleKilled = $killed
        logTail = Get-LogTail $testLog; log = $testLog
    }
    Write-Host "TESTS RED (timeout) — $resultPath"
    exit 1
}

# Parse the UE report (index.json) — stronger signal than log scraping.
$passed = 0; $failed = 0; $skipped = 0; $failing = @()
$indexJson = Join-Path $reportDir 'index.json'
if (Test-Path $indexJson) {
    $report = Get-Content $indexJson -Raw | ConvertFrom-Json
    foreach ($t in $report.tests) {
        switch -Regex ("$($t.state)") {
            '^Success$'    { $passed++ }
            '^Fail'        { $failed++; $failing += $t.fullTestPath }
            '^(Skipped|NotRun)$' { $skipped++ }
            default        { $failed++; $failing += "$($t.fullTestPath) (state=$($t.state))" }
        }
    }
} else {
    $failed = -1  # report missing = the run itself broke (crash before writing)
}

$crashed = ($proc.ExitCode -ne 0)
$green = (-not $crashed) -and ($failed -eq 0) -and ($passed -gt 0)

$resultPath = Write-ResultJson 'run_tests' @{
    status = if ($green) { 'green' } else { 'red' }
    exitCode = $proc.ExitCode; durationSec = $duration; filter = $Filter
    passed = $passed; failed = $failed; skipped = $skipped
    failingTests = $failing
    reportIndex = $indexJson; log = $testLog; staleKilled = $killed
    reason = if ($failed -eq -1) { 'UE report index.json missing — run likely crashed' }
             elseif ($passed -eq 0 -and $failed -eq 0) { "no tests matched filter '$Filter'" }
             else { $null }
    logTail = if ($green) { @() } else { Get-LogTail $testLog }
    recentCrashes = if ($green) { @() } else { Get-RecentCrashes 60 }
}

Write-Host "TESTS $(if ($green) { 'GREEN' } else { 'RED' }) — passed=$passed failed=$failed skipped=$skipped — $resultPath"
if (-not $green) { $failing | Select-Object -First 10 | ForEach-Object { Write-Host "  FAIL: $_" } }
exit $(if ($green) { 0 } else { 1 })
