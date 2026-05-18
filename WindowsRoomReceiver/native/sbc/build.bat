@echo off
rem ---------------------------------------------------------------------------
rem Builds sbc_decoder_native.dll (x64) using MSVC Build Tools 2022.
rem Output: native\sbc\out\sbc_decoder_native.dll
rem
rem Sources (all in this folder):
rem   src\sbc.c                 - google/libsbc decoder/encoder (Apache 2.0)
rem   src\bits.c                - google/libsbc bitstream helpers (Apache 2.0)
rem   src\sbc_decoder_native.c  - thin C wrapper exporting cdecl entry points
rem ---------------------------------------------------------------------------

setlocal EnableExtensions
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VCVARS%" goto :no_vcvars
call "%VCVARS%" >nul
if errorlevel 1 goto :vc_failed

pushd "%~dp0"
if not exist out mkdir out

cl.exe /nologo /LD /O2 /W3 /MD /TC /std:c17 /experimental:c11atomics ^
    /D_CRT_SECURE_NO_WARNINGS /DSBC_NATIVE_DLL=1 ^
    /FI"sbc_msvc_compat.h" ^
    /Iinclude /Isrc ^
    src\sbc.c src\bits.c src\sbc_decoder_native.c ^
    /Fe:out\sbc_decoder_native.dll ^
    /Fo:out\ ^
    /link /MACHINE:X64

set "RC=%ERRORLEVEL%"
popd
if not "%RC%"=="0" goto :build_failed

echo [build.bat] Built out\sbc_decoder_native.dll
exit /b 0

:no_vcvars
echo [build.bat] vcvars64.bat not found at:
echo     %VCVARS%
echo Install the "Desktop development with C++" workload, then re-run.
exit /b 1

:vc_failed
echo [build.bat] Failed to initialize MSVC environment.
exit /b 1

:build_failed
echo [build.bat] Build failed with exit code %RC%.
exit /b %RC%
