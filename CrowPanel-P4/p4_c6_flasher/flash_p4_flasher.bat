@echo off
REM Build, ERASE, flash and monitor the P4-as-C6-flasher app.
REM Usage:  flash_p4_flasher.bat [COMx]
REM Example: flash_p4_flasher.bat COM9
REM
REM The full erase guarantees any leftover descriptor/OTA state from the
REM previous (factory) firmware is gone, so Windows enumerates the new
REM TinyUSB device cleanly with its real product string.

setlocal
if "%1"=="" (
    set PORT=COM9
) else (
    set PORT=%1
)

call "D:\Bluetooth Project - Copy\esp-idf\export.bat"
if errorlevel 1 (
    echo [ERROR] failed to source ESP-IDF export.bat
    exit /b 1
)

idf.py -C "%~dp0" set-target esp32p4
if errorlevel 1 exit /b 1

idf.py -C "%~dp0" build
if errorlevel 1 exit /b 1

echo.
echo === Erasing entire flash on %PORT% (wipes stale factory data) ===
idf.py -C "%~dp0" -p %PORT% erase-flash
if errorlevel 1 exit /b 1

echo.
echo === Flashing P4-as-C6-flasher to %PORT% ===
idf.py -C "%~dp0" -p %PORT% flash
if errorlevel 1 exit /b 1

echo.
echo === Opening monitor on %PORT% (Ctrl-]  to exit) ===
echo   You should see "P4-as-C6-flasher v1 booting" within a second.
echo   A NEW COM port will appear on the OTHER USB-C connector.
echo.
idf.py -C "%~dp0" -p %PORT% monitor
endlocal
