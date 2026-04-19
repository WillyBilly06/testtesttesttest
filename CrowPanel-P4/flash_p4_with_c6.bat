@echo off
chcp 65001 >nul
set PYTHONIOENCODING=utf-8

echo === Flashing P4 Firmware + C6 Slave Binary on COM7 ===
cd /d "D:\Bluetooth Project - Copy\esp-idf"
call export.bat

cd /d "D:\ESP32-Code\CrowPanel-P4\factory_project"

REM Flash P4 firmware + C6 slave firmware in slave_fw partition at 0x810000
python -m esptool --chip esp32p4 -p COM7 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x2000 build\bootloader\bootloader.bin 0x8000 build\partition_table\partition-table.bin 0x10000 build\esp_brookesia_demo.bin 0x810000 ..\c6_firmware\build\crowpanel_c6_firmware.bin 0xa10000 build\storage.bin --force

if %ERRORLEVEL% EQU 0 (
    echo === FLASH SUCCESSFUL ===
) else (
    echo === FLASH FAILED ===
)
