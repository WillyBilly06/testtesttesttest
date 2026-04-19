@echo off
chcp 65001 >nul
set PYTHONIOENCODING=utf-8
cd /d D:\ESP32-Code\CrowPanel-P4\c6_firmware
call "D:\Bluetooth Project - Copy\esp-idf\export.bat" >nul 2>&1
echo === Building C6 firmware ===
idf.py build
if errorlevel 1 (
    echo === BUILD FAILED ===
    exit /b 1
)
echo === BUILD SUCCESSFUL ===
