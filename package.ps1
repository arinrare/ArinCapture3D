param(
  [ValidateSet('Debug','Release','RelWithDebInfo','MinSizeRel')]
  [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'

function Set-VsDevEnvironment {
  # If we're already in a VS developer environment, calling VsDevCmd again can push PATH
  # over cmd.exe's 8191-character command-line limit and cause failures.
  if ($env:VSCMD_VER -or $env:VCToolsInstallDir -or ($env:Path -match 'VC\\Tools\\MSVC')) {
    Write-Host "== VS Dev Environment ==" -ForegroundColor Cyan
    Write-Host "VS developer environment already active; skipping VsDevCmd.bat." -ForegroundColor DarkGray
    return $null
  }

  if ($env:Path -and $env:Path.Length -ge 7600) {
    Write-Host "== VS Dev Environment ==" -ForegroundColor Cyan
    Write-Warning "PATH is already very long ($($env:Path.Length) chars). Skipping VsDevCmd.bat to avoid cmd.exe length limits."
    return $null
  }

  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) {
    Write-Warning "vswhere.exe not found; assuming build environment is already configured."
    return
  }

  $vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
  if (-not $vsInstall) {
    Write-Warning "Visual Studio installation not found by vswhere; assuming build environment is already configured."
    return
  }

  $vsDevCmd = Join-Path $vsInstall 'Common7\Tools\VsDevCmd.bat'
  if (-not (Test-Path $vsDevCmd)) {
    Write-Warning "VsDevCmd.bat not found; assuming build environment is already configured."
    return
  }

  # Instead of importing all environment variables into this PowerShell session (which can
  # trigger cmd.exe "input line is too long" issues on some setups), return the VsDevCmd path
  # and run the build steps inside a single cmd.exe call.
  return $vsDevCmd
}

function Invoke-InVsDevCmd {
  param(
    [Parameter(Mandatory = $true)][string]$VsDevCmd,
    [Parameter(Mandatory = $true)][string]$Command
  )

  Write-Host "== VS Dev Environment ==" -ForegroundColor Cyan
  if (-not (Test-Path $VsDevCmd)) {
    throw "VsDevCmd.bat not found at $VsDevCmd"
  }

  # Run the given command inside a VS dev cmd environment.
  # Build the cmd line via concatenation to avoid PowerShell escaping pitfalls.
  $cmd = 'call "' + $VsDevCmd + '" -arch=x64 -host_arch=x64 >nul && ' + $Command
  & cmd.exe /d /c $cmd
  if ($LASTEXITCODE -ne 0) {
    throw "Command failed with exit code $LASTEXITCODE"
  }
}

function Get-ProjectVersion {
  $readmePath = Join-Path $PSScriptRoot 'README.md'
  if (-not (Test-Path $readmePath)) {
    throw "README.md not found at $readmePath"
  }
  $readme = Get-Content $readmePath
  # Accept either:
  #   ### Alpha version: 0.0.17
  #   ### Beta version: 0.1.0
  #   ### Beta version 0.1.0
  # and require a full x.y.z numeric semver.
  $versionLine = $readme |
    Where-Object { $_ -match '^###\s+(Alpha|Beta)\s+version\s*:?' } |
    Select-Object -First 1

  if ($versionLine -match '(?i)^###\s+(Alpha|Beta)\s+version\s*:?\s*(\d+\.\d+\.\d+)\s*$') {
    return $Matches[2]
  }

  throw 'Could not find version in README.md (expected: ### Alpha version: x.y.z or ### Beta version: x.y.z)'
}

function Find-Ninja {
  $ninjaCmd = Get-Command ninja.exe -ErrorAction SilentlyContinue
  if ($ninjaCmd -and $ninjaCmd.Source) {
    return $ninjaCmd.Source
  }

  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) {
    return $null
  }

  $vsInstall = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
  if (-not $vsInstall) {
    return $null
  }

  $candidates = @(
    (Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'),
    (Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\bin\ninja.exe')
  )

  foreach ($candidate in $candidates) {
    if (Test-Path $candidate) {
      return $candidate
    }
  }

  $cmakeExtRoot = Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake'
  if (Test-Path $cmakeExtRoot) {
    $found = Get-ChildItem -Path $cmakeExtRoot -Recurse -Filter 'ninja.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($found -and $found.FullName) {
      return $found.FullName
    }
  }

  return $null
}

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $root 'build'
$distDir = Join-Path $root 'dist'
$stageName = "ArinCaptureSBS-$Config"
$stageDir = Join-Path $distDir $stageName

$vsDevCmd = Set-VsDevEnvironment

$ninjaPath = Find-Ninja
if (-not $ninjaPath) {
  throw 'ninja.exe not found. Install Ninja or install Visual Studio CMake/Ninja components, or run from a shell where Ninja is on PATH.'
}

Write-Host "== Configure ==" -ForegroundColor Cyan
if (Test-Path (Join-Path $buildDir 'CMakeCache.txt')) {
  $cache = Get-Content (Join-Path $buildDir 'CMakeCache.txt') -Raw
  if (($cache -notmatch 'CMAKE_GENERATOR:INTERNAL=Ninja Multi-Config') -or ($cache -match 'CMAKE_MAKE_PROGRAM:FILEPATH=CMAKE_MAKE_PROGRAM-NOTFOUND')) {
    Remove-Item -Recurse -Force $buildDir
  }
}

# Always configure to ensure CMAKE_MAKE_PROGRAM is valid in this environment.
if (Test-Path $stageDir) { Remove-Item -Recurse -Force $stageDir }
New-Item -ItemType Directory -Force -Path $stageDir | Out-Null

$configureCmd = 'cmake -S "' + $root + '" -B "' + $buildDir + '" -G "Ninja Multi-Config" -DCMAKE_MAKE_PROGRAM="' + $ninjaPath + '" -DCMAKE_EXPORT_COMPILE_COMMANDS=TRUE'
$buildCmd = 'cmake --build "' + $buildDir + '" --config ' + $Config
$installCmd = 'cmake --install "' + $buildDir + '" --config ' + $Config + ' --prefix "' + $stageDir + '"'

Write-Host "== Configure/Build/Stage ($Config) ==" -ForegroundColor Cyan
if ($vsDevCmd) {
  Invoke-InVsDevCmd -VsDevCmd $vsDevCmd -Command "$configureCmd && $buildCmd && $installCmd"
} else {
  # Fall back to running directly (assumes environment already has cl/link on PATH).
  cmake -S $root -B $buildDir -G "Ninja Multi-Config" -DCMAKE_MAKE_PROGRAM="$ninjaPath" -DCMAKE_EXPORT_COMPILE_COMMANDS=TRUE
  cmake --build $buildDir --config $Config
  cmake --install $buildDir --config $Config --prefix $stageDir
}

# Optional: include PDB for debugging if present
$pdb = Join-Path $buildDir "$Config\ArinCaptureSBS.pdb"
if (Test-Path $pdb) {
  Copy-Item $pdb -Destination $stageDir -Force
}

$version = Get-ProjectVersion
$zipName = "ArinCaptureSBS-$Config-$version.zip"
$zipPath = Join-Path $distDir $zipName

if (-not (Test-Path $distDir)) {
  New-Item -ItemType Directory -Force -Path $distDir | Out-Null
}
if (Test-Path $zipPath) {
  Remove-Item -Force $zipPath
}

Write-Host "== Zip ==" -ForegroundColor Cyan
$sevenZip = "C:\Program Files\7-Zip\7z.exe"
if (-not (Test-Path $sevenZip)) { $sevenZip = "C:\Program Files (x86)\7-Zip\7z.exe" }

if (Test-Path $sevenZip) {
  Push-Location $distDir
  try {
    & $sevenZip a -tzip $zipPath $stageName | Write-Host
  } finally {
    Pop-Location
  }
} else {
  Compress-Archive -Path $stageDir -DestinationPath $zipPath -Force
}

Write-Host "Created: $zipPath" -ForegroundColor Green
