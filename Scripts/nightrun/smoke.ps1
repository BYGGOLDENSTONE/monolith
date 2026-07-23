# HTTP smoke test against a RUNNING editor's Monolith MCP server. Talks
# directly to port 9316 (bypasses the proxy's 30s timeout). Runs the standard
# series — /health, MCP initialize, tools/list, monolith_status,
# monolith_discover, plus any -ExtraToolCalls the night's feature needs — and
# writes results/smoke.json for the verification agent.
# NOTE: tools/call returns HTTP 200 even on action failure; the real verdict
# lives in result.isError, which every step below inspects.
# Exit code: 0 = all steps passed, 1 = any failure.

param(
    [int]$Port = 0,
    # Extra tool calls as 'toolName' or 'toolName|{"json":"args"}' strings.
    [string[]]$ExtraToolCalls = @()
)

. "$PSScriptRoot\nightrun_common.ps1"
if ($Port -eq 0) { $Port = $NightrunPort }

$mcpUri = "http://127.0.0.1:$Port/mcp"
$steps = [System.Collections.Generic.List[hashtable]]::new()
$script:nextId = 0

function Invoke-McpRpc {
    param([string]$Method, $Params)
    $script:nextId++
    $body = @{ jsonrpc = '2.0'; id = $script:nextId; method = $Method }
    if ($null -ne $Params) { $body['params'] = $Params }
    return Invoke-RestMethod -Uri $mcpUri -Method Post -ContentType 'application/json' `
        -Body ($body | ConvertTo-Json -Depth 12 -Compress) -TimeoutSec 120
}

function Add-Step {
    param([string]$Name, [scriptblock]$Action)
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $detail = & $Action
        $steps.Add(@{ name = $Name; ok = $true; ms = $sw.ElapsedMilliseconds; detail = $detail })
        Write-Host "  OK  $Name ($($sw.ElapsedMilliseconds)ms)"
    } catch {
        $steps.Add(@{ name = $Name; ok = $false; ms = $sw.ElapsedMilliseconds; error = "$($_.Exception.Message)" })
        Write-Host "  FAIL $Name — $($_.Exception.Message)"
    }
}

function Invoke-ToolCall {
    # Shared assertion path for tools/call: JSON-RPC error and isError both fail the step.
    param([string]$Tool, $Arguments)
    $r = Invoke-McpRpc 'tools/call' @{ name = $Tool; arguments = $Arguments }
    if ($r.error) { throw "JSON-RPC error: $($r.error.message)" }
    if ($r.result.isError) {
        $text = if ($r.result.content) { $r.result.content[0].text } else { '(no content)' }
        throw "tool reported isError: $text"
    }
    return $r.result.content[0].text
}

Write-Host "SMOKE against port $Port"

Add-Step 'health' {
    $h = Get-NightrunHealth -Port $Port -TimeoutSec 5
    if (-not $h -or $h.status -ne 'ok') { throw "/health not ok" }
    "version=$($h.version) tools=$($h.tools_registered) uptime=$([int]$h.uptime_seconds)s"
}

Add-Step 'initialize' {
    $r = Invoke-McpRpc 'initialize' @{
        protocolVersion = '2025-03-26'; capabilities = @{}
        clientInfo = @{ name = 'nightrun-smoke'; version = '1.0' }
    }
    if ($r.error) { throw "JSON-RPC error: $($r.error.message)" }
    "server=$($r.result.serverInfo.name) $($r.result.serverInfo.version)"
}

Add-Step 'tools/list' {
    $r = Invoke-McpRpc 'tools/list' $null
    if ($r.error) { throw "JSON-RPC error: $($r.error.message)" }
    $count = @($r.result.tools).Count
    if ($count -lt 1) { throw 'no tools registered' }
    "tools=$count"
}

Add-Step 'monolith_status' {
    $text = Invoke-ToolCall 'monolith_status' @{}
    $status = $text | ConvertFrom-Json
    if (-not $status.server_running) { throw 'server_running is false' }
    "actions=$($status.total_actions) engine=$($status.engine_version) project=$($status.project_name)"
}

Add-Step 'monolith_discover' {
    $text = Invoke-ToolCall 'monolith_discover' @{}
    "chars=$($text.Length)"
}

foreach ($extra in $ExtraToolCalls) {
    $toolName, $argJson = $extra -split '\|', 2
    Add-Step "extra:$toolName" {
        $arguments = if ($argJson) { $argJson | ConvertFrom-Json } else { @{} }
        $text = Invoke-ToolCall $toolName $arguments
        "chars=$($text.Length)"
    }.GetNewClosure()
}

$failed = @($steps | Where-Object { -not $_.ok })
$green = ($failed.Count -eq 0)
$resultPath = Write-ResultJson 'smoke' @{
    status = if ($green) { 'green' } else { 'red' }
    port = $Port
    passed = ($steps.Count - $failed.Count); failed = $failed.Count
    steps = @($steps)
}

Write-Host "SMOKE $(if ($green) { 'GREEN' } else { 'RED' }) — $($steps.Count - $failed.Count)/$($steps.Count) steps — $resultPath"
exit $(if ($green) { 0 } else { 1 })
