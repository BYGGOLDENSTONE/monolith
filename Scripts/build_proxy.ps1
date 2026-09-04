param([string]$BuildDirectory, [string]$CMake, [string]$Generator = "Visual Studio 17 2022")
$ErrorActionPreference = 'Stop'
$pluginRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $pluginRoot 'Saved/ProxyBuild' }
if (-not $CMake) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vsPath) { $CMake = Join-Path $vsPath 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' }
    }
    if (-not $CMake -or -not (Test-Path -LiteralPath $CMake)) {
        $command = Get-Command cmake -ErrorAction SilentlyContinue
        if ($command) { $CMake = $command.Source }
    }
}
if (-not $CMake -or -not (Test-Path -LiteralPath $CMake)) {
    throw 'CMake not found. Install Visual Studio C++ and CMake tools, or pass -CMake <path>.'
}
& $CMake -S (Join-Path $pluginRoot 'Tools/MonolithProxy') -B $BuildDirectory -G $Generator -A x64
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)." }
& $CMake --build $BuildDirectory --config Release
if ($LASTEXITCODE -ne 0) { throw "Proxy build failed ($LASTEXITCODE)." }
$builtProxy = Join-Path $BuildDirectory 'Release/monolith_proxy.exe'
if (-not (Test-Path -LiteralPath $builtProxy)) { throw "Expected build output missing: $builtProxy" }
$binaryDirectory = Join-Path $pluginRoot 'Binaries'
New-Item -ItemType Directory -Force -Path $binaryDirectory | Out-Null
Copy-Item -LiteralPath $builtProxy -Destination (Join-Path $binaryDirectory 'monolith_proxy.exe')
Write-Output "Built $(Join-Path $binaryDirectory 'monolith_proxy.exe')"
