@echo off
REM Erase + flash P4-as-C6-flasher bridge to a P4. No monitor (auto-only).
REM Usage: flash_p4_flasher_only.bat [COMx]   default COM7
setlocal
if "%1"=="" (set PORT=COM7) else (set PORT=%1)
call "D:\Bluetooth Project - Copy\esp-idf\export.bat" >NUL 2>&1
if errorlevel 1 exit /b 1
echo === Erasing %PORT% ===
idf.py -C "%~dp0p4_c6_flasher" -p %PORT% erase-flash
if errorlevel 1 exit /b 1
echo === Flashing P4-as-C6-flasher to %PORT% ===
idf.py -C "%~dp0p4_c6_flasher" -p %PORT% flash
if errorlevel 1 exit /b 1
echo === FLASH SUCCESSFUL ===
endlocal
