# Replays an F10 frame capture with the native renderer and writes a PNG.
# See docs/native-replay.md.
#
#   .\tools\replay\replay.ps1                       # newest capture
#   .\tools\replay\replay.ps1 -Capture <frame_....txt> [-Dump] [-Draws <n>]
param(
    [string]$Capture = '',
    [switch]$Dump,
    [int]$Draws = -1
)
$ErrorActionPreference = 'Stop'
$Root = Resolve-Path (Join-Path $PSScriptRoot '..\..')

$exe = Get-ChildItem -Path (Join-Path $Root 'out\build') -Filter 'amped3-replay.exe' -Recurse -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $exe) { throw 'amped3-replay.exe not found under out\build. Build the project first (setup.ps1).' }
$logs = Join-Path $exe.DirectoryName 'logs'

if (-not $Capture) {
    $c = Get-ChildItem -Path $logs -Filter 'frame_*.txt' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $c) { throw "No frame_*.txt in $logs. Press F10 in game first." }
    $Capture = $c.FullName
}

$glslang = $null
if ($env:VULKAN_SDK -and (Test-Path (Join-Path $env:VULKAN_SDK 'Bin\glslangValidator.exe'))) {
    $glslang = Join-Path $env:VULKAN_SDK 'Bin\glslangValidator.exe'
} else {
    $cmd = Get-Command 'glslangValidator.exe' -ErrorAction SilentlyContinue
    if ($cmd) { $glslang = $cmd.Source }
}
if (-not $glslang) { throw 'glslangValidator.exe not found. Install the Vulkan SDK.' }

$argsList = @($Capture, '--shaders', (Join-Path (Split-Path $Capture) 'shaders'), '--glslang', $glslang)
if ($Dump) { $argsList += '--dump' }
if ($Draws -ge 0) { $argsList += @('--draws', $Draws) }
& $exe.FullName @argsList
exit $LASTEXITCODE
