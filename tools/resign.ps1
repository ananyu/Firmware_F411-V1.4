param([string]$Ver = "1.0.22")
$root = Split-Path $PSScriptRoot -Parent
$fromelf = 'E:\study_software\software\keil_v5\ARM\ARMCC\Bin\fromelf.exe'
$bin = Join-Path $root 'USER\BIN'
& $fromelf --bin (Join-Path $root 'OBJ\Firmware_F411.axf') -o (Join-Path $bin 'Firmware_F411.bin')
if ($LASTEXITCODE -ne 0) { throw "fromelf failed" }
$img = Join-Path $bin ("Firmware_F411_image_v$Ver.bin")
$hex = Join-Path $bin ("Firmware_F411_image_v$Ver.hex")
imgtool sign --pad-header --align 8 -H 0x200 -S 0x1FA00 -v $Ver (Join-Path $bin 'Firmware_F411.bin') $img
if ($LASTEXITCODE -ne 0) { throw "imgtool failed" }
python (Join-Path $root 'tools\bin2hex.py') $img $hex 0x08020000
if ($LASTEXITCODE -ne 0) { throw "bin2hex failed" }
Get-Item $img | Select-Object Length, LastWriteTime
