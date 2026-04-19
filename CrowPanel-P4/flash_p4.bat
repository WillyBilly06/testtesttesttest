@echo off
chcp 65001 >nul
set PYTHONIOENCODING=utf-8
cd /d D:\ESP32-Code\CrowPanel-P4\factory_project
call "D:\Bluetooth Project - Copy\esp-idf\export.bat" >nul 2>&1
echo === Flashing P4 firmware on COM13 ===
idf.py -p COM13 flash
if errorlevel 1 (
    echo === FLASH FAILED ===
    exit /b 1
)
echo === FLASH SUCCESSFUL ===
