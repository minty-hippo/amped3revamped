# Translates the shaders saved by the game (capture_shaders) to GLSL and
# SPIR-V and validates them. See docs/native-shaders.md.
#
#   .\tools\shader-translate\translate.ps1                 # newest build's logs\shaders
#   .\tools\shader-translate\translate.ps1 -Shaders <folder> [-Out <folder>] [-Blobs]
#
# Needs glslangValidator.exe and spirv-val.exe from the Vulkan SDK (found via
# %VULKAN_SDK%\Bin or PATH). Without them it only translates.
param(
    [string]$Shaders = '',
    [string]$Out = '',
    # Also translate the CreateVertexShader / CreatePixelShader blobs (vs_*.bin,
    # ps_*.bin) with placeholder vertex fetches. By default only the microcode
    # as drawn (ucode_*.bin) is translated.
    [switch]$Blobs
)
$ErrorActionPreference = 'Stop'
$Root = Resolve-Path (Join-Path $PSScriptRoot '..\..')

$exe = Get-ChildItem -Path (Join-Path $Root 'out\build') -Filter 'amped3-shader-translate.exe' -Recurse -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $exe) { throw 'amped3-shader-translate.exe not found under out\build. Build the project first (setup.ps1).' }

if (-not $Shaders) { $Shaders = Join-Path $exe.DirectoryName 'logs\shaders' }
if (-not (Test-Path $Shaders)) { throw "No shader folder at $Shaders. Run the game with capture_shaders on first." }
if (-not $Out) { $Out = Join-Path $Shaders 'translated' }

function Find-Tool([string]$name) {
    if ($env:VULKAN_SDK) {
        $p = Join-Path $env:VULKAN_SDK "Bin\$name"
        if (Test-Path $p) { return $p }
    }
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return $null
}
$glslang = Find-Tool 'glslangValidator.exe'
$spirvVal = Find-Tool 'spirv-val.exe'

$ucodeCount = @(Get-ChildItem -Path $Shaders -Filter 'ucode_*.bin').Count
if ($ucodeCount -eq 0 -and -not $Blobs) {
    throw "No ucode_*.bin in $Shaders. Play a little with capture_shaders on (this build saves them at draw time)."
}

$argsList = @($Shaders, '-o', $Out)
if ($Blobs) { $argsList += '--blobs' }
if ($glslang) { $argsList += @('--glslang', $glslang) } else { Write-Warning 'glslangValidator.exe not found (install the Vulkan SDK); translating only.' }
if ($spirvVal) { $argsList += @('--spirv-val', $spirvVal) } else { Write-Warning 'spirv-val.exe not found; skipping SPIR-V validation.' }

Write-Host "Translating $ucodeCount drawn shader(s) from $Shaders"
& $exe.FullName @argsList
$rc = $LASTEXITCODE
Write-Host "Results: $(Join-Path $Out 'summary.txt')"
exit $rc
