# SPDX-License-Identifier: MIT

[CmdletBinding()]
param(
  [ValidateSet("Debug", "Release")]
  [string]$Configuration = "Release",
  [string]$BuildDirectory = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Get-VdsVisualStudioPath {
  $VsWhere = Join-Path ${env:ProgramFiles(x86)} `
    "Microsoft Visual Studio\Installer\vswhere.exe"
  if (!(Test-Path -LiteralPath $VsWhere -PathType Leaf)) {
    throw "vswhere.exe was not found. Install Visual Studio 2022 C++ Build Tools and vcpkg."
  }

  $VsWhereArguments = @(
    "-latest",
    "-products", "*",
    "-requires", "Microsoft.VisualStudio.Component.Vcpkg",
    "-property", "installationPath"
  )
  $InstallPaths = @(& $VsWhere @VsWhereArguments)
  if ($LASTEXITCODE -ne 0 -or $InstallPaths.Count -eq 0) {
    throw "Visual Studio 2022 with the vcpkg component was not found."
  }
  return $InstallPaths[0].Trim()
}

function Get-VdsCMakePath {
  param([Parameter(Mandatory = $true)][string]$VisualStudioPath)

  $Command = Get-Command cmake.exe -ErrorAction SilentlyContinue
  if ($Command) {
    return $Command.Source
  }

  $BundledCMake = Join-Path $VisualStudioPath `
    "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  if (!(Test-Path -LiteralPath $BundledCMake -PathType Leaf)) {
    throw "cmake.exe was not found. Install the Visual Studio CMake component."
  }
  return $BundledCMake
}

function Invoke-VdsCommand {
  param(
    [Parameter(Mandatory = $true)][string]$FilePath,
    [Parameter(Mandatory = $true)][string[]]$Arguments
  )

  & $FilePath @Arguments
  if ($LASTEXITCODE -ne 0) {
    throw "Command failed with exit code $LASTEXITCODE`: $FilePath"
  }
}

$SourceDirectory = $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
  $BuildDirectory = Join-Path $SourceDirectory "out\build\windows"
} else {
  $BuildDirectory = [System.IO.Path]::GetFullPath($BuildDirectory)
}

$VisualStudioPath = Get-VdsVisualStudioPath
$CMake = Get-VdsCMakePath -VisualStudioPath $VisualStudioPath
$VcpkgToolchain = Join-Path $VisualStudioPath `
  "VC\vcpkg\scripts\buildsystems\vcpkg.cmake"
if (!(Test-Path -LiteralPath $VcpkgToolchain -PathType Leaf)) {
  throw "The vcpkg CMake toolchain was not found: $VcpkgToolchain"
}

Write-Host "Configuring vDS ($Configuration)..."
$ConfigureArguments = @(
  "-S", $SourceDirectory,
  "-B", $BuildDirectory,
  "-G", "Visual Studio 17 2022",
  "-A", "x64",
  "-DINSTALL_SERVICE=YES"
)
$CMakeCache = Join-Path $BuildDirectory "CMakeCache.txt"
if (!(Test-Path -LiteralPath $CMakeCache -PathType Leaf)) {
  $ConfigureArguments += "-DCMAKE_TOOLCHAIN_FILE=$VcpkgToolchain"
}
Invoke-VdsCommand -FilePath $CMake -Arguments $ConfigureArguments

Write-Host "Building vDS..."
$BuildArguments = @(
  "--build", $BuildDirectory,
  "--config", $Configuration,
  "--parallel"
)
Invoke-VdsCommand -FilePath $CMake -Arguments $BuildArguments

$OutputDirectory = Join-Path $BuildDirectory $Configuration
$DotNetCommand = Get-Command dotnet.exe -ErrorAction SilentlyContinue
if (!$DotNetCommand) {
  throw "dotnet.exe was not found. Install the .NET 8 SDK."
}
$GuiProject = Join-Path $SourceDirectory "gui\VdsGui.csproj"
$GuiOutputDirectory = Join-Path $SourceDirectory "out\gui"
Write-Host "Publishing vDS Control Center..."
$PublishArguments = @(
  "publish", $GuiProject,
  "--configuration", $Configuration,
  "--runtime", "win-x64",
  "--self-contained", "false",
  "--output", $GuiOutputDirectory,
  "--nologo",
  "-p:PublishSingleFile=true",
  "-p:DebugSymbols=false",
  "-p:DebugType=None"
)
Invoke-VdsCommand -FilePath $DotNetCommand.Source -Arguments $PublishArguments

Write-Host "Build complete: $OutputDirectory"
Write-Host "GUI: $GuiOutputDirectory\VdsGui.exe"
Write-Host "Next: run .\setup-windows.ps1 -Mode Install as Administrator."
