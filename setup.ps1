<#
.SYNOPSIS
  Builds the ReXGlue SDK and the Amped 3 PC port in one go.

.DESCRIPTION
  1. Checks for Visual Studio (C++ workload), Clang 20+, Git, CMake and Ninja.
  2. Clones the ReXGlue SDK (pinned commit), applies the two fixes in
     patches\rexglue, builds it and installs it.
  3. Runs `rexglue init` against your extracted game, merges in this project's
     files, and builds amped3.exe (the first build recompiles the game's code,
     which takes a while).

  Every step logs to .\logs\. If something fails, send the newest log.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\setup.ps1
  powershell -ExecutionPolicy Bypass -File .\setup.ps1 -Config RelWithDebInfo -Run
#>
[CmdletBinding()]
param(
  # Extracted Amped 3 folder (the one with default.xex). Default: the first
  # folder holding default.xex inside this project folder, else next to it.
  [string]$GameDir = '',
  # Where the SDK is cloned. Shared by any other ReXGlue projects you make.
  # Default: "rexglue-sdk" next to this project folder.
  [string]$SdkDir = '',
  # Release for playing, RelWithDebInfo for chasing crashes, Debug if desperate.
  [ValidateSet('Release', 'RelWithDebInfo', 'Debug')]
  [string]$Config = 'Release',
  # Skip rebuilding the SDK if it is already installed.
  [switch]$SkipSdk,
  # Launch the game after a successful build.
  [switch]$Run,
  # Launch the game repeatedly; each time it crashes calling a function the
  # recompiler missed, register that function and rebuild.
  [switch]$FixCrashes,
  [int]$MaxFixes = 25,
  # How long a test run must survive to count as past startup.
  [int]$StartupSeconds = 45
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# The SDK commit this project was written and checked against (main, 2026-08-21).
$SdkRepo = 'https://github.com/rexglue/rexglue-sdk.git'
$SdkCommit = 'c94f5ebdcb3c9d1a460ca48e04f9758448f8d518'

# Windows PowerShell 5.1 can leave $PSScriptRoot empty (in parameter defaults,
# and in some launch modes), so work the folder out with fallbacks here.
$ProjectDir = if ($PSScriptRoot) { $PSScriptRoot }
              elseif ($MyInvocation.MyCommand.Path) { Split-Path -Parent $MyInvocation.MyCommand.Path }
              else { (Get-Location).Path }
$Parent = Split-Path -Parent $ProjectDir
if (-not $GameDir) {
  # Any folder one level down in the project, then beside it (no particular
  # names needed).
  $candidates = @()
  $candidates += Get-ChildItem -LiteralPath $ProjectDir -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -notin 'out', 'generated', 'logs', 'src' } | ForEach-Object FullName
  $candidates += Get-ChildItem -LiteralPath $Parent -Directory -ErrorAction SilentlyContinue |
    ForEach-Object FullName
  $GameDir = $candidates | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'default.xex') } |
    Select-Object -First 1
  if (-not $GameDir) {
    Write-Host "`nERROR: Couldn't find Amped 3's default.xex in or next to $ProjectDir." -ForegroundColor Red
    Write-Host 'Pass the folder explicitly:  .\setup.ps1 -GameDir "<folder with default.xex>"'
    exit 1
  }
}
if (-not $SdkDir) { $SdkDir = Join-Path $Parent 'rexglue-sdk' }
Set-Location $ProjectDir
$LogDir = Join-Path $ProjectDir 'logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'

function Step($text) { Write-Host "`n==> $text" -ForegroundColor Cyan }
function Fail($text) { Write-Host "`nERROR: $text" -ForegroundColor Red; exit 1 }

# Runs a native command, tees output to a log, fails on a non-zero exit code.
# With -AllowFail, returns $true/$false instead of stopping the script.
function Invoke-Logged([string]$Name, [scriptblock]$Command, [switch]$AllowFail) {
  $log = Join-Path $LogDir "$Stamp-$Name.log"
  $script:LastLog = $log
  Write-Host "    (log: $log)" -ForegroundColor DarkGray
  # Native tools write progress to stderr; under Windows PowerShell 5.1 that
  # would be a terminating error with 'Stop', so judge by exit code instead.
  $ErrorActionPreference = 'Continue'
  $global:LASTEXITCODE = 0
  & $Command 2>&1 | Tee-Object -FilePath $log | ForEach-Object { Write-Host "    $_" }
  if ($LASTEXITCODE -ne 0) {
    if ($AllowFail) { return $false }
    Fail "$Name failed (exit $LASTEXITCODE). See $log"
  }
  if ($AllowFail) { return $true }
}

function Get-ClangMajor([string]$clangExe) {
  if (-not (Test-Path $clangExe)) { return 0 }
  $line = (& $clangExe --version 2>$null | Select-Object -First 1)
  if ($line -match 'version (\d+)') { return [int]$Matches[1] }
  return 0
}

# ---------------------------------------------------------------------------
Step 'Checking the game folder'
$GameDir = [IO.Path]::GetFullPath($GameDir)
$Xex = Join-Path $GameDir 'default.xex'
if (-not (Test-Path $Xex)) { Fail "No default.xex in '$GameDir'. Pass -GameDir <folder>." }
Write-Host "    $Xex"

# ---------------------------------------------------------------------------
Step 'Loading the Visual Studio build environment'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
  Fail ('Visual Studio not found. Install Visual Studio 2022 (Community is fine) with the ' +
        '"Desktop development with C++" workload, plus the individual components ' +
        '"C++ Clang Compiler for Windows" and "MSBuild support for LLVM (clang-cl) toolset".')
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { Fail 'Visual Studio is installed but the "Desktop development with C++" workload is missing.' }
Write-Host "    $vs"
& (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
Set-Location $ProjectDir

# ReXGlue needs Clang 20 or newer. Prefer Visual Studio's, fall back to a
# standalone LLVM install (winget install LLVM.LLVM).
$clangCandidates = @(
  (Join-Path $env:VCINSTALLDIR 'Tools\Llvm\x64\bin'),
  (Join-Path $env:ProgramFiles 'LLVM\bin')
)
$clangBin = $null
foreach ($dir in $clangCandidates) {
  $major = Get-ClangMajor (Join-Path $dir 'clang.exe')
  Write-Host ("    clang in {0}: {1}" -f $dir, $(if ($major) { "v$major" } else { 'not found' }))
  if ($major -ge 20 -and -not $clangBin) { $clangBin = $dir }
}
if (-not $clangBin) {
  Fail ('Clang 20+ not found. Either update Visual Studio and add the "C++ Clang Compiler for ' +
        'Windows" component, or run:  winget install LLVM.LLVM   then re-run this script.')
}
$env:PATH = "$clangBin;$env:PATH"
Write-Host "    using clang from $clangBin"

foreach ($tool in 'git', 'cmake', 'ninja') {
  if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
    if ($tool -eq 'git') { Fail 'Git not found. Install it from https://git-scm.com or: winget install Git.Git' }
    Fail "$tool not found (it normally comes with Visual Studio's C++ workload)."
  }
}

# ---------------------------------------------------------------------------
$SdkDir = [IO.Path]::GetFullPath($SdkDir)

# Real symlinks need Developer Mode (or admin). Without it, asking Git for them
# makes checkouts fail with "unable to create symlink: Permission denied".
$devMode = $false
try {
  $unlock = Get-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock' -ErrorAction Stop
  if ($unlock.PSObject.Properties['AllowDevelopmentWithoutDevLicense']) {
    $devMode = $unlock.AllowDevelopmentWithoutDevLicense -eq 1
  }
} catch {
  # Key missing: Developer Mode has never been turned on.
}
$SymlinksOpt = if ($devMode) { 'core.symlinks=true' } else { 'core.symlinks=false' }
$SdkInstall = Join-Path $SdkDir 'out\install\win-amd64'
$Rexglue = Join-Path $SdkInstall 'bin\rexglue.exe'

if (-not $SkipSdk -or -not (Test-Path $Rexglue)) {
  Step "Getting the ReXGlue SDK ($($SdkCommit.Substring(0, 7)))"
  if (-not (Test-Path (Join-Path $SdkDir '.git'))) {
    # Some submodules contain symlinks (see $SymlinksOpt below).
    # Developer Mode on; otherwise they are fixed up below.
    Invoke-Logged 'sdk-clone' { git -c $SymlinksOpt clone $SdkRepo $SdkDir }
  }
  Push-Location $SdkDir
  try {
    $head = (git rev-parse HEAD).Trim()
    if ($head -ne $SdkCommit) {
      $dirty = git status --porcelain --untracked-files=no
      if ($dirty) {
        Write-Host '    Resetting local SDK changes (patches are re-applied below)' -ForegroundColor Yellow
        git checkout -- . 2>&1 | Out-Null
      }
      # The patches are gone (or were made for another commit): re-apply them.
      Remove-Item (Join-Path $SdkDir '.amped3-patches') -ErrorAction SilentlyContinue
      Invoke-Logged 'sdk-fetch' { git fetch origin }
      Invoke-Logged 'sdk-checkout' { git -c advice.detachedHead=false checkout $SdkCommit }
    }
    # --force redoes checkouts a previous run left half-finished.
    Invoke-Logged 'sdk-submodules' { git -c $SymlinksOpt submodule update --init --recursive --force }

    # Without real symlinks, Git writes each link as a small text file holding
    # the target path. Replace those with copies of the target (file or folder).
    # Links that point at other links are resolved over a few passes.
    $fixed = 0
    $pending = @{}
    # List symlinks per submodule in PowerShell (no shell pipeline: Windows
    # PowerShell 5.1 mangles quotes in arguments passed to native programs).
    $subPaths = @(git submodule foreach --quiet --recursive 'echo $displaypath')
    foreach ($sub in $subPaths) {
      if (-not $sub) { continue }
      $subDir = Join-Path $SdkDir $sub.Trim()
      foreach ($entry in @(git -C $subDir ls-files -s)) {
        # "<mode> <sha> <stage>`t<path>"; mode 120000 = symlink
        if ($entry -notmatch '^120000 \S+ \d+\t(.+)$') { continue }
        $path = [IO.Path]::GetFullPath((Join-Path $subDir $Matches[1]))
        $item = Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
        if ($item -and -not $item.PSIsContainer -and
            -not ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
          $pending[$path] = $true
        }
      }
    }
    Write-Host "    $($pending.Count) symlink placeholder(s) to replace"
    for ($pass = 0; $pass -lt 4 -and $pending.Count; $pass++) {
      foreach ($path in @($pending.Keys)) {
        $target = (Get-Content -LiteralPath $path -Raw).Trim()
        $source = [IO.Path]::GetFullPath((Join-Path (Split-Path $path) $target))
        if ($pending.ContainsKey($source)) { continue }  # resolve that one first
        if (Test-Path -LiteralPath $source -PathType Leaf) {
          Copy-Item -LiteralPath $source -Destination $path -Force
        } elseif (Test-Path -LiteralPath $source -PathType Container) {
          Remove-Item -LiteralPath $path -Force
          Copy-Item -LiteralPath $source -Destination $path -Recurse -Force
        } else {
          # Points outside the checkout (or nowhere); leave it.
        }
        $pending.Remove($path)
        $fixed++
      }
    }
    if ($fixed) { Write-Host "    replaced $fixed symlink placeholder(s) with copies" }

    Step 'Applying SDK fixes'
    $ErrorActionPreference = 'Continue'
    # Patches replaced by later versions. Unzipping a new copy of the project
    # over an old one leaves these behind, so remove them here.
    foreach ($old in @('0005-vulkan-intel-default-fsi.patch')) {
      $oldPath = Join-Path $ProjectDir "patches\rexglue\$old"
      if (Test-Path $oldPath) {
        Remove-Item $oldPath
        Write-Host "    removed obsolete $old"
      }
    }
    $patchFiles = @(Get-ChildItem (Join-Path $ProjectDir 'patches\rexglue\*.patch') | Sort-Object Name)
    # A patch that was changed or removed since the last run can't be detected
    # by "already applied", so remember the exact set and start from a clean
    # tree whenever it differs.
    $stampFile = Join-Path $SdkDir '.amped3-patches'
    $wanted = ($patchFiles | ForEach-Object { "$($_.Name) $((Get-FileHash $_.FullName -Algorithm SHA256).Hash)" }) -join "`n"
    $applied = if (Test-Path $stampFile) { (Get-Content $stampFile -Raw).Trim() } else { $null }
    if ($applied -ne $wanted) {
      $dirty = git status --porcelain --untracked-files=no --ignore-submodules=all
      if ($dirty) {
        Write-Host '    SDK fixes changed; resetting the SDK source before re-applying them' -ForegroundColor Yellow
        git checkout -- . 2>&1 | Out-Null
      }
      # Files the patches add are untracked, so checkout leaves them; remove
      # them too or re-applying those patches fails with "already exists".
      foreach ($patch in $patchFiles) {
        $lines = Get-Content $patch.FullName
        for ($i = 0; $i -lt $lines.Count - 1; $i++) {
          if ($lines[$i] -eq '--- /dev/null' -and $lines[$i + 1] -match '^\+\+\+ b/(.+)$') {
            $added = Join-Path $SdkDir ($Matches[1] -replace '/', '\')
            if (Test-Path $added) { Remove-Item $added -Force }
          }
        }
      }
      Remove-Item $stampFile -ErrorAction SilentlyContinue
    }
    if ($applied -eq $wanted) {
      # The stamp is written only after every patch applied, so the tree
      # already has exactly this set. (Checking each patch with a reverse
      # apply doesn't work here: a later patch that edits the same lines
      # makes an earlier one look unapplied.)
      Write-Host "    all $($patchFiles.Count) SDK fixes already applied"
    } else {
      foreach ($patch in $patchFiles) {
        git apply --check --reverse $patch.FullName 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) { Write-Host "    already applied: $($patch.Name)"; continue }
        git apply $patch.FullName 2>&1 | ForEach-Object { Write-Host "    $_" }
        if ($LASTEXITCODE -ne 0) {
          # Leave no stamp, so the next run starts again from a clean tree.
          Remove-Item $stampFile -ErrorAction SilentlyContinue
          Fail "Could not apply $($patch.Name) to the SDK."
        }
        Write-Host "    applied: $($patch.Name)"
      }
      Set-Content -Path $stampFile -Value $wanted -Encoding ascii
    }
    $ErrorActionPreference = 'Stop'

    Step "Building the SDK ($Config) - the first time takes a while"
    # Vulkan is off by default on Windows; build both so the port can offer either
    # (Options -> Graphics -> Graphics API).
    Invoke-Logged 'sdk-configure' { cmake --preset win-amd64 -DREXGLUE_USE_D3D12=ON -DREXGLUE_USE_VULKAN=ON }
    # The rexglue codegen tool is installed for Release only (other configs map
    # to it), so Release is always installed; then the chosen config's libraries.
    Invoke-Logged 'sdk-build-release' { cmake --build out/build/win-amd64 --config Release --target install }
    if ($Config -ne 'Release') {
      Invoke-Logged 'sdk-build' { cmake --build out/build/win-amd64 --config $Config --target install }
    }
  } finally {
    Pop-Location
  }
}
if (-not (Test-Path $Rexglue)) { Fail "SDK install did not produce $Rexglue" }

# ---------------------------------------------------------------------------
Step 'Setting up the Amped 3 project'
$Manifest = Join-Path $ProjectDir 'amped3_manifest.toml'
# Recompiler hints, loaded in this order (later files win):
#   amped3_functions_scan.toml  static scan of default.xex for pointer-only functions
#   amped3_functions_auto.toml  entry points setup.ps1 adds from codegen errors/crashes
#   amped3_d3d_names.toml       names for the mapped Direct3D functions (hooks use them)
#   amped3_functions.toml       yours
$AutoHintsName = 'amped3_functions_auto.toml'
$ScanHintsName = 'amped3_functions_scan.toml'
$NamesHintsName = 'amped3_d3d_names.toml'
$AutoHints = Join-Path $ProjectDir $AutoHintsName
$IncludeList = @($ScanHintsName, $AutoHintsName)
$IncludeList += $NamesHintsName
$IncludeList += 'amped3_functions.toml'
$Includes = 'includes = [' + (($IncludeList | ForEach-Object { '"' + $_ + '"' }) -join ', ') + ']'
if (-not (Test-Path (Join-Path $ProjectDir $ScanHintsName))) {
  Set-Content -Path (Join-Path $ProjectDir $ScanHintsName) -Value '[functions]'
}
if (-not (Test-Path $AutoHints)) {
  Set-Content -Path $AutoHints -Value @(
    '# Written by setup.ps1. Function entry points the recompiler could not find on'
    '# its own (branch targets outside every known function). No size given, so the'
    '# analysis works out where each one ends. Safe to delete: it gets rebuilt.'
    ''
    '[functions]'
  )
}
# Entry points already found on earlier machines (codegen rounds and -FixCrashes
# runs). Seeded so a fresh checkout builds an exe that doesn't crash on the
# first pointer call into one of them. Added only when missing, so an existing
# file (and with it the generated code) is left alone.
$KnownHints = [ordered]@{
  '82748350' = 'branch from 0x82751E44'
  '8274BD28' = 'branch from 0x827503A4'
  '827FDBC8' = 'branch from 0x8283A160'
  '82831090' = 'called through a pointer at runtime'
  '826C8FE8' = 'called through a pointer at runtime'
  '821E6F20' = 'called through a pointer at runtime'
  '82749450' = 'called through a pointer at runtime'
}
$autoText = Get-Content $AutoHints -Raw
foreach ($k in $KnownHints.Keys) {
  if ($autoText -notmatch "0x$k") {
    Add-Content -Path $AutoHints -Value ("""0x{0}"" = {{}}  # {1}" -f $k, $KnownHints[$k])
  }
}
if (-not (Test-Path $Manifest)) {
  # init keeps src\main.cpp and src\amped3_app.h (ours) and writes the
  # manifest, CMake presets and generated\rexglue.cmake.
  Invoke-Logged 'init' {
    & $Rexglue init --project-name amped3 --xex-path $Xex --game-root $GameDir --project-root $ProjectDir
  }
  # Include our recompiler hints file in the entrypoint section.
  $text = Get-Content $Manifest -Raw
  $text = ([regex]'includes = \[\]').Replace($text, $Includes, 1)
  Set-Content -Path $Manifest -Value $text -NoNewline
}
# Keep the entrypoint's includes line (the one naming amped3_functions.toml) in
# step with the current list. Only rewritten when it differs, so the manifest's
# timestamp (and with it a full recompile) changes only when the list does.
$text = Get-Content $Manifest -Raw
$newText = ([regex]'includes = \[[^\]\r\n]*"amped3_functions\.toml"\]').Replace($text, $Includes, 1)
if ($newText -ne $text) {
  Set-Content -Path $Manifest -Value $newText -NoNewline
}
Copy-Item (Join-Path $ProjectDir 'overlay\CMakeLists.txt') (Join-Path $ProjectDir 'CMakeLists.txt') -Force

# ---------------------------------------------------------------------------
$Preset = switch ($Config) {
  'Release' { 'win-amd64-release' }
  'RelWithDebInfo' { 'win-amd64-relwithdebinfo' }
  'Debug' { 'win-amd64-debug' }
}
$ConfigureArgs = @("-DCMAKE_PREFIX_PATH=$SdkInstall")
# The native renderer compiles its shaders in the game with glslang; the SDK
# checkout has a copy (thirdparty/glslang, a submodule).
$GlslangDir = Join-Path $SdkDir 'thirdparty\glslang'
if (Test-Path (Join-Path $GlslangDir 'CMakeLists.txt')) {
  $ConfigureArgs += "-DAMPED3_GLSLANG_DIR=$GlslangDir"
  $ConfigureArgs += "-DAMPED3_SDK_SOURCE_DIR=$SdkDir"
} else {
  Write-Host "    glslang not found at $GlslangDir; the native renderer preview will be unavailable" -ForegroundColor Yellow
}
$Exe = Join-Path $ProjectDir "out\build\$Preset\amped3.exe"
$RunLogDir = Join-Path (Split-Path $Exe) 'logs'

# Upscaler runtimes for the native renderer (Display > Image quality >
# Upscaler): Intel XeSS and AMD FidelityFX (FSR), loaded by the game at run
# time from next to amped3.exe. Downloaded once (official release zips, into
# out\downloads), checked against known hashes; only a warning when that fails
# (the upscaler options then report the missing file and the game draws
# without them).
$UpscalerRuntimes = @(
  @{ Name = 'libxess.dll'; Entry = 'bin/libxess.dll'
     Zip = 'XeSS_SDK_3.0.2.zip'
     Url = 'https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip'
     ZipHash = '88B8A373F30E33F3558A77A93E634F11B8132FC3047EA1A8EDEEAD32B8471990'
     Hash = '251659DD84A3E84DE67C886A4186E01F3ECA49B00641906FE38BB6B807E5D5B7'
     License = 'LICENSE.txt'; LicenseAs = 'XeSS-LICENSE.txt' },
  @{ Name = 'amd_fidelityfx_vk.dll'; Entry = 'PrebuiltSignedDLL/amd_fidelityfx_vk.dll'
     Zip = 'FidelityFX-SDK-v1.1.4.zip'
     Url = 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v1.1.4/FidelityFX-SDK-v1.1.4.zip'
     ZipHash = '0216556BFB0E243CEC30004A2A98D38F4E3F7406CB7938E3C1B85C758E95D952'
     Hash = 'A1624CC4238FEF046F30C4D80CE3F47BE63FC5F5373F49E3EE9EDB9960F54C78'
     License = 'sdk/LICENSE.txt'; LicenseAs = 'FidelityFX-LICENSE.txt' }
)
function Install-UpscalerRuntimes {
  $dest = Split-Path $Exe
  $cache = Join-Path $ProjectDir 'out\downloads'
  foreach ($r in $UpscalerRuntimes) {
    $target = Join-Path $dest $r.Name
    if ((Test-Path $target) -and ((Get-FileHash $target -Algorithm SHA256).Hash -eq $r.Hash)) { continue }
    try {
      New-Item -ItemType Directory -Force -Path $cache | Out-Null
      $zip = Join-Path $cache $r.Zip
      if (-not (Test-Path $zip) -or ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $r.ZipHash)) {
        Step "Downloading $($r.Zip) for $($r.Name) (upscaler runtime)"
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $r.Url -OutFile $zip -UseBasicParsing
        if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $r.ZipHash) {
          Remove-Item $zip -Force
          throw "unexpected contents (hash mismatch)"
        }
      }
      Add-Type -AssemblyName System.IO.Compression.FileSystem
      $archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
      try {
        foreach ($pair in @(@($r.Entry, $r.Name), @($r.License, $r.LicenseAs))) {
          $entry = $archive.GetEntry($pair[0])
          if (-not $entry) { throw "$($pair[0]) missing from $($r.Zip)" }
          [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $dest $pair[1]), $true)
        }
      } finally { $archive.Dispose() }
      if ((Get-FileHash $target -Algorithm SHA256).Hash -ne $r.Hash) { throw "$($r.Name) hash mismatch" }
      Write-Host "    $($r.Name) installed next to amped3.exe"
    } catch {
      Write-Host "    Could not install $($r.Name): $_ (that upscaler stays unavailable)" -ForegroundColor Yellow
    }
  }
}

# Intel's XeSS headers, needed to build the XeSS upscaler: not part of this
# project's sources (Intel's license), so taken from the same official XeSS
# SDK zip as libxess.dll, into third_party\xess (ignored by git). Without
# them the game builds with FSR only.
function Install-XessHeaders {
  $r = $UpscalerRuntimes | Where-Object { $_.Name -eq 'libxess.dll' } | Select-Object -First 1
  $incDir = Join-Path $ProjectDir 'third_party\xess\inc\xess'
  $headers = @('xess.h', 'xess_vk.h')
  if (-not ($headers | Where-Object { -not (Test-Path (Join-Path $incDir $_)) })) { return }
  try {
    $cache = Join-Path $ProjectDir 'out\downloads'
    New-Item -ItemType Directory -Force -Path $cache | Out-Null
    $zip = Join-Path $cache $r.Zip
    if (-not (Test-Path $zip) -or ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $r.ZipHash)) {
      Step "Downloading $($r.Zip) (XeSS headers and runtime)"
      $ProgressPreference = 'SilentlyContinue'
      Invoke-WebRequest -Uri $r.Url -OutFile $zip -UseBasicParsing
      if ((Get-FileHash $zip -Algorithm SHA256).Hash -ne $r.ZipHash) {
        Remove-Item $zip -Force
        throw "unexpected contents (hash mismatch)"
      }
    }
    New-Item -ItemType Directory -Force -Path $incDir | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
    try {
      foreach ($h in $headers) {
        $entry = $archive.GetEntry("inc/xess/$h")
        if (-not $entry) { throw "inc/xess/$h missing from $($r.Zip)" }
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $incDir $h), $true)
      }
      $lic = $archive.GetEntry('LICENSE.txt')
      if ($lic) {
        [System.IO.Compression.ZipFileExtensions]::ExtractToFile($lic, (Join-Path $ProjectDir 'third_party\xess\LICENSE.txt'), $true)
      }
    } finally { $archive.Dispose() }
    Write-Host '    XeSS headers ready (third_party\xess)'
  } catch {
    Write-Host "    Could not get the XeSS headers: $_ (building without XeSS; FSR still works)" -ForegroundColor Yellow
  }
}

function Add-FunctionHint([string]$Address, [string]$Note) {
  Add-Content -Path $AutoHints -Value ("""0x{0}"" = {{}}  # {1}" -f $Address.ToUpper(), $Note)
}

# Recompile (adding entry points codegen reports as missing), then configure
# again so CMake picks up the generated C++, then build.
# A build folder configured from another place (the project folder was moved
# or renamed): CMake refuses it, so start that build folder's configuration
# over. Everything recompiles once (its paths are all absolute).
function Reset-MovedBuild {
  $buildDir = Join-Path $ProjectDir "out\build\$Preset"
  $cache = Join-Path $buildDir 'CMakeCache.txt'
  if (-not (Test-Path $cache)) { return }
  $line = Select-String -LiteralPath $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$' | Select-Object -First 1
  if (-not $line) { return }
  $was = [IO.Path]::GetFullPath($line.Matches[0].Groups[1].Value.Trim()).TrimEnd('\')
  $now = [IO.Path]::GetFullPath($ProjectDir).TrimEnd('\')
  if ($was -ieq $now) { return }
  Write-Host "    The project was set up in $was; starting $Preset's build configuration over (a full rebuild)." -ForegroundColor Yellow
  Remove-Item -LiteralPath $cache -Force
  $files = Join-Path $buildDir 'CMakeFiles'
  if (Test-Path $files) { Remove-Item -LiteralPath $files -Recurse -Force }
  foreach ($f in 'build.ninja', '.ninja_deps', '.ninja_log', 'cmake_install.cmake') {
    $p = Join-Path $buildDir $f
    if (Test-Path $p) { Remove-Item -LiteralPath $p -Force }
  }
}

function Build-Game([string]$Tag) {
  Step "Recompiling the game's code ($Preset)"
  Reset-MovedBuild
  Invoke-Logged "$Tag-configure" { cmake --preset $Preset @ConfigureArgs }
  $generated = $false
  for ($round = 1; $round -le 10; $round++) {
    if (Invoke-Logged "$Tag-codegen-$round" { cmake --build --preset $Preset --target amped3_codegen } -AllowFail) {
      $generated = $true; break
    }
    # Branches into code that isn't inside any known function.
    $known = Get-Content $AutoHints -Raw
    $new = @(Select-String -LiteralPath $script:LastLog `
        -Pattern '0x([0-9A-Fa-f]{8}) from 0x([0-9A-Fa-f]{8}): bl? 0x[0-9A-Fa-f]{8} from 0x[0-9A-Fa-f]{8}.*target not in any function' |
      ForEach-Object { [pscustomobject]@{ Target = $_.Matches[0].Groups[1].Value.ToUpper(); Site = $_.Matches[0].Groups[2].Value.ToUpper() } } |
      Where-Object { $known -notmatch "0x$($_.Target)" } |
      Sort-Object Target -Unique)
    if (-not $new.Count) { Fail "Recompiling failed. See $($script:LastLog)" }
    Step "Round ${round}: adding $($new.Count) function entry point(s) the recompiler missed"
    foreach ($n in $new) {
      Write-Host "    0x$($n.Target)  (branched to from 0x$($n.Site))"
      Add-FunctionHint $n.Target "branch from 0x$($n.Site)"
    }
  }
  if (-not $generated) { Fail "Recompiling still failing after 10 rounds. See $($script:LastLog)" }

  $sources = Join-Path $ProjectDir 'generated\default\sources.cmake'
  if (-not (Test-Path $sources)) { Fail "Recompiler finished but $sources is missing." }
  Step "Building amped3.exe ($Preset) - compiling the recompiled game takes a while"
  Invoke-Logged "$Tag-reconfigure" { cmake --preset $Preset @ConfigureArgs }
  Invoke-Logged "$Tag-build" { cmake --build --preset $Preset }
  if (-not (Test-Path $Exe)) { Fail "Build finished but $Exe is missing." }
  Install-UpscalerRuntimes
}

# Launches the game and watches its log. Returns the guest address of a
# "call to unregistered function" crash, or $null if the game was still
# running after $Seconds (or exited some other way).
function Test-GameStartup([int]$Seconds) {
  $started = Get-Date
  $proc = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Exe) -PassThru `
    -ArgumentList "--game_data_root=`"$GameDir`""
  $null = $proc.Handle  # keep a handle so ExitCode is readable after exit
  $script:LastProc = $proc
  $script:LastStart = $started
  $pattern = 'Call to invalid or unregistered function at guest address 0x([0-9A-Fa-f]{8})'
  $script:RunLog = $null
  while (((Get-Date) - $started).TotalSeconds -lt $Seconds) {
    Start-Sleep -Milliseconds 500
    $log = Get-ChildItem -LiteralPath $RunLogDir -Filter 'amped3_*.log' -ErrorAction SilentlyContinue |
      Where-Object { $_.LastWriteTime -ge $started.AddSeconds(-1) } |
      Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($log) {
      $script:RunLog = $log.FullName
      $hit = Select-String -LiteralPath $log.FullName -Pattern $pattern | Select-Object -First 1
      if ($hit) {
        if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
        return $hit.Matches[0].Groups[1].Value.ToUpper()
      }
    }
    if ($proc.HasExited) { return $null }
  }
  return $null
}

Install-XessHeaders
Build-Game 'build'

# Lets the build folder's exe find the game when started directly (or
# restarted from the options menu): amped3 reads the game folder from
# game.path next to it when there's no game\ folder there.
[IO.File]::WriteAllText((Join-Path (Split-Path $Exe) 'game.path'), "$GameDir`n")

Write-Host "`nBuilt: $Exe" -ForegroundColor Green
Write-Host 'In game: Back+Start (or F1) opens the options menu. F3 debug overlay, F4 every setting, ` console.'
Write-Host "Runtime logs go to $RunLogDir."

if ($FixCrashes) {
  # The game calls code through function pointers (callbacks, thread entries,
  # vtables) that static analysis can't always see. Each such call crashes
  # with "unregistered function"; register the address and rebuild.
  for ($fix = 1; $fix -le $MaxFixes; $fix++) {
    Step "Test run $fix - watching for a missed function (up to $StartupSeconds s)"
    $addr = Test-GameStartup $StartupSeconds
    if (-not $addr) {
      $proc = Get-Process -Name amped3 -ErrorAction SilentlyContinue
      if ($proc) {
        Write-Host "`nThe game is still running after $StartupSeconds s with no missed-function crash." -ForegroundColor Green
        Write-Host 'Leaving it open. If it crashes later, run with -FixCrashes again.'
      } else {
        Write-Host "`nThe game exited, but not with a missed-function crash." -ForegroundColor Yellow
        # Exit codes come back as signed 32-bit; compare them as hex text.
        $hex = '?'
        try { $hex = '{0:X8}' -f [BitConverter]::ToUInt32([BitConverter]::GetBytes([int32]$script:LastProc.ExitCode), 0) } catch {}
        $codeName = switch ($hex) {
          '00000000' { 'normal exit (exit(0))' }
          '00000001' { 'exit(1)' }
          'C0000005' { 'access violation' }
          'C0000409' { 'fail-fast / stack buffer overrun (bypasses crash handlers)' }
          'C00000FD' { 'stack overflow' }
          'C000001D' { 'illegal instruction' }
          '80000003' { 'breakpoint / debug break' }
          'C0000374' { 'heap corruption' }
          'C000008E' { 'floating-point divide by zero (FP exceptions unmasked)' }
          'C000008F' { 'floating-point inexact result (FP exceptions unmasked)' }
          'C0000090' { 'floating-point invalid operation (FP exceptions unmasked)' }
          'C0000091' { 'floating-point overflow (FP exceptions unmasked)' }
          'C0000093' { 'floating-point underflow (FP exceptions unmasked)' }
          default { 'unknown' }
        }
        Write-Host ("Exit code: 0x{0} - {1}" -f $hex, $codeName) -ForegroundColor Yellow

        # Windows Error Reporting records crashes the process couldn't report itself.
        try {
          Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $script:LastStart } -ErrorAction Stop |
            Where-Object { $_.ProviderName -in 'Application Error', 'Windows Error Reporting' -and $_.Message -match 'amped3' } |
            Select-Object -First 2 | ForEach-Object {
              Write-Host "Windows event ($($_.ProviderName)):" -ForegroundColor Yellow
              $_.Message -split "`r?`n" | Where-Object { $_ } | Select-Object -First 14 |
                ForEach-Object { Write-Host "    $_" }
            }
        } catch {}

        $report = Get-ChildItem -LiteralPath $RunLogDir -Filter 'amped3_*_*.txt' -ErrorAction SilentlyContinue |
          Where-Object { $_.LastWriteTime -ge $script:LastStart.AddSeconds(-1) } |
          Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($report) {
          Write-Host "Report ($($report.FullName)):" -ForegroundColor Yellow
          Get-Content -LiteralPath $report.FullName | ForEach-Object { Write-Host "    $_" }
          if ($Config -eq 'Release') {
            Write-Host 'For function names in the stack, rebuild with -Config RelWithDebInfo.'
          }
        }
        Write-Host "Send this log: $($script:RunLog)"
      }
      exit 0
    }
    if ((Get-Content $AutoHints -Raw) -match "0x$addr") {
      Fail "0x$addr is already registered but the game still can't call it. Send $($script:RunLog)"
    }
    Write-Host "    crashed calling 0x$addr - registering it and rebuilding" -ForegroundColor Yellow
    Add-FunctionHint $addr 'called through a pointer at runtime'
    Build-Game "fix$fix"
  }
  Fail "Still finding missed functions after $MaxFixes rebuilds. Run -FixCrashes again to continue."
}

if ($Run) {
  Step 'Launching'
  # The build folder has no game\ of its own: point it at the extracted game.
  Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Exe) -ArgumentList "--game_data_root=`"$GameDir`""
}
