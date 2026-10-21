$root = Split-Path $PSScriptRoot -Parent
$fromelf = 'E:\study_software\software\keil_v5\ARM\ARMCC\Bin\fromelf.exe'
$bin = Join-Path $root 'USER\BIN'
& $fromelf --bin (Join-Path $root 'OBJ\Firmware_F411.axf') -o (Join-Path $bin 'Firmware_F411.bin')
if ($LASTEXITCODE -ne 0) { throw "fromelf failed" }
imgtool sign --pad-header --align 8 -H 0x200 -S 0x1FA00 -v 1.0.21 (Join-Path $bin 'Firmware_F411.bin') (Join-Path $bin 'Firmware_F411_image_v1.0.21.bin')
if ($LASTEXITCODE -ne 0) { throw "imgtool failed" }
python (Join-Path $root 'tools\bin2hex.py') (Join-Path $bin 'Firmware_F411_image_v1.0.21.bin') (Join-Path $bin 'Firmware_F411_image_v1.0.21.hex') 0x08020000
if ($LASTEXITCODE -ne 0) { throw "bin2hex failed" }
Get-Item (Join-Path $bin 'Firmware_F411_image_v1.0.21.bin') | Select-Object Length, LastWriteTime
