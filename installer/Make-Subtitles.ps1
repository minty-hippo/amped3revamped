<#
Makes subtitles for the game's dialogue on this PC (Options > Audio >
Subtitles), from your own copy of the game.

Downloads a speech recognition model once (whisper.cpp's base.en, 142 MB, from
the whisper.cpp project's Hugging Face page, checked against its known hash)
and runs amped3-subtitles, which finds the game's sounds in its own files,
decodes them and writes down what's said, with times. Only the dialogue is
listened to: the game keeps it apart (its own wave banks per language, and a
dialogue track per language in the movies), so music and effects are skipped
entirely. The result is
<install>\subtitles\amped3.subtitles. It all runs offline on the CPU; nothing
of the game leaves the PC. Expect it to take a while (tens of minutes on a
recent PC); stopping it is fine, running it again carries on where it was.
What it found is listed in <install>\subtitles\scan.txt.

  -InstallDir   the portable install (holds amped3.exe and game\)
  -Language     English (default), French or German: which dialogue
  -Model        base (default) or small (466 MB, more accurate, ~3x slower);
                English uses the English-only models (base.en, small.en)
  -Threads      CPU threads to use (default: up to 8)
  -Force        transcribe everything again
#>
param(
  [string]$InstallDir = '',
  [ValidateSet('English', 'French', 'German')]
  [string]$Language = 'English',
  [ValidateSet('base', 'small', 'base.en', 'small.en')]
  [string]$Model = 'base',
  [int]$Threads = 0,
  [switch]$Force,
  [string]$Tool = ''
)

$ErrorActionPreference = 'Stop'
$InstallerDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$ProjectDir = Split-Path -Parent $InstallerDir

# whisper.cpp's models (MIT), with the SHA-1 hashes its models\README.md lists.
$Models = @{
  'base.en'  = @{ File = 'ggml-base.en.bin'; Size = '142 MB'; Sha1 = '137C40403D78FD54D454DA0F9BD998F78703390C' }
  'small.en' = @{ File = 'ggml-small.en.bin'; Size = '466 MB'; Sha1 = 'DB8A495A91D927739E50B3FC1CC4C6B8F6C2D022' }
  'base'     = @{ File = 'ggml-base.bin'; Size = '142 MB'; Sha1 = '465707469FF3A37A2B9B8D8F89F2F99DE7299DAC' }
  'small'    = @{ File = 'ggml-small.bin'; Size = '466 MB'; Sha1 = '55356645C2B361A969DFD0EF2C5A50D530AFD8D5' }
}
$size = $Model -replace '\.en$', ''
$Model = if ($Language -eq 'English') { "$size.en" } else { $size }
$ModelUrlBase = 'https://huggingface.co/ggerganov/whisper.cpp/resolve/main/'

if (-not $InstallDir) {
  $default = Join-Path ([Environment]::GetFolderPath('UserProfile')) 'Games\Amped 3 Revamped'
  $InstallDir = Read-Host "Amped 3 Revamped install folder (the one with amped3.exe) [$default]"
  if (-not $InstallDir) { $InstallDir = $default }
}
$game = Join-Path $InstallDir 'game'
if (-not (Test-Path (Join-Path $game 'default.xex'))) { throw "No game in $game." }

if (-not $Tool) {
  foreach ($preset in 'win-amd64-release', 'win-amd64-relwithdebinfo') {
    $candidate = Join-Path $ProjectDir "out\build\$preset\amped3-subtitles.exe"
    if (Test-Path $candidate) { $Tool = $candidate; break }
  }
}
if (-not $Tool -or -not (Test-Path $Tool)) {
  throw 'amped3-subtitles.exe not found: build the project first (setup.ps1 builds it when it could get whisper.cpp).'
}

# The model, once.
$m = $Models[$Model]
$modelDir = Join-Path $ProjectDir 'out\models'
$modelFile = Join-Path $modelDir $m.File
if (-not (Test-Path $modelFile) -or ((Get-FileHash $modelFile -Algorithm SHA1).Hash -ne $m.Sha1)) {
  New-Item -ItemType Directory -Force -Path $modelDir | Out-Null
  Write-Host "    Downloading the speech recognition model ($($m.File), $($m.Size))"
  $partial = "$modelFile.part"
  $ProgressPreference = 'SilentlyContinue'
  Invoke-WebRequest -Uri ($ModelUrlBase + $m.File) -OutFile $partial -UseBasicParsing
  if ((Get-FileHash $partial -Algorithm SHA1).Hash -ne $m.Sha1) {
    Remove-Item $partial -Force
    throw 'The model download has unexpected contents (hash mismatch).'
  }
  Move-Item -LiteralPath $partial -Destination $modelFile -Force
}

# Who says what (speaker colours): Resemblyzer's voice encoder (Apache 2.0),
# 17 MB, at a pinned commit, checked against its hash. Optional.
$speakerFile = Join-Path $modelDir 'resemblyzer-pretrained.pt'
$speakerSha = '39373B86598FA3DA9FCDDEE6142382EFE09777E8D37DC9C0561F41F0070F134E'
$speakerUrl = 'https://raw.githubusercontent.com/resemble-ai/Resemblyzer/15d828edebe06bc72b9cabc8ef8ca5ab2cb457ce/resemblyzer/pretrained.pt'
if (-not (Test-Path $speakerFile) -or ((Get-FileHash $speakerFile -Algorithm SHA256).Hash -ne $speakerSha)) {
  Write-Host '    Downloading the voice model for speaker colours (Resemblyzer, 17 MB)'
  try {
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -Uri $speakerUrl -OutFile "$speakerFile.part" -UseBasicParsing
    if ((Get-FileHash "$speakerFile.part" -Algorithm SHA256).Hash -ne $speakerSha) { throw 'hash mismatch' }
    Move-Item -LiteralPath "$speakerFile.part" -Destination $speakerFile -Force
  } catch {
    Remove-Item "$speakerFile.part" -Force -ErrorAction SilentlyContinue
    Write-Host "    Couldn't get it ($_); subtitles without speaker colours." -ForegroundColor Yellow
  }
}

$out = Join-Path $InstallDir 'subtitles'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$toolArgs = @('--game', $game, '--out', $out, '--model', $modelFile, '--language', $Language)
if (Test-Path $speakerFile) { $toolArgs += @('--speaker-model', $speakerFile) }
if ($Threads -gt 0) { $toolArgs += @('--threads', "$Threads") }
if ($Force) { $toolArgs += '--force' }
& $Tool @toolArgs
if ($LASTEXITCODE -ne 0) { throw "Making subtitles failed (exit $LASTEXITCODE). What was found: $(Join-Path $out 'scan.txt')" }
Write-Host "    Subtitles are in $(Join-Path $out 'amped3.subtitles') (in game: Options > Audio > Subtitles)"
