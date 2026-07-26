# SPDX-License-Identifier: MIT

param(
  [string]$ServiceName = "vdsd",
  [int]$TimeoutSeconds = 20
)

$ErrorActionPreference = "Stop"

function Get-VdsService {
  param(
    [Parameter(Mandatory = $true)]
    [string]$Name
  )

  return Get-Service -Name $Name -ErrorAction SilentlyContinue
}

$Service = Get-VdsService -Name $ServiceName
if (!$Service) {
  exit 0
}

if ($Service.Status -ne "Stopped") {
  Write-Host "Stopping Windows service $ServiceName"
  Stop-Service -Name $ServiceName -ErrorAction SilentlyContinue
}

$Deadline = (Get-Date).AddSeconds($TimeoutSeconds)
while ((Get-Date) -lt $Deadline) {
  $Service = Get-VdsService -Name $ServiceName
  if (!$Service -or $Service.Status -eq "Stopped") {
    exit 0
  }
  Start-Sleep -Seconds 1
}

throw "timed out while waiting for Windows service $ServiceName to stop"
