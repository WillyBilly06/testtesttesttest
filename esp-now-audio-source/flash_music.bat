@echo off
chcp 65001 >nul
set PYTHONIOENCODING=utf-8
cd /d D:\ESP32-Code\esp-now-audio-source
call "D:\Bluetooth Project - Copy\esp-idf\export.bat" >nul 2>&1
echo === Writing music to COM4 at 0x400000 ===
python tools\write_music.py COM4 data\music.mp3
if errorlevel 1 (
    echo === MUSIC FLASH FAILED ===
    exit /b 1
)
echo === MUSIC FLASH SUCCESSFUL ===
