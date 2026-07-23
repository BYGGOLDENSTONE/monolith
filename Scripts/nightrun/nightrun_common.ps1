# Shared helpers for the nightrun unattended harness. Dot-source from sibling scripts:
#   . "$PSScriptRoot\nightrun_common.ps1"
# All scripts write machine-readable JSON results into Scripts/nightrun/results/
# (gitignored) so the verification agent can read them independently.

$ErrorActionPreference = 'Stop'

$NightrunEngine  = 'D:\UE_5.7'
$NightrunProject = 'D:\UnrealProjects\MonolithDev\MonolithDev.uproject'
$NightrunProjectDir = Split-Path -Parent $NightrunProject
$NightrunPort    = 9316
$NightrunResults = Join-Path $PSScriptRoot 'results'

if (-not (Test-Path $NightrunResults)) {
    New-Item -ItemType Directory -Path $NightrunResults -Force | Out-Null
}

function Get-MonolithProcesses {
    # Every process that can hold a lock on the MonolithDev build/editor.
    # ShaderCompileWorker/LiveCodingConsole carry no project hint in their
    # command line, so they match on name alone — acceptable on a dev machine
    # where MonolithDev is the only UE project running unattended.
    Get-CimInstance Win32_Process | Where-Object {
        ($_.Name -match '^(UnrealEditor|UnrealEditor-Cmd|UnrealBuildTool)' -and $_.CommandLine -match 'MonolithDev') -or
        ($_.Name -match '^(ShaderCompileWorker|LiveCodingConsole)')
    }
}

function Stop-MonolithProcesses {
    param([int]$WaitSeconds = 10)
    $procs = @(Get-MonolithProcesses)
    $killed = @()
    foreach ($p in $procs) {
        $killed += "$($p.Name) (pid $($p.ProcessId))"
        try { Stop-Process -Id $p.ProcessId -Force -ErrorAction Stop } catch {}
    }
    if ($killed.Count -gt 0) { Start-Sleep -Seconds $WaitSeconds }
    return $killed
}

function Test-NightrunPort {
    param([int]$Port = $NightrunPort, [int]$TimeoutMs = 2000)
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $async = $client.BeginConnect('127.0.0.1', $Port, $null, $null)
        if ($async.AsyncWaitHandle.WaitOne($TimeoutMs) -and $client.Connected) { return $true }
        return $false
    } catch { return $false } finally { $client.Dispose() }
}

function Get-NightrunHealth {
    # GET /health — the server's own liveness endpoint (richer than a TCP probe:
    # returns version, uptime and tools_registered). $null while not ready.
    param([int]$Port = $NightrunPort, [int]$TimeoutSec = 3)
    try {
        return Invoke-RestMethod -Uri "http://127.0.0.1:$Port/health" -TimeoutSec $TimeoutSec
    } catch { return $null }
}

function Write-ResultJson {
    param([string]$Name, [hashtable]$Object)
    $Object['script']    = $Name
    $Object['timestamp'] = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
    $path = Join-Path $NightrunResults "$Name.json"
    $Object | ConvertTo-Json -Depth 8 | Set-Content -Path $path -Encoding UTF8
    return $path
}

function Get-LogTail {
    param([string]$Path, [int]$Lines = 40)
    if (Test-Path $Path) { @(Get-Content $Path -Tail $Lines) } else { @() }
}

function Get-RecentCrashes {
    # Crash folders UE wrote in the last N minutes, newest first.
    param([int]$SinceMinutes = 180)
    $crashDir = Join-Path $NightrunProjectDir 'Saved\Crashes'
    if (-not (Test-Path $crashDir)) { return @() }
    $cutoff = (Get-Date).AddMinutes(-$SinceMinutes)
    @(Get-ChildItem $crashDir -Directory |
        Where-Object { $_.LastWriteTime -gt $cutoff } |
        Sort-Object LastWriteTime -Descending |
        ForEach-Object { $_.FullName })
}
