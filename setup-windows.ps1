# SPDX-License-Identifier: MIT

[CmdletBinding()]
param(
  [ValidateSet("Check", "Install")]
  [string]$Mode = "Check",
  [ValidateSet("Debug", "Release")]
  [string]$Configuration = "Release",
  [string]$BuildDirectory = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Test-VdsAdministrator {
  $Identity = [Security.Principal.WindowsIdentity]::GetCurrent()
  $Principal = New-Object Security.Principal.WindowsPrincipal($Identity)
  return $Principal.IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Get-VdsVisualStudioPath {
  $VsWhere = Join-Path ${env:ProgramFiles(x86)} `
    "Microsoft Visual Studio\Installer\vswhere.exe"
  if (!(Test-Path -LiteralPath $VsWhere -PathType Leaf)) {
    throw "vswhere.exe was not found. Install Visual Studio 2022 Build Tools."
  }
  $Arguments = @(
    "-latest", "-products", "*",
    "-requires", "Microsoft.VisualStudio.Component.Vcpkg",
    "-property", "installationPath"
  )
  $Paths = @(& $VsWhere @Arguments)
  if ($LASTEXITCODE -ne 0 -or $Paths.Count -eq 0) {
    throw "Visual Studio 2022 with the vcpkg component was not found."
  }
  return $Paths[0].Trim()
}

function Get-VdsCMakePath {
  param([Parameter(Mandatory = $true)][string]$VisualStudioPath)

  $Command = Get-Command cmake.exe -ErrorAction SilentlyContinue
  if ($Command) {
    return $Command.Source
  }
  $Path = Join-Path $VisualStudioPath `
    "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  if (!(Test-Path -LiteralPath $Path -PathType Leaf)) {
    throw "cmake.exe was not found."
  }
  return $Path
}

function Invoke-VdsDependencyScript {
  param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("Check", "Download", "Install")]
    [string]$DependencyMode,
    [Parameter(Mandatory = $true)][string]$DownloadDirectory,
    [Parameter(Mandatory = $true)][string]$LogPath
  )

  $Script = Join-Path $PSScriptRoot `
    "packaging\windows\install_dependencies.ps1"
  $Arguments = @(
    "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $Script,
    "-Mode", $DependencyMode,
    "-DownloadDir", $DownloadDirectory,
    "-LogPath", $LogPath
  )
  & powershell.exe @Arguments
  return $LASTEXITCODE
}

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
  $BuildDirectory = Join-Path $PSScriptRoot "out\build\windows"
} else {
  $BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
}

$DependencyDirectory = Join-Path $env:TEMP "vds-dependencies"
$DependencyLog = Join-Path $DependencyDirectory "install.log"

if ($Mode -eq "Check") {
  $DependencyResult = Invoke-VdsDependencyScript `
    -DependencyMode Check `
    -DownloadDirectory $DependencyDirectory `
    -LogPath $DependencyLog
  if ($DependencyResult -eq 0) {
    Write-Host "[OK] USB/IP and HidHide are installed."
  } elseif ($DependencyResult -eq 10) {
    Write-Warning "USB/IP or HidHide is missing. Run -Mode Install as Administrator."
  } else {
    throw "Dependency check failed with exit code $DependencyResult. Log: $DependencyLog"
  }

  $DaemonPath = Join-Path $BuildDirectory "$Configuration\vdsd.exe"
  $ControlPath = Join-Path $BuildDirectory "$Configuration\vdsctl.exe"
  if ((Test-Path -LiteralPath $DaemonPath -PathType Leaf) -and
      (Test-Path -LiteralPath $ControlPath -PathType Leaf)) {
    Write-Host "[OK] vDS build found: $BuildDirectory\$Configuration"
  } else {
    Write-Warning "Build output is missing. Run .\build-windows.ps1 first."
  }

  $Service = Get-Service -Name vdsd -ErrorAction SilentlyContinue
  if ($Service) {
    Write-Host "[Status] vdsd service: $($Service.Status)"
  } else {
    Write-Warning "The vdsd service is not installed."
  }
  exit 0
}

if (!(Test-VdsAdministrator)) {
  throw "Driver and service installation requires an Administrator PowerShell."
}

$DaemonPath = Join-Path $BuildDirectory "$Configuration\vdsd.exe"
$ControlPath = Join-Path $BuildDirectory "$Configuration\vdsctl.exe"
if (!(Test-Path -LiteralPath $DaemonPath -PathType Leaf) -or
    !(Test-Path -LiteralPath $ControlPath -PathType Leaf)) {
  throw "Build output is missing. Run .\build-windows.ps1 first."
}

Write-Warning "Installing USB/IP restarts USB 3 hubs and may briefly disconnect USB devices."
Write-Host "Downloading and verifying signed USB/IP and HidHide installers..."
$DownloadResult = Invoke-VdsDependencyScript `
  -DependencyMode Download `
  -DownloadDirectory $DependencyDirectory `
  -LogPath $DependencyLog
if ($DownloadResult -ne 0) {
  throw "Dependency download failed with exit code $DownloadResult. Log: $DependencyLog"
}

Write-Host "Installing USB/IP and HidHide..."
$InstallResult = Invoke-VdsDependencyScript `
  -DependencyMode Install `
  -DownloadDirectory $DependencyDirectory `
  -LogPath $DependencyLog
if ($InstallResult -notin @(0, 3010)) {
  throw "Dependency install failed with exit code $InstallResult. Log: $DependencyLog"
}

$VisualStudioPath = Get-VdsVisualStudioPath
$CMake = Get-VdsCMakePath -VisualStudioPath $VisualStudioPath
Write-Host "Installing vDS and registering its automatic service..."
& $CMake --install $BuildDirectory --config $Configuration
if ($LASTEXITCODE -ne 0) {
  throw "vDS install failed with exit code $LASTEXITCODE."
}

$GuiSource = Join-Path $PSScriptRoot "out\gui\VdsGui.exe"
if (Test-Path -LiteralPath $GuiSource -PathType Leaf) {
  $ProgramFiles64 = [Environment]::GetEnvironmentVariable("ProgramW6432")
  if ([string]::IsNullOrWhiteSpace($ProgramFiles64)) {
    $ProgramFiles64 = $env:ProgramFiles
  }
  $VdsInstallDirectory = Join-Path $ProgramFiles64 "vDS"
  $GuiDestination = Join-Path $VdsInstallDirectory "VdsGui.exe"
  try {
    Copy-Item -LiteralPath $GuiSource -Destination $GuiDestination -Force
    $SourceMarker = Join-Path $VdsInstallDirectory "vds-source-root.txt"
    [System.IO.File]::WriteAllText(
      $SourceMarker,
      $PSScriptRoot,
      (New-Object System.Text.UTF8Encoding($false))
    )
    $ProgramsDirectory = [Environment]::GetFolderPath("CommonPrograms")
    $ShortcutDirectory = Join-Path $ProgramsDirectory "vDS"
    New-Item -ItemType Directory -Force -Path $ShortcutDirectory | Out-Null
    $ShortcutPath = Join-Path $ShortcutDirectory "vDS Control Center.lnk"
    $Shell = New-Object -ComObject WScript.Shell
    $Shortcut = $Shell.CreateShortcut($ShortcutPath)
    $Shortcut.TargetPath = $GuiDestination
    $Shortcut.WorkingDirectory = $VdsInstallDirectory
    $Shortcut.Description = "Bluetooth DualSense to virtual USB controller"
    $Shortcut.Save()
    Write-Host "Installed vDS Control Center: $GuiDestination"
  } catch {
    Write-Warning "Could not update the GUI or Start menu shortcut: $($_.Exception.Message)"
  }
}

if ($InstallResult -eq 3010) {
  Write-Warning "A restart is required. The vdsd service will start after reboot."
} else {
  Start-Service -Name vdsd
  (Get-Service -Name vdsd).WaitForStatus("Running", [TimeSpan]::FromSeconds(15))
  Write-Host "Installation complete. The vdsd service is running."
}
