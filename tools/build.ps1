$uv4 = 'E:\study_software\software\keil_v5\UV4\UV4.exe'
if (-not (Test-Path $uv4)) { Write-Host "NOT FOUND: $uv4"; exit 2 }
$root = Split-Path $PSScriptRoot -Parent
$proj = Join-Path $root 'USER\Firmware_F411.uvprojx'
$log = Join-Path $root 'USER\build_bbx.log'
Push-Location (Join-Path $root 'USER')
& $uv4 -b $proj -j0 -o $log | Out-Null
Pop-Location
Get-Content $log -Tail 15
