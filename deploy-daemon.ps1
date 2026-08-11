$ErrorActionPreference = 'Stop'
$log = Join-Path $env:TEMP 'vds-deploy-daemon.log'
Start-Transcript -Path $log -Force
$src = 'C:\Users\Administrator\Documents\New project\windows-vds\out\build\windows\Release'
$dst = 'C:\Program Files\vDS'

sc.exe stop vdsd | Out-Null
Start-Sleep -Seconds 2

Copy-Item (Join-Path $src 'vdsd.exe') -Destination $dst -Force
Copy-Item (Join-Path $src 'vdsctl.exe') -Destination $dst -Force

sc.exe start vdsd | Out-Null
Start-Sleep -Seconds 2
Stop-Transcript
