<#
.SYNOPSIS
  Updates Amped 3 Revamped from GitHub. Run by the game's launcher (and usable
  by hand); the Linux counterpart is update.sh.

.DESCRIPTION
  -Action branches   Lists the repository's branches.
  -Action check      What's new on -Branch since -Base (the commit the running
                     game was built from).
  -Action update     Gets -Branch into this project folder (git when it is a
                     git checkout, else the branch's zip from GitHub), then
                     rebuilds:
                       - the game in -ExeDir is an installed copy (made by
                         "Install Amped 3 Revamped.bat"): the installer runs
                         again for that folder;
                       - the game in -ExeDir is this project's own build
                         (out\build\...): setup.ps1 runs.

  The launcher reads lines starting with "::" (::branch::, ::remote::,
  ::behind::, ::ahead::, ::commit::, ::step::, ::error::, ::done::); everything else is
  shown as it is. A git checkout is reset to the branch on GitHub
  (git checkout -f -B): local changes to tracked files are discarded.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File update.ps1 -Action update -Branch main -ExeDir "D:\Games\Amped 3 Revamped"
#>
[CmdletBinding()]
param(
  [ValidateSet('branches', 'check', 'update')]
  [string]$Action = 'check',
  [string]$Branch = 'main',
  # The commit the running game was built from (check).
  [string]$Base = '',
  # The folder of the game being updated (update; default: the installer's
  # suggested folder is not guessed, so it is required).
  [string]$ExeDir = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$ProgressPreference = 'SilentlyContinue'  # Invoke-WebRequest is slow with the progress bar
try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch {}

$ProjectDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
$DefaultRepo = 'minty-hippo/amped3revamped'

function Say([string]$tag, [string]$text) { [Console]::Out.WriteLine("::${tag}:: $text"); [Console]::Out.Flush() }
function Line([string]$text) { [Console]::Out.WriteLine($text); [Console]::Out.Flush() }
function Fail([string]$text) { Say 'error' $text; exit 1 }

function IsGitCheckout {
  (Test-Path (Join-Path $ProjectDir '.git')) -and (Get-Command git -ErrorAction SilentlyContinue)
}

# Runs a program, passing its output (stderr too) on line by line; returns its
# exit code. Native programs' stderr must not stop the script (git writes its
# progress there).
function Invoke-Streamed([string]$exe, [string[]]$argv) {
  $ErrorActionPreference = 'Continue'
  & $exe @argv 2>&1 | ForEach-Object { Line "$_" }
  return $LASTEXITCODE
}

function Invoke-Git {
  $code = Invoke-Streamed 'git' (@('-C', $ProjectDir, '-c', 'safe.directory=*') + $args)
  if ($code -ne 0) { Fail "git $($args -join ' ') failed (exit $code)." }
}

# owner/repo: from the checkout's origin when it points at GitHub.
function RepoSlug {
  if (IsGitCheckout) {
    $ErrorActionPreference = 'Continue'
    $url = (& git -C $ProjectDir -c safe.directory=* remote get-url origin 2>$null) | Select-Object -First 1
    if ($url -match 'github\.com[:/]+([^/]+)/([^/\s]+?)(\.git)?/?$') { return "$($Matches[1])/$($Matches[2])" }
  }
  return $DefaultRepo
}

function Api([string]$path) {
  $headers = @{ 'User-Agent' = 'amped3-revamped-launcher'; 'Accept' = 'application/vnd.github+json' }
  Invoke-RestMethod -Uri "https://api.github.com/repos/$(RepoSlug)$path" -Headers $headers -TimeoutSec 20
}

function FirstLine([string]$text) { ($text -split "`n")[0].Trim() }

switch ($Action) {
  'branches' {
    try { $list = Api '/branches?per_page=100' } catch { Fail "Couldn't reach GitHub: $($_.Exception.Message)" }
    foreach ($b in $list) { Say 'branch' $b.name }
    Say 'done' ''
    exit 0
  }

  'check' {
    try { $head = Api "/commits/$([Uri]::EscapeDataString($Branch))" } catch {
      Fail "Couldn't read branch '$Branch' from GitHub: $($_.Exception.Message)"
    }
    $date = ([datetime]$head.commit.committer.date).ToString('yyyy-MM-dd')
    Say 'remote' "$($head.sha) $date $(FirstLine $head.commit.message)"
    if ($Base -and $Base -ne 'unknown') {
      if ($head.sha.StartsWith($Base) -or $Base.StartsWith($head.sha)) {
        Say 'behind' '0'
      } else {
        try {
          $cmp = Api "/compare/$Base...$($head.sha)"
          Say 'behind' "$($cmp.ahead_by)"
          Say 'ahead' "$($cmp.behind_by)"  # in the build, not on the branch
          $commits = @($cmp.commits)
          [array]::Reverse($commits)  # newest first
          foreach ($c in ($commits | Select-Object -First 20)) {
            Say 'commit' "$($c.sha.Substring(0, 7)) $(FirstLine $c.commit.message)"
          }
        } catch {
          # The build's commit isn't on GitHub (local commits) or the API refused.
          Say 'behind' '?'
        }
      }
    }
    Say 'done' ''
    exit 0
  }

  'update' {
    if (-not $ExeDir) { Fail '-ExeDir is required.' }
    $ExeDir = [IO.Path]::GetFullPath($ExeDir)
    $outDir = [IO.Path]::GetFullPath((Join-Path $ProjectDir 'out')) + '\'
    $devBuild = $ExeDir.StartsWith($outDir, [StringComparison]::OrdinalIgnoreCase)

    Say 'step' "Getting the $Branch branch"
    if (IsGitCheckout) {
      Invoke-Git fetch origin $Branch
      Invoke-Git checkout -f -B $Branch "origin/$Branch"
      Invoke-Git log -1 '--format=%h %cs %s'
    } else {
      $repo = RepoSlug
      $tmp = Join-Path ([IO.Path]::GetTempPath()) ("amped3-update-" + [guid]::NewGuid().ToString('N'))
      New-Item -ItemType Directory -Force -Path $tmp | Out-Null
      try {
        try { $head = Api "/commits/$([Uri]::EscapeDataString($Branch))" } catch {
          Fail "Couldn't read branch '$Branch' from GitHub: $($_.Exception.Message)"
        }
        $zip = Join-Path $tmp 'src.zip'
        Line "Downloading https://github.com/$repo/archive/$($head.sha).zip"
        Invoke-WebRequest -UseBasicParsing -Uri "https://codeload.github.com/$repo/zip/$($head.sha)" `
          -OutFile $zip -Headers @{ 'User-Agent' = 'amped3-revamped-launcher' } -TimeoutSec 300
        Expand-Archive -Path $zip -DestinationPath (Join-Path $tmp 'x') -Force
        $top = @(Get-ChildItem -Path (Join-Path $tmp 'x') -Directory)[0].FullName
        Line "Copying into $ProjectDir"
        & robocopy $top $ProjectDir /E /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { Fail "Copying the new sources failed (robocopy $LASTEXITCODE)." }
        $date = ([datetime]$head.commit.committer.date).ToString('yyyy-MM-dd')
        [IO.File]::WriteAllText((Join-Path $ProjectDir '.amped3-source'),
          "branch=$Branch`ncommit=$($head.sha)`ndate=$date`n")
        Line "$($head.sha.Substring(0, 7)) $date $(FirstLine $head.commit.message)"
      } finally {
        Remove-Item -Recurse -Force -Path $tmp -ErrorAction SilentlyContinue
      }
    }

    # The running game holds its exe and DLLs open: Windows can't replace
    # them, but can rename them. The game deletes the *.old files when it next
    # starts.
    foreach ($file in Get-ChildItem -Path $ExeDir -File -ErrorAction SilentlyContinue |
             Where-Object { $_.Name -eq 'amped3.exe' -or $_.Extension -eq '.dll' }) {
      $old = "$($file.FullName).old"
      try {
        Remove-Item -LiteralPath $old -Force -ErrorAction SilentlyContinue
        Move-Item -LiteralPath $file.FullName -Destination $old -Force
      } catch {
        Line "Couldn't move $($file.Name) aside: $($_.Exception.Message)"
      }
    }

    if ($devBuild) {
      Say 'step' 'Building (setup.ps1)'
      $setupArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $ProjectDir 'setup.ps1'))
      $gamePath = Join-Path $ExeDir 'game.path'
      if (Test-Path $gamePath) { $setupArgs += @('-GameDir', ([IO.File]::ReadAllText($gamePath).Trim())) }
      $code = Invoke-Streamed 'powershell.exe' $setupArgs
      if ($code -ne 0) { Fail "The build failed (setup.ps1 exit $code); its logs are in $ProjectDir\logs." }
    } else {
      Say 'step' 'Building and installing (Install-Amped3.ps1)'
      $installArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                       (Join-Path $ProjectDir 'installer\Install-Amped3.ps1'),
                       '-GameDir', (Join-Path $ExeDir 'game'), '-InstallDir', $ExeDir,
                       '-Yes', '-UpscaleTextures', 'no')
      # (an older installer, on a branch from before the launcher, lacks it)
      if (Select-String -Path (Join-Path $ProjectDir 'installer\Install-Amped3.ps1') -Pattern 'NoShortcut' -Quiet) {
        $installArgs += '-NoShortcut'
      }
      $code = Invoke-Streamed 'powershell.exe' $installArgs
      if ($code -ne 0) { Fail "The build failed (installer exit $code); its logs are in $ProjectDir\logs." }
    }
    # Whatever wasn't rebuilt (a DLL the build didn't touch) comes back.
    foreach ($old in Get-ChildItem -Path $ExeDir -Filter '*.old' -File -ErrorAction SilentlyContinue) {
      $orig = $old.FullName.Substring(0, $old.FullName.Length - 4)
      if (-not (Test-Path -LiteralPath $orig)) { Copy-Item -LiteralPath $old.FullName -Destination $orig }
    }
    Say 'done' ''
    exit 0
  }
}
