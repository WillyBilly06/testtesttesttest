@echo off
REM Flash the lboshuizen SDIO OTA tool to P4 on COMx (default COM7), then monitor.
REM This will OTA-flash our C6 binary via SDIO without WiFi (no contention).
setlocal
if "%1"=="" (set PORT=COM7) else (set PORT=%1)
set PYTHONIOENCODING=utf-8
set PYTHONUTF8=1
call "D:\Bluetooth Project - Copy\esp-idf\export.bat" >NUL 2>&1
if errorlevel 1 exit /b 1
echo === Flashing c6_sdio_ota to %PORT% ===
idf.py -C "%~dp0c6_sdio_ota" -p %PORT% flash
if errorlevel 1 exit /b 1
echo === FLASH SUCCESSFUL ===
echo === Opening monitor (Ctrl-]  to exit) ===
idf.py -C "%~dp0c6_sdio_ota" -p %PORT% monitor
endlocal
