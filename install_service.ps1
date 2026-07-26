# SPDX-License-Identifier: MIT

param(
  [string]$ServiceName = "vdsd",
  [string]$DisplayName = "vDS Daemon",
  [string]$Description = "vDS userspace daemon",
  [Parameter(Mandatory = $true)]
  [string]$BinaryPath,
  [ValidateSet("Automatic", "Manual", "Disabled")]
  [string]$StartupType = "Automatic"
)

$ErrorActionPreference = "Stop"

function Convert-VdsServiceStartType {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Value
  )

  switch ($Value) {
    "Automatic" { return "auto" }
    "Manual" { return "demand" }
    "Disabled" { return "disabled" }
  }
}

function Invoke-VdsSc {
  param(
    [Parameter(Mandatory = $true)]
    [string[]]$Arguments
  )

  $Output = & sc.exe @Arguments 2>&1
  if ($LASTEXITCODE -ne 0) {
    throw "sc.exe $($Arguments -join ' ') failed: $Output"
  }
}

function Test-VdsServiceExists {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Name
  )

  $null = & sc.exe query $Name 2>$null
  return $LASTEXITCODE -eq 0
}

$ResolvedBinary = [System.IO.Path]::GetFullPath($BinaryPath)
$QuotedBinary = '"' + $ResolvedBinary + '"'
$ScStartType = Convert-VdsServiceStartType -Value $StartupType

if (Test-VdsServiceExists -Name $ServiceName) {
  Write-Host "Updating Windows service $ServiceName"
  Invoke-VdsSc -Arguments @(
    "config",
    $ServiceName,
    "binPath=",
    $QuotedBinary,
    "start=",
    $ScStartType,
    "DisplayName=",
    $DisplayName
  )
} else {
  Write-Host "Registering Windows service $ServiceName"
  Invoke-VdsSc -Arguments @(
    "create",
    $ServiceName,
    "binPath=",
    $QuotedBinary,
    "start=",
    $ScStartType,
    "DisplayName=",
    $DisplayName
  )
}

Invoke-VdsSc -Arguments @("description", $ServiceName, $Description)
