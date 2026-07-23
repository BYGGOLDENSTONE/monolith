# Clean shutdown. Every worker runs this before finishing, success or failure —
# leftover editor/UBT processes would deadlock the next build on the Live
# Coding / -WaitMutex mutex. Also records any crash folders the session left.
# Exit code: always 0 (teardown must never block the chain); verdict is in JSON.

param([int]$GraceSeconds = 5)

. "$PSScriptRoot\nightrun_common.ps1"

$before = @(Get-MonolithProcesses | ForEach-Object { "$($_.Name) (pid $($_.ProcessId))" })
$killed = Stop-MonolithProcesses -WaitSeconds $GraceSeconds
$leftover = @(Get-MonolithProcesses | ForEach-Object { "$($_.Name) (pid $($_.ProcessId))" })
$portOpen = Test-NightrunPort

$resultPath = Write-ResultJson 'teardown' @{
    status = if ($leftover.Count -eq 0 -and -not $portOpen) { 'clean' } else { 'dirty' }
    processesFound = $before; killed = $killed; leftover = $leftover
    portStillOpen = $portOpen
    recentCrashes = Get-RecentCrashes 180
}

Write-Host "TEARDOWN $(if ($leftover.Count -eq 0 -and -not $portOpen) { 'CLEAN' } else { 'DIRTY' }) — $resultPath"
exit 0
