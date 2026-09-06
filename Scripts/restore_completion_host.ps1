param(
    [string]$Evidence = 'D:/UnrealProjects/monolith/Saved/Completion20260906',
    [switch]$Apply
)
$ErrorActionPreference = 'Stop'
$snapshot = Get-Content -LiteralPath (Join-Path $Evidence 'baseline.json') -Raw | ConvertFrom-Json
$targetRoot = [IO.Path]::GetFullPath($snapshot.root).TrimEnd('\')
if ($targetRoot -ne 'D:\UnrealProjects\RecycleCo') { throw "Unexpected target: $targetRoot" }
if (Get-Process UnrealEditor,UnrealEditor-Cmd,RecycleCo -ErrorAction SilentlyContinue) { throw 'Close the test editor/game before restoring.' }
function Assert-InTarget([string]$Path) {
    $absolute = [IO.Path]::GetFullPath($Path)
    if (-not $absolute.StartsWith($targetRoot + '\', [StringComparison]::OrdinalIgnoreCase)) { throw "Outside target: $absolute" }
    return $absolute
}
function Get-TargetFiles([string]$Directory) {
    foreach ($item in Get-ChildItem -LiteralPath $Directory -Force) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
        if ($item.PSIsContainer) { Get-TargetFiles $item.FullName }
        else { $item }
    }
}
$known = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($relative in $snapshot.files) { [void]$known.Add($relative.Replace('\','/')) }
$newFiles = @(Get-TargetFiles $targetRoot | Where-Object {
    $relative = $_.FullName.Substring($targetRoot.Length + 1).Replace('\','/')
    -not $known.Contains($relative) -and $relative -match '^(Source|Content|Config|Saved|Binaries|Intermediate|DerivedDataCache|Build)/'
})
# All candidates are concrete files found without following plugin junctions.
foreach ($file in $newFiles) { [void](Assert-InTarget $file.FullName) }
$hashes = @{}
foreach ($entry in $snapshot.preserved.PSObject.Properties) {
    $backup = Join-Path (Join-Path $Evidence 'backup') $entry.Name
    if ((Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.Value) { throw "Backup mismatch: $($entry.Name)" }
    [void](Assert-InTarget (Join-Path $targetRoot $entry.Name))
}
if ($Apply) {
    foreach ($file in $newFiles) { Remove-Item -LiteralPath $file.FullName -Force }
    foreach ($entry in $snapshot.preserved.PSObject.Properties) {
        $destination = Assert-InTarget (Join-Path $targetRoot $entry.Name)
        New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path (Join-Path $Evidence 'backup') $entry.Name) -Destination $destination -Force
    }
}
foreach ($entry in $snapshot.preserved.PSObject.Properties) {
    $destination = Assert-InTarget (Join-Path $targetRoot $entry.Name)
    $hashes[$entry.Name] = (Test-Path -LiteralPath $destination) -and ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -eq $entry.Value)
}
$report = [ordered]@{ applied=[bool]$Apply; removed_file_count= $(if ($Apply) {$newFiles.Count} else {0}); candidates=$newFiles.Count; preserved=$hashes; all_preserved_match=($hashes.Values -notcontains $false); junction_followed=$false }
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Evidence 'cleanup.json') -Encoding UTF8
$report | ConvertTo-Json -Depth 5
if ($Apply -and -not $report.all_preserved_match) { throw 'Host restoration verification failed.' }
