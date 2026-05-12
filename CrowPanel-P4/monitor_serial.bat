@echo off
setlocal
if "%1"=="" (set PORT=COM7) else (set PORT=%1)
if "%2"=="" (set DUR=90) else (set DUR=%2)
call "D:\Bluetooth Project - Copy\esp-idf\export.bat" >NUL 2>&1
python "%~dp0read_serial.py" %PORT% 115200 %DUR%
endlocal
