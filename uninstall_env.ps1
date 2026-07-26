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

function Remove-VdsPathListEntry {
  param(
    [string[]]$Entries,
    [string]$Path
  )

  $NormalizedPath = $Path.TrimEnd("\")
  return @(
    $Entries | Where-Object {
      -not $_.TrimEnd("\").Equals(
        $NormalizedPath,
        [System.StringComparison]::OrdinalIgnoreCase
      )
    }
  )
}

function Join-VdsPathList {
  param(
    [string[]]$Entries
  )

  return ($Entries | Where-Object { $_.Length -gt 0 }) -join ";"
}

function Resolve-VdsPathText {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Path
  )

  $TrimmedPath = $Path.Trim().Trim('"').TrimEnd("\")
  try {
    return [System.IO.Path]::GetFullPath($TrimmedPath).TrimEnd("\")
  } catch {
    return $TrimmedPath
  }
}

if ([string]::IsNullOrWhiteSpace($InstallPath)) {
  throw "path argument is empty"
}
if ($InstallPath.Contains(";")) {
  throw "path argument must not contain ';'"
}

$ResolvedPath = Resolve-VdsPathText -Path $InstallPath
$Target = Get-VdsEnvironmentTarget
$PathValue = [System.Environment]::GetEnvironmentVariable("Path", $Target)
$Entries = @(Split-VdsPathList -Value $PathValue)
$Entries = @(Remove-VdsPathListEntry -Entries $Entries -Path $ResolvedPath)
[System.Environment]::SetEnvironmentVariable(
  "Path",
  (Join-VdsPathList -Entries $Entries),
  $Target
)

$ProcessEntries = @(Split-VdsPathList -Value $env:Path)
$ProcessEntries = @(
  Remove-VdsPathListEntry -Entries $ProcessEntries -Path $ResolvedPath
)
$env:Path = Join-VdsPathList -Entries $ProcessEntries

Write-Output "Removed from $Target PATH: $ResolvedPath"
