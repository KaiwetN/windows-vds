# SPDX-License-Identifier: MIT

param(
  [Parameter(Mandatory = $true, Position = 0)]
  [string]$InstallPath
)

$ErrorActionPreference = "Stop"

function Test-VdsElevated {
  $Identity = [Security.Principal.WindowsIdentity]::GetCurrent()
  $Principal = New-Object Security.Principal.WindowsPrincipal($Identity)
  return $Principal.IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator
  )
}

function Get-VdsEnvironmentTarget {
  if (Test-VdsElevated) {
    return [System.EnvironmentVariableTarget]::Machine
  }
  return [System.EnvironmentVariableTarget]::User
}

function Split-VdsPathList {
  param(
    [string]$Value
  )

  if ([string]::IsNullOrWhiteSpace($Value)) {
    return @()
  }

  return $Value -split ";" |
    ForEach-Object { $_.Trim() } |
    Where-Object { $_.Length -gt 0 }
}

function Test-VdsPathListContains {
  param(
    [string[]]$Entries,
    [string]$Path
  )

  $NormalizedPath = $Path.TrimEnd("\")
  foreach ($Entry in $Entries) {
    if ($Entry.TrimEnd("\").Equals(
        $NormalizedPath,
        [System.StringComparison]::OrdinalIgnoreCase
      )) {
      return $true
    }
  }
  return $false
}

function Join-VdsPathList {
  param(
    [string[]]$Entries
  )

  return ($Entries | Where-Object { $_.Length -gt 0 }) -join ";"
}

if ([string]::IsNullOrWhiteSpace($InstallPath)) {
  throw "path argument is empty"
}
if ($InstallPath.Contains(";")) {
  throw "path argument must not contain ';'"
}

$ResolvedPath = [System.IO.Path]::GetFullPath($InstallPath).TrimEnd("\")
$Target = Get-VdsEnvironmentTarget
$PathValue = [System.Environment]::GetEnvironmentVariable("Path", $Target)
$Entries = @(Split-VdsPathList -Value $PathValue)

if (!(Test-VdsPathListContains -Entries $Entries -Path $ResolvedPath)) {
  $Entries += $ResolvedPath
  [System.Environment]::SetEnvironmentVariable(
    "Path",
    (Join-VdsPathList -Entries $Entries),
    $Target
  )
}

$ProcessEntries = @(Split-VdsPathList -Value $env:Path)
if (!(Test-VdsPathListContains -Entries $ProcessEntries -Path $ResolvedPath)) {
  $ProcessEntries += $ResolvedPath
  $env:Path = Join-VdsPathList -Entries $ProcessEntries
}

Write-Output "Added to $Target PATH: $ResolvedPath"
