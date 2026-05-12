@echo off
chcp 65001 >nul
set PYTHONIOENCODING=utf-8

set PORT=%1
if "%PORT%"=="" set PORT=COM13

echo === Flashing C6 Firmware on %PORT% ===
call "D:\Bluetooth Project - Copy\esp-idf\export.bat" >nul 2>&1

cd /d "D:\ESP32-Code\CrowPanel-P4\c6_firmware"

python -m esptool --chip esp32c6 -p %PORT% -b 460800 --before default_reset --after hard_reset ^
  write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m ^
  0x0     build\bootloader\bootloader.bin ^
  0x8000  build\partition_table\partition-table.bin ^
  0xd000  build\ota_data_initial.bin ^
  0x10000 build\crowpanel_c6_firmware.bin

if %ERRORLEVEL% EQU 0 (
    echo === FLASH SUCCESSFUL ===
) else (
    echo === FLASH FAILED ===
    exit /b 1
)
