@echo off
chcp 65001 >nul
set PYTHONIOENCODING=utf-8
call "D:\Bluetooth Project - Copy\esp-idf\export.bat" >nul 2>&1
cd /d "D:\ESP32-Code\CrowPanel-P4\c6_firmware"
idf.py -p COM13 monitor
