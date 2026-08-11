$ErrorActionPreference = 'Stop'
$log = Join-Path $env:TEMP 'vds-deploy.log'
Start-Transcript -Path $log -Force
$src = 'C:\Users\Administrator\Documents\New project\windows-vds\gui\bin\Release\net8.0-windows'
$targets = @(
    'C:\Users\Administrator\Documents\New project\windows-vds\out\gui',
    'C:\Program Files\vDS'
)

Get-Process VdsGui -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

foreach ($target in $targets) {
    if (-not (Test-Path $target)) {
        New-Item -ItemType Directory -Path $target -Force | Out-Null
    }
    Copy-Item -Path (
        Join-Path $src 'VdsGui.exe'),
        (Join-Path $src 'VdsGui.dll'),
        (Join-Path $src 'VdsGui.deps.json'),
        (Join-Path $src 'VdsGui.runtimeconfig.json') -Destination $target -Force
    Copy-Item -Path (Join-Path $src 'NAudio*.dll') -Destination $target -Force
}

Start-Process -FilePath 'C:\Program Files\vDS\VdsGui.exe'
Stop-Transcript
