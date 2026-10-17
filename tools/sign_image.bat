@echo off
REM ============================================================
REM sign_image.bat - One-click: axf -> bin -> signed image -> hex
REM
REM Usage (after building the Firmware project in Keil):
REM   tools\sign_image.bat
REM
REM Outputs (in USER\BIN\):
REM   Firmware_F411.bin        raw app binary
REM   Firmware_F411_image.bin  MCUBoot image (0x200 header + SHA256 TLV)
REM   Firmware_F411_image.hex  Intel HEX, base 0x08020000 (ready to flash)
REM ============================================================
setlocal

set "ROOT=%~dp0.."
set "AXF=%ROOT%\OBJ\Firmware_F411.axf"
set "BIN_DIR=%ROOT%\USER\BIN"
set "FROMELF=E:\study_software\software\keil_v5\ARM\ARMCC\Bin\fromelf.exe"

if not exist "%AXF%" (
    echo [ERROR] %AXF% not found. Build the Firmware project first.
    exit /b 1
)

if not exist "%BIN_DIR%" mkdir "%BIN_DIR%"

echo [1/3] fromelf: axf -^> bin
"%FROMELF%" --bin "%AXF%" -o "%BIN_DIR%\Firmware_F411.bin" || exit /b 1

echo [2/3] imgtool: add 0x200 header + SHA256 TLV
imgtool sign --pad-header --align 8 -H 0x200 -S 0x1FA00 -v 1.0.0 ^
    "%BIN_DIR%\Firmware_F411.bin" "%BIN_DIR%\Firmware_F411_image.bin" || exit /b 1

echo [3/3] bin2hex: image -^> hex (base 0x08020000)
python "%ROOT%\tools\bin2hex.py" ^
    "%BIN_DIR%\Firmware_F411_image.bin" ^
    "%BIN_DIR%\Firmware_F411_image.hex" 0x08020000 || exit /b 1

echo.
echo Done. Flash  ^"%BIN_DIR%\Firmware_F411_image.hex^"  at 0x08020000.
endlocal
