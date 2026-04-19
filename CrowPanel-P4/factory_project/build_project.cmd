@echo off
setlocal
chcp 65001 >nul

set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"

set "IDF_PATH=D:\Bluetooth Project - Copy\esp-idf"
if not exist "%IDF_PATH%\export.bat" (
    echo ESP-IDF export.bat not found at "%IDF_PATH%"
    exit /b 1
)

pushd "%~dp0"
call "%IDF_PATH%\export.bat" || exit /b 1
idf.py reconfigure build || exit /b 1
popd
