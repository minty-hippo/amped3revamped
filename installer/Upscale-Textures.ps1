<#
Upscales the game's textures 4x on this PC with Real-ESRGAN, for the native
renderer (Options > Native renderer > Upscaled textures).

Downloads realesrgan-ncnn-vulkan once (the official release, checked against
its known hash; it runs on any Vulkan GPU) and runs amped3-texture-upscale,
which reads the textures from the game's own files and writes
<install>\textures\upscaled\<name>_<hash>.png. Textures you replaced by hand
(any <name>_<hash>.png elsewhere in textures\) and the game's fonts (Sharp
fonts) are left alone. Running it again only does what's missing; -Force
redoes everything.

  -InstallDir   the portable install (holds amped3.exe and game\)
  -Set          ui (menus, HUD, loading screens; default), world (characters,
                boards, levels; experimental) or all
  -Model        realesrgan-x4plus-anime (default; flat art) or
                realesrgan-x4plus (photographic)
  -Tile         tile size for GPUs short of memory (e.g. 200); 0 = automatic
#>
param(
  [string]$InstallDir = '',
  [ValidateSet('ui', 'world', 'all')]
  [string]$Set = 'ui',
  [ValidateSet('realesrgan-x4plus-anime', 'realesrgan-x4plus')]
  [string]$Model = 'realesrgan-x4plus-anime',
  [int]$Tile = 0,
  [switch]$Force,
  [string]$Tool = ''
)

$ErrorActionPreference = 'Stop'
$InstallerDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$ProjectDir = Split-Path -Parent $InstallerDir

$Esrgan = @{
  Zip = 'realesrgan-ncnn-vulkan-20220424-windows.zip'
  Url = 'https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip'
  Hash = 'ABC02804E17982A3BE33675E4D471E91EA374E65B70167ABC09E31ACB412802D'
}

if (-not $InstallDir) {
  $default = Join-Path ([Environment]::GetFolderPath('UserProfile')) 'Games\Amped 3 Revamped'
  $InstallDir = Read-Host "Amped 3 Revamped install folder (the one with amped3.exe) [$default]"
  if (-not $InstallDir) { $InstallDir = $default }
}
$game = Join-Path $InstallDir 'game'
if (-not (Test-Path (Join-Path $game 'default.xex'))) { throw "No game in $game." }

if (-not $Tool) {
  foreach ($preset in 'win-amd64-release', 'win-amd64-relwithdebinfo') {
    $candidate = Join-Path $ProjectDir "out\build\$preset\amped3-texture-upscale.exe"
    if (Test-Path $candidate) { $Tool = $candidate; break }
  }
}
if (-not $Tool -or -not (Test-Path $Tool)) {
  throw 'amped3-texture-upscale.exe not found: build the project first (setup.ps1).'
}

# Real-ESRGAN (ncnn / Vulkan), once.
$downloads = Join-Path $ProjectDir 'out\downloads'
$esrganDir = Join-Path $ProjectDir 'out\tools\realesrgan'
$esrganExe = Join-Path $esrganDir 'realesrgan-ncnn-vulkan.exe'
if (-not (Test-Path $esrganExe)) {
  New-Item -ItemType Directory -Force -Path $downloads | Out-Null
  $zip = Join-Path $downloads $Esrgan.Zip
  if (-not (Test-Path $zip) -or ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $Esrgan.Hash)) {
    Write-Host "    Downloading Real-ESRGAN ($($Esrgan.Zip), 45 MB)"
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -Uri $Esrgan.Url -OutFile $zip -UseBasicParsing
    if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $Esrgan.Hash) {
      Remove-Item $zip -Force
      throw 'The Real-ESRGAN download has unexpected contents (hash mismatch).'
    }
  }
  if (Test-Path $esrganDir) { Remove-Item $esrganDir -Recurse -Force }
  Expand-Archive -LiteralPath $zip -DestinationPath $esrganDir -Force
  if (-not (Test-Path $esrganExe)) { throw "realesrgan-ncnn-vulkan.exe missing from $($Esrgan.Zip)." }
}

$textures = Join-Path $InstallDir 'textures'
New-Item -ItemType Directory -Force -Path $textures | Out-Null
$toolArgs = @('--game', $game, '--textures', $textures, '--esrgan', $esrganExe, '--set', $Set,
              '--model', $Model)
if ($Tile -gt 0) { $toolArgs += @('--tile', "$Tile") }
if ($Force) { $toolArgs += '--force' }
& $Tool @toolArgs
if ($LASTEXITCODE -ne 0) { throw "Texture upscaling failed (exit $LASTEXITCODE)." }
Write-Host "    Upscaled textures are in $(Join-Path $textures 'upscaled')"
