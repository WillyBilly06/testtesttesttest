@echo off
REM Build the P4-as-C6-flasher bridge (no flash, no monitor).
setlocal
call "D:\Bluetooth Project - Copy\esp-idf\export.bat" >NUL 2>&1
if errorlevel 1 (
    echo [ERROR] failed to source ESP-IDF export.bat
    exit /b 1
)
idf.py -C "%~dp0p4_c6_flasher" set-target esp32p4
if errorlevel 1 exit /b 1
idf.py -C "%~dp0p4_c6_flasher" build
if errorlevel 1 exit /b 1
echo === BUILD SUCCESSFUL ===
endlocal
