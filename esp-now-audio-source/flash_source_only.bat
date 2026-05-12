@echo off
chcp 65001 >nul
set PYTHONIOENCODING=utf-8
cd /d D:\ESP32-Code\esp-now-audio-source
call "D:\Bluetooth Project - Copy\esp-idf\export.bat" >nul 2>&1
echo === Flashing Source firmware on COM4 ===
idf.py -p COM4 flash
if errorlevel 1 (
    echo === FLASH FAILED ===
    exit /b 1
)
echo === FLASH SUCCESSFUL ===
