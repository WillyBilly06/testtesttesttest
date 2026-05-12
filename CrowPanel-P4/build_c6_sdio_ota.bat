@echo off
REM Build the lboshuizen SDIO OTA tool (no WiFi, no contention) for our C6 binary.
setlocal
call "D:\Bluetooth Project - Copy\esp-idf\export.bat" >NUL 2>&1
if errorlevel 1 exit /b 1
idf.py -C "%~dp0c6_sdio_ota" set-target esp32p4
if errorlevel 1 exit /b 1
idf.py -C "%~dp0c6_sdio_ota" build
if errorlevel 1 exit /b 1
echo === BUILD SUCCESSFUL ===
endlocal
