@echo off
REM Flash-only variant (no monitor). After flash, the OTA app reboots and starts running.
setlocal
if "%1"=="" (set PORT=COM7) else (set PORT=%1)
set PYTHONIOENCODING=utf-8
set PYTHONUTF8=1
call "D:\Bluetooth Project - Copy\esp-idf\export.bat" >NUL 2>&1
if errorlevel 1 exit /b 1
idf.py -C "%~dp0c6_sdio_ota" -p %PORT% flash
if errorlevel 1 exit /b 1
echo === FLASH SUCCESSFUL ===
endlocal
