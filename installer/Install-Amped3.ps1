<#
.SYNOPSIS
  Builds Amped 3 Revamped, a portable PC version of Amped 3, from your own
  Amped 3 (Xbox 360) disc image.

.DESCRIPTION
  1. Asks for the Amped 3 ISO (or takes -Iso) and where to put the game.
  2. Checks for the build tools (Visual Studio 2022 Build Tools with C++ and
     Clang, LLVM/Clang 20+, Git) and offers to install what's missing.
  3. Extracts the ISO (Xbox 360 disc format, read directly; no other tools)
     into <install folder>\game and checks it is Amped 3 (USA).
  4. Runs setup.ps1: gets and builds the ReXGlue SDK, recompiles the game's
     code and builds amped3.exe. The first build takes a long time.
  5. Puts amped3.exe and its runtime files in the install folder, next to the
     game folder. That folder is self-contained: it can be moved or copied to
     another PC (with a Vulkan / Direct3D 12 GPU) as is.

  Everything is logged to <project>\logs. Run it again to rebuild after an
  update; the ISO is only extracted once (-ReExtract to redo it).

.EXAMPLE
  Double-click "Install Amped 3 Revamped.bat" in the project folder.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File installer\Install-Amped3.ps1 -Iso "D:\Amped 3.iso" -InstallDir "D:\Games\Amped 3 Revamped"
#>
[CmdletBinding()]
param(
  # The Amped 3 (USA) Xbox 360 disc image. Default: asked for.
  [string]$Iso = '',
  # Or an already extracted game folder (the one holding default.xex).
  [string]$GameDir = '',
  # Where the playable game goes. Default: asked for (suggests Games\Amped 3 Revamped in your user folder).
  [string]$InstallDir = '',
  [ValidateSet('Release', 'RelWithDebInfo')]
  [string]$Config = 'Release',
  # Extract the ISO again even if the install folder already has the game.
  [switch]$ReExtract,
  # Don't offer to install missing build tools; just report them.
  [switch]$NoToolInstall,
  # Answer yes to every question (tool installs, shortcut).
  [switch]$Yes,
  # Launch the game when done.
  [switch]$Run,
  # Don't offer a desktop shortcut (the launcher's Update, through update.ps1).
  [switch]$NoShortcut,
  # Upscale the game's textures 4x on this PC with Real-ESRGAN (optional;
  # asked unless given). -UpscaleSet ui (menus, HUD, loading screens),
  # world (experimental) or all.
  [ValidateSet('ask', 'yes', 'no')]
  [string]$UpscaleTextures = 'ask',
  [ValidateSet('ui', 'world', 'all')]
  [string]$UpscaleSet = 'ui'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$InstallerDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$ProjectDir = Split-Path -Parent $InstallerDir
$SetupScript = Join-Path $ProjectDir 'setup.ps1'
$LogDir = Join-Path $ProjectDir 'logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$Transcript = Join-Path $LogDir ("install-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
try { Start-Transcript -Path $Transcript -Append | Out-Null } catch {}

# Amped 3 (USA): the title ID in default.xex's execution info.
$AmpedTitleId = 0x545407D4
# Rough space needed: game files (6 GB) + SDK and its build + the recompiled game's build.
$NeededGB = 40

function Step($text) { Write-Host "`n==> $text" -ForegroundColor Cyan }
function Note($text) { Write-Host "    $text" }
function Warn($text) { Write-Host "    $text" -ForegroundColor Yellow }
function Fail($text) {
  Write-Host "`nERROR: $text" -ForegroundColor Red
  Write-Host "Log: $Transcript"
  try { Stop-Transcript | Out-Null } catch {}
  if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null }
  exit 1
}
function Ask([string]$question) {
  if ($Yes) { return $true }
  $answer = Read-Host "$question [Y/n]"
  return (-not $answer) -or ($answer -match '^[Yy]')
}

if (-not (Test-Path $SetupScript)) { Fail "setup.ps1 not found next to the installer folder ($ProjectDir)." }

# ---------------------------------------------------------------------------
# Xbox 360 disc images (XDVDFS / "XISO"): the game partition starts at one of
# a few known offsets (XGD2, XGD3, XGD1, or 0 for a trimmed / rebuilt image);
# its volume descriptor (sector 32) points at the root directory, a binary
# tree of entries (left / right subtree offsets in dwords, first sector, size,
# attributes, name).
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Text;
using System.Collections.Generic;

public class XisoEntry {
  public string Path;
  public uint Sector;
  public uint Size;
  public bool IsDir;
}

public static class Xiso {
  const int SectorSize = 2048;
  static readonly long[] PartitionOffsets = { 0xFD90000L, 0x2080000L, 0x18300000L, 0L };

  static void ReadFull(Stream s, byte[] buf, int count) {
    int got = 0;
    while (got < count) {
      int n = s.Read(buf, got, count - got);
      if (n <= 0) throw new EndOfStreamException("Unexpected end of the disc image.");
      got += n;
    }
  }

  public static long FindPartition(FileStream fs) {
    byte[] magic = new byte[20];
    foreach (long off in PartitionOffsets) {
      long p = off + 32L * SectorSize;
      if (p + 28 > fs.Length) continue;
      fs.Position = p;
      ReadFull(fs, magic, 20);
      if (Encoding.ASCII.GetString(magic) == "MICROSOFT*XBOX*MEDIA") return off;
    }
    return -1;
  }

  // Every file and folder in the image (paths with backslashes, relative).
  public static List<XisoEntry> List(FileStream fs, long partition) {
    byte[] vd = new byte[28];
    fs.Position = partition + 32L * SectorSize;
    ReadFull(fs, vd, 28);
    uint rootSector = BitConverter.ToUInt32(vd, 20);
    uint rootSize = BitConverter.ToUInt32(vd, 24);
    List<XisoEntry> list = new List<XisoEntry>();
    Walk(fs, partition, rootSector, rootSize, "", list, 0);
    return list;
  }

  static bool SafeName(string name) {
    if (name.Length == 0 || name == "." || name == "..") return false;
    foreach (char c in name) {
      if (c < 32 || "\\/:*?\"<>|".IndexOf(c) >= 0) return false;
    }
    return true;
  }

  static void Walk(FileStream fs, long partition, uint sector, uint size, string prefix,
                   List<XisoEntry> list, int depth) {
    if (size == 0 || size > 64 * 1024 * 1024 || depth > 64) return;
    byte[] dir = new byte[size];
    fs.Position = partition + (long)sector * SectorSize;
    ReadFull(fs, dir, (int)size);
    Stack<int> todo = new Stack<int>();
    HashSet<int> seen = new HashSet<int>();
    todo.Push(0);
    while (todo.Count > 0) {
      int off = todo.Pop();
      if (off < 0 || off + 14 > dir.Length || !seen.Add(off)) continue;
      ushort left = BitConverter.ToUInt16(dir, off);
      ushort right = BitConverter.ToUInt16(dir, off + 2);
      if (left == 0xFFFF && right == 0xFFFF) continue;  // padding / empty folder
      uint entrySector = BitConverter.ToUInt32(dir, off + 4);
      uint entrySize = BitConverter.ToUInt32(dir, off + 8);
      byte attributes = dir[off + 12];
      int nameLength = dir[off + 13];
      if (off + 14 + nameLength > dir.Length) continue;
      string name = Encoding.GetEncoding(28591).GetString(dir, off + 14, nameLength);
      if (left != 0) todo.Push(left * 4);
      if (right != 0) todo.Push(right * 4);
      if (!SafeName(name)) continue;
      XisoEntry e = new XisoEntry();
      e.Path = prefix + name;
      e.Sector = entrySector;
      e.Size = entrySize;
      e.IsDir = (attributes & 0x10) != 0;
      list.Add(e);
      if (e.IsDir) Walk(fs, partition, entrySector, entrySize, e.Path + "\\", list, depth + 1);
    }
  }

  public static byte[] ReadFile(FileStream fs, long partition, XisoEntry e, int max) {
    int n = (int)Math.Min((long)e.Size, (long)max);
    byte[] data = new byte[n];
    fs.Position = partition + (long)e.Sector * SectorSize;
    ReadFull(fs, data, n);
    return data;
  }

  public static void ExtractFile(FileStream fs, long partition, XisoEntry e, string destination,
                                 byte[] buffer) {
    fs.Position = partition + (long)e.Sector * SectorSize;
    using (FileStream o = new FileStream(destination, FileMode.Create, FileAccess.Write,
                                         FileShare.None, 1 << 20)) {
      long left = e.Size;
      while (left > 0) {
        int n = (int)Math.Min(left, (long)buffer.Length);
        ReadFull(fs, buffer, n);
        o.Write(buffer, 0, n);
        left -= n;
      }
    }
  }
}
'@

# default.xex's title ID (XEX2 header, execution info, optional header 0x00040006).
function Get-XexTitleId([byte[]]$xex) {
  if ($xex.Length -lt 24 -or [Text.Encoding]::ASCII.GetString($xex, 0, 4) -ne 'XEX2') { return $null }
  function BE32([byte[]]$b, [int]$o) {
    return ([uint32]$b[$o] -shl 24) -bor ([uint32]$b[$o + 1] -shl 16) -bor ([uint32]$b[$o + 2] -shl 8) -bor [uint32]$b[$o + 3]
  }
  $count = BE32 $xex 0x14
  for ($i = 0; $i -lt $count -and (0x18 + $i * 8 + 8) -le $xex.Length; $i++) {
    if ((BE32 $xex (0x18 + $i * 8)) -eq 0x00040006) {
      $at = [int](BE32 $xex (0x18 + $i * 8 + 4))
      if ($at + 16 -le $xex.Length) { return (BE32 $xex ($at + 12)) }
    }
  }
  return $null
}

function Test-AmpedXex([byte[]]$xex) {
  $id = Get-XexTitleId $xex
  if ($null -eq $id) { return 'not an Xbox 360 executable' }
  if ($id -ne $AmpedTitleId) { return ('title ID {0:X8}, not Amped 3 (USA, {1:X8})' -f $id, $AmpedTitleId) }
  return $null
}

# ---------------------------------------------------------------------------
Write-Host 'Amped 3 Revamped installer' -ForegroundColor Green
Write-Host 'Builds the game from your own Xbox 360 disc image. Nothing from the game is downloaded.'

if (-not $Iso -and -not $GameDir) {
  Add-Type -AssemblyName System.Windows.Forms
  $dialog = New-Object System.Windows.Forms.OpenFileDialog
  $dialog.Title = 'Choose your Amped 3 (Xbox 360) disc image'
  $dialog.Filter = 'Xbox 360 disc image (*.iso)|*.iso|All files (*.*)|*.*'
  if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Fail 'No disc image chosen.' }
  $Iso = $dialog.FileName
}
if (-not $InstallDir) {
  $suggested = Join-Path ([Environment]::GetFolderPath('UserProfile')) 'Games\Amped 3 Revamped'
  if ($Yes) {
    $InstallDir = $suggested
  } else {
    $answer = Read-Host "Install folder [$suggested]"
    $InstallDir = if ($answer) { $answer.Trim('"') } else { $suggested }
  }
}
$InstallDir = [IO.Path]::GetFullPath($InstallDir)
$TargetGame = Join-Path $InstallDir 'game'
Note "Install folder: $InstallDir"

# ---------------------------------------------------------------------------
Step 'Checking free space'
$drive = [IO.Path]::GetPathRoot($InstallDir)
$projectDrive = [IO.Path]::GetPathRoot($ProjectDir)
foreach ($d in @($drive, $projectDrive) | Select-Object -Unique) {
  try {
    $free = (New-Object IO.DriveInfo $d).AvailableFreeSpace / 1GB
    Note ('{0} {1:N0} GB free' -f $d, $free)
    if ($free -lt $NeededGB) {
      Warn "The build needs roughly $NeededGB GB in total (game, SDK, compiled code); $d may run out."
    }
  } catch {}
}

# ---------------------------------------------------------------------------
Step 'Checking the build tools'
function Update-Path {
  $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
  $user = [Environment]::GetEnvironmentVariable('Path', 'User')
  $env:Path = "$machine;$user"
}
function Get-ClangMajor([string]$exe) {
  if (-not (Test-Path $exe)) { return 0 }
  $line = (& $exe --version 2>$null | Select-Object -First 1)
  if ($line -match 'version (\d+)') { return [int]$Matches[1] }
  return 0
}
function Find-VS {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) { return $null }
  $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if ($path) { return $path }
  return $null
}
function Find-Clang20 {
  $dirs = @((Join-Path $env:ProgramFiles 'LLVM\bin'))
  $vs = Find-VS
  if ($vs) { $dirs += Join-Path $vs 'VC\Tools\Llvm\x64\bin' }
  foreach ($dir in $dirs) { if ((Get-ClangMajor (Join-Path $dir 'clang.exe')) -ge 20) { return $dir } }
  return $null
}
function Find-CMakeNinja {
  $vs = Find-VS
  if (-not $vs) { return $false }
  $cm = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
  return (Test-Path (Join-Path $cm 'CMake\bin\cmake.exe')) -and (Test-Path (Join-Path $cm 'Ninja\ninja.exe'))
}
$haveWinget = [bool](Get-Command winget -ErrorAction SilentlyContinue)
$VsComponents = @(
  'Microsoft.VisualStudio.Workload.VCTools',
  'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
  'Microsoft.VisualStudio.Component.Windows11SDK.22621',
  'Microsoft.VisualStudio.Component.VC.CMake.Project',
  'Microsoft.VisualStudio.Component.VC.Llvm.Clang',
  'Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset'
)
function Install-Tool([string]$what, [string]$wingetId, [string]$override, [string]$manual) {
  if ($NoToolInstall) { Fail "$what is missing. Install it ($manual) and run the installer again." }
  if (-not (Ask "    $what is missing. Install it now?")) { Fail "$what is needed. Install it ($manual) and run the installer again." }
  if ($haveWinget) {
    $wingetArgs = @('install', '--id', $wingetId, '-e', '--source', 'winget', '--accept-package-agreements', '--accept-source-agreements')
    if ($override) { $wingetArgs += @('--override', $override) }
    Note "winget $($wingetArgs -join ' ')"
    & winget @wingetArgs
    Update-Path
  } elseif ($wingetId -eq 'Microsoft.VisualStudio.2022.BuildTools') {
    $boot = Join-Path $env:TEMP 'vs_BuildTools.exe'
    Note 'Downloading the Visual Studio Build Tools installer'
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -Uri 'https://aka.ms/vs/17/release/vs_BuildTools.exe' -OutFile $boot -UseBasicParsing
    Start-Process -FilePath $boot -ArgumentList $override -Wait
  } else {
    Fail "$what is missing and winget isn't available to install it. Install it ($manual) and run the installer again."
  }
}

$vsArgs = '--wait --passive --norestart ' + (($VsComponents | ForEach-Object { "--add $_" }) -join ' ')
$vsPath = Find-VS
if (-not $vsPath -or -not (Find-CMakeNinja)) {
  if ($vsPath) {
    # Visual Studio is there but without everything: add the components.
    $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
    if (-not $NoToolInstall -and (Test-Path $installer) -and (Ask '    Visual Studio is missing some C++ components (CMake / Clang). Add them now?')) {
      Start-Process -FilePath $installer -ArgumentList ("modify --installPath `"$vsPath`" " + $vsArgs) -Wait
    }
  } else {
    Install-Tool 'Visual Studio 2022 Build Tools (C++, Clang, CMake)' 'Microsoft.VisualStudio.2022.BuildTools' $vsArgs `
      'https://visualstudio.microsoft.com/downloads/ - Build Tools for Visual Studio 2022, "Desktop development with C++" plus the C++ Clang components'
  }
  $vsPath = Find-VS
  if (-not $vsPath) { Fail 'Visual Studio Build Tools still not found after installing. Restart Windows and run the installer again.' }
}
Note "Visual Studio: $vsPath"

if (-not (Find-Clang20)) {
  Install-Tool 'LLVM / Clang 20 or newer' 'LLVM.LLVM' '' 'https://github.com/llvm/llvm-project/releases (LLVM-20 or newer, win64)'
  if (-not (Find-Clang20)) { Fail 'Clang 20+ still not found. Install LLVM from https://github.com/llvm/llvm-project/releases and run again.' }
}
Note "Clang 20+: $(Find-Clang20)"

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
  Install-Tool 'Git' 'Git.Git' '' 'https://git-scm.com/download/win'
  if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail 'Git still not found. Open a new window and run the installer again.' }
}
Note "Git: $((Get-Command git).Source)"

# ---------------------------------------------------------------------------
if ($GameDir) {
  Step 'Checking the game folder'
  $GameDir = [IO.Path]::GetFullPath($GameDir)
  $xexPath = Join-Path $GameDir 'default.xex'
  if (-not (Test-Path $xexPath)) { Fail "No default.xex in $GameDir." }
  $bad = Test-AmpedXex ([IO.File]::ReadAllBytes($xexPath))
  if ($bad) { Fail "$xexPath is $bad." }
  if ($GameDir -ne $TargetGame) {
    Note "Copying the game files to $TargetGame"
    New-Item -ItemType Directory -Force -Path $TargetGame | Out-Null
    & robocopy $GameDir $TargetGame /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { Fail "Copying the game files failed (robocopy $LASTEXITCODE)." }
  }
} else {
  $existing = Join-Path $TargetGame 'default.xex'
  if ((Test-Path $existing) -and -not $ReExtract -and -not (Test-AmpedXex ([IO.File]::ReadAllBytes($existing)))) {
    Step 'Game files already extracted'
    Note "$TargetGame (use -ReExtract to extract the ISO again)"
  } else {
    Step 'Reading the disc image'
    if (-not (Test-Path $Iso)) { Fail "Disc image not found: $Iso" }
    $fs = [IO.File]::Open($Iso, 'Open', 'Read', 'Read')
    try {
      $partition = [Xiso]::FindPartition($fs)
      if ($partition -lt 0) { Fail "$Iso isn't an Xbox 360 disc image (no XDVDFS volume found)." }
      Note ('Xbox disc volume at 0x{0:X}' -f $partition)
      $entries = [Xiso]::List($fs, $partition)
      $xexEntry = $entries | Where-Object { -not $_.IsDir -and $_.Path -ieq 'default.xex' } | Select-Object -First 1
      if (-not $xexEntry) { Fail 'The disc image has no default.xex.' }
      $bad = Test-AmpedXex ([Xiso]::ReadFile($fs, $partition, $xexEntry, 1MB))
      if ($bad) { Fail "This disc's default.xex is $bad." }
      $files = @($entries | Where-Object { -not $_.IsDir })
      $total = ($files | Measure-Object -Property Size -Sum).Sum
      Note ('Amped 3 (USA): {0} files, {1:N1} GB' -f $files.Count, ($total / 1GB))

      Step "Extracting to $TargetGame"
      New-Item -ItemType Directory -Force -Path $TargetGame | Out-Null
      foreach ($dir in ($entries | Where-Object { $_.IsDir })) {
        New-Item -ItemType Directory -Force -Path (Join-Path $TargetGame $dir.Path) | Out-Null
      }
      $buffer = New-Object byte[] (4MB)
      $done = 0L
      $lastPct = -1
      foreach ($f in $files) {
        $dest = Join-Path $TargetGame $f.Path
        New-Item -ItemType Directory -Force -Path (Split-Path $dest) | Out-Null
        [Xiso]::ExtractFile($fs, $partition, $f, $dest, $buffer)
        $done += $f.Size
        $pct = [int](100 * $done / [Math]::Max($total, 1))
        if ($pct -ne $lastPct) {
          Write-Progress -Activity 'Extracting Amped 3' -Status "$pct% - $($f.Path)" -PercentComplete $pct
          $lastPct = $pct
        }
      }
      Write-Progress -Activity 'Extracting Amped 3' -Completed
      Note 'Done.'
    } finally {
      $fs.Dispose()
    }
  }
}

# ---------------------------------------------------------------------------
Step 'Building (setup.ps1) - the first build takes a long time (often an hour or more)'
$setupArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $SetupScript,
               '-GameDir', $TargetGame, '-Config', $Config)
& powershell.exe @setupArgs
if ($LASTEXITCODE -ne 0) { Fail "The build failed (setup.ps1 exit $LASTEXITCODE). Its logs are in $LogDir." }

# Copies over a file the running game may hold open (the launcher's Update):
# Windows can't overwrite it, but can rename it; the game deletes *.old files
# when it next starts.
function Copy-Replacing([string]$from, [string]$to) {
  try {
    Copy-Item -LiteralPath $from -Destination $to -Force
  } catch {
    Remove-Item -LiteralPath "$to.old" -Force -ErrorAction SilentlyContinue
    Move-Item -LiteralPath $to -Destination "$to.old" -Force
    Copy-Item -LiteralPath $from -Destination $to -Force
  }
}

# ---------------------------------------------------------------------------
Step 'Putting the game together'
$preset = if ($Config -eq 'Release') { 'win-amd64-release' } else { 'win-amd64-relwithdebinfo' }
$buildDir = Join-Path $ProjectDir "out\build\$preset"
$exe = Join-Path $buildDir 'amped3.exe'
if (-not (Test-Path $exe)) { Fail "The build finished but $exe is missing." }
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$copied = 0
foreach ($pattern in 'amped3.exe', '*.dll', '*LICENSE*.txt', 'amped3.pdb') {
  if ($pattern -eq 'amped3.pdb' -and $Config -eq 'Release') { continue }
  foreach ($file in Get-ChildItem -Path $buildDir -Filter $pattern -File -ErrorAction SilentlyContinue) {
    Copy-Replacing $file.FullName (Join-Path $InstallDir $file.Name)
    $copied++
  }
}
# The texture pack (the new logos; see textures\README.txt). Ours are
# refreshed, anything else put there is kept.
$packSrc = Join-Path $buildDir 'textures'
if (Test-Path $packSrc) {
  $packDst = Join-Path $InstallDir 'textures'
  New-Item -ItemType Directory -Force -Path $packDst | Out-Null
  foreach ($file in Get-ChildItem -Path $packSrc -File -Recurse) {
    $rel = $file.FullName.Substring($packSrc.Length).TrimStart('\')
    $dst = Join-Path $packDst $rel
    New-Item -ItemType Directory -Force -Path (Split-Path $dst) | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $dst -Force
    $copied++
  }
}
Note "$copied file(s) copied next to $TargetGame"
# Starting settings, only on a first install: amped3.toml holds the options
# menu's choices, which a reinstall keeps.
$defaults = Join-Path $ProjectDir 'amped3.defaults.toml'
if ((Test-Path $defaults) -and -not (Test-Path (Join-Path $InstallDir 'amped3.toml'))) {
  Copy-Item $defaults (Join-Path $InstallDir 'amped3.toml') -Force
}
$installedExe = Join-Path $InstallDir 'amped3.exe'
# The shortcut's icon: the game's own Xbox 360 dashboard icon, taken from this
# copy's default.xex (tools\xex-icon; nothing of the game ships with the project).
$iconTool = Join-Path $buildDir 'amped3-icon.exe'
$iconFile = Join-Path $InstallDir 'amped3-icon.ico'
if (Test-Path $iconTool) {
  $out = & $iconTool (Join-Path $TargetGame 'default.xex') $InstallDir 2>&1
  if ($LASTEXITCODE -ne 0) { Warn "Couldn't make the icon from default.xex: $out" }
}
# Where the sources are, for the launcher's Update (update.ps1).
[IO.File]::WriteAllText((Join-Path $InstallDir 'source.path'), "$ProjectDir`n")

# Optional: the game's textures upscaled on this PC (installer\Upscale-Textures.ps1;
# also "Upscale Textures.bat" to run it later). Not part of -Yes: it downloads
# Real-ESRGAN and keeps the GPU busy for a while.
$upscale = switch ($UpscaleTextures) {
  'yes' { $true }
  'no' { $false }
  default {
    if ($Yes) { $false } else {
      $answer = Read-Host ("    Upscale the game's menu, HUD and loading screen textures 4x on this PC with " +
                           "Real-ESRGAN? Downloads 45 MB and takes a few minutes on the GPU; textures " +
                           "you replaced by hand are left alone. [y/N]")
      $answer -match '^[Yy]'
    }
  }
}
if ($upscale) {
  Step 'Upscaling textures (Real-ESRGAN)'
  try {
    & (Join-Path $InstallerDir 'Upscale-Textures.ps1') -InstallDir $InstallDir -Set $UpscaleSet `
      -Tool (Join-Path $buildDir 'amped3-texture-upscale.exe')
  } catch {
    Warn "Texture upscaling didn't finish: $_ (the game runs without it; run 'Upscale Textures.bat' to retry)"
  }
}

if (-not $NoShortcut -and (Ask '    Add a desktop shortcut?')) {
  $shell = New-Object -ComObject WScript.Shell
  $link = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'Amped 3 Revamped.lnk'))
  $link.TargetPath = $installedExe
  $link.WorkingDirectory = $InstallDir
  if (Test-Path $iconFile) { $link.IconLocation = "$iconFile,0" }
  $link.Save()
  Note 'Desktop shortcut created.'
}

Write-Host "`nDone. Amped 3 Revamped is in $InstallDir" -ForegroundColor Green
Write-Host 'The folder is portable: amped3.exe finds the game in the "game" folder next to it.'
Write-Host 'In game: Back+Start (or F1) opens the options menu.'
Write-Host "To rebuild after updating the project, run the installer again (the ISO isn't extracted twice)."
try { Stop-Transcript | Out-Null } catch {}
if ($Run) { Start-Process -FilePath $installedExe -WorkingDirectory $InstallDir }
if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null }
