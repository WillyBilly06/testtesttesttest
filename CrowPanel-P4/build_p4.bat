@echo off
chcp 65001 >nul
set PYTHONIOENCODING=utf-8

echo === Building P4 Firmware ===
cd /d "D:\Bluetooth Project - Copy\esp-idf"
call export.bat

cd /d "D:\ESP32-Code\CrowPanel-P4\factory_project"
idf.py build

if %ERRORLEVEL% EQU 0 (
    echo === BUILD SUCCESSFUL ===
) else (
    echo === BUILD FAILED ===
)
