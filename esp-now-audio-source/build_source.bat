@echo off
chcp 65001 >nul
set PYTHONIOENCODING=utf-8

echo === Building ESP-NOW Audio Source ===
cd /d "D:\ESP32-Code\esp-now-audio-source"
call "D:\Bluetooth Project - Copy\esp-idf\export.bat"

idf.py build

if %ERRORLEVEL% EQU 0 (
    echo === BUILD SUCCESSFUL ===
) else (
    echo === BUILD FAILED ===
    exit /b 1
)
