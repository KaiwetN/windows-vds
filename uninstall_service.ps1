# SPDX-License-Identifier: MIT

param(
  [string]$ServiceName = "vdsd",
  [switch]$Stop
)

$ErrorActionPreference = "Stop"

function Test-VdsServiceExists {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Name
  )

  $null = & sc.exe query $Name 2>$null
  return $LASTEXITCODE -eq 0
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

if (!(Test-VdsServiceExists -Name $ServiceName)) {
  exit 0
}

if ($Stop) {
  & "$PSScriptRoot\stop_service.ps1" -ServiceName $ServiceName
}

Write-Host "Removing Windows service $ServiceName"
Invoke-VdsSc -Arguments @("delete", $ServiceName)
