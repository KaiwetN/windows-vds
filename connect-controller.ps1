# SPDX-License-Identifier: MIT

[CmdletBinding()]
param(
  [string]$Address = "",
  [ValidateSet("auto", "ds5", "dse")]
  [string]$Profile = "auto",
  [ValidateSet("auto", "0", "1", "2", "3")]
  [string]$Port = "auto",
  [string]$ToolDirectory = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Get-VdsControlPath {
  if (![string]::IsNullOrWhiteSpace($ToolDirectory)) {
    $ExplicitPath = Join-Path `
      ([System.IO.Path]::GetFullPath($ToolDirectory)) "vdsctl.exe"
    if (Test-Path -LiteralPath $ExplicitPath -PathType Leaf) {
      return $ExplicitPath
    }
    throw "vdsctl.exe was not found in the requested directory: $ExplicitPath"
  }

  $ProgramFiles64 = [Environment]::GetEnvironmentVariable("ProgramW6432")
  if ([string]::IsNullOrWhiteSpace($ProgramFiles64)) {
    $ProgramFiles64 = $env:ProgramFiles
  }
  $Candidates = @(
    (Join-Path $ProgramFiles64 "vDS\vdsctl.exe"),
    (Join-Path $PSScriptRoot "out\build\windows\Release\vdsctl.exe"),
    (Join-Path $PSScriptRoot "out\build\windows\Debug\vdsctl.exe")
  )
  foreach ($Candidate in $Candidates) {
    if (Test-Path -LiteralPath $Candidate -PathType Leaf) {
      return $Candidate
    }
  }
  throw "vdsctl.exe was not found. Run build-windows.ps1 and setup-windows.ps1 first."
}

function Invoke-VdsControl {
  param(
    [Parameter(Mandatory = $true)][string]$ControlPath,
    [Parameter(Mandatory = $true)][string[]]$Arguments
  )

  $Output = @(& $ControlPath @Arguments 2>&1)
  if ($LASTEXITCODE -ne 0) {
    throw "vdsctl failed: $($Output -join [Environment]::NewLine)"
  }
  return $Output
}

function ConvertFrom-VdsJsonLines {
  param([object[]]$Lines)

  $Objects = @()
  foreach ($Line in $Lines) {
    $Text = $Line.ToString().Trim()
    if ($Text.Length -gt 0) {
      $Objects += $Text | ConvertFrom-Json
    }
  }
  return $Objects
}

$ControlPath = Get-VdsControlPath
$Service = Get-Service -Name vdsd -ErrorAction SilentlyContinue
if (!$Service) {
  throw "The vdsd service is not installed. Run setup-windows.ps1 -Mode Install as Administrator."
}
if ($Service.Status -ne "Running") {
  try {
    Start-Service -Name vdsd
    $Service.WaitForStatus("Running", [TimeSpan]::FromSeconds(15))
  } catch {
    throw "Could not start vdsd. Start it as Administrator and retry: $($_.Exception.Message)"
  }
}

$Targets = @(ConvertFrom-VdsJsonLines -Lines `
  (Invoke-VdsControl -ControlPath $ControlPath -Arguments @("list-targets")))
if ($Targets.Count -eq 0) {
  throw "No paired DualSense was found. Pair it in Windows Bluetooth settings, then press PS."
}

$Selected = $null
if (![string]::IsNullOrWhiteSpace($Address)) {
  $NormalizedAddress = $Address.Replace("-", ":").ToLowerInvariant()
  $Selected = @($Targets | Where-Object {
      $_.address.ToLowerInvariant() -eq $NormalizedAddress
    }) | Select-Object -First 1
  if (!$Selected) {
    throw "No paired DualSense was found at address $Address."
  }
} else {
  $OnlineTargets = @($Targets | Where-Object { $_.online })
  if ($OnlineTargets.Count -eq 1) {
    $Selected = $OnlineTargets[0]
  } elseif ($OnlineTargets.Count -gt 1) {
    $Choices = $OnlineTargets | ForEach-Object { "$($_.address) $($_.name)" }
    throw "Multiple online controllers found. Select one with -Address: $($Choices -join ', ')"
  } elseif ($Targets.Count -eq 1) {
    $Selected = $Targets[0]
  } else {
    $Choices = $Targets | ForEach-Object { "$($_.address) $($_.name)" }
    throw "Multiple paired controllers found. Select one with -Address: $($Choices -join ', ')"
  }
}

if ($Selected.registered) {
  Write-Host "Controller already registered: $($Selected.name) [$($Selected.address)]"
  $Status = Invoke-VdsControl -ControlPath $ControlPath -Arguments @("list")
  $Status | ForEach-Object { Write-Host $_ }
  exit 0
}

$AttachArguments = @(
  "attach", $Selected.address
)
if ($Port -ne "auto") {
  $AttachArguments += @("--ports", $Port)
}
if ($Profile -ne "auto") {
  $AttachArguments += @("--profile", $Profile)
}
$Reply = @(ConvertFrom-VdsJsonLines -Lines `
  (Invoke-VdsControl -ControlPath $ControlPath -Arguments $AttachArguments))
if ($Reply.Count -ne 1 -or !$Reply[0].OK) {
  $Reason = if ($Reply.Count -gt 0) { $Reply[0].error } else { "unknown error" }
  throw "Failed to register controller: $Reason"
}

$PortDescription = if ($Port -eq "auto") { "automatic" } else { $Port }
Write-Host "Registered: $($Selected.name) [$($Selected.address)] -> virtual USB port $PortDescription"
if (!$Selected.online) {
  Write-Warning "The controller is offline. Press PS and vDS will create the virtual wired device."
} else {
  Write-Host "A virtual wired DualSense should appear in Windows within a few seconds."
}
