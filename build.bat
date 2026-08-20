@echo off
setlocal enabledelayedexpansion

echo ========================================
echo Building PiP Jumper
echo ========================================

set "TARGET_ARCH="
if defined VSCMD_ARG_TGT_ARCH set "TARGET_ARCH=%VSCMD_ARG_TGT_ARCH%"
if not defined TARGET_ARCH if defined Platform set "TARGET_ARCH=%Platform%"
if not defined TARGET_ARCH if "%PROCESSOR_ARCHITECTURE%"=="AMD64" set "TARGET_ARCH=x64"
if not defined TARGET_ARCH if "%PROCESSOR_ARCHITECTURE%"=="x86" set "TARGET_ARCH=x86"

if not defined TARGET_ARCH (
echo [ERROR] Cannot determine target architecture!
echo Please run this script from Visual Studio Developer Command Prompt.
pause
exit /b 1
)

echo Target Architecture: %TARGET_ARCH%
echo.

if "%TARGET_ARCH%"=="x86" goto build_x86
if "%TARGET_ARCH%"=="x64" goto build_x64

echo [ERROR] Unknown architecture: %TARGET_ARCH%
pause
exit /b 1

:build_x86
echo [1/2] Compiling for x86 (32-bit)...
cl /O1 /GS- /Gy /Gw /Oi- /c /Fo"pipjumperx86.obj" pipjumper.c
if errorlevel 1 goto error

echo.
echo [2/2] Linking for x86 (32-bit)...
link pipjumperx86.obj /SUBSYSTEM:WINDOWS /ENTRY:EntryPoint /NODEFAULTLIB /OPT:REF /OPT:ICF kernel32.lib user32.lib shell32.lib /MACHINE:X86 /OUT:pipjumperx86.exe
if errorlevel 1 goto error

echo.
echo ========================================
echo BUILD SUCCESSFUL! (x86 32-bit)
echo   - pipjumperx86.exe
echo ========================================
goto end

:build_x64
echo [1/2] Compiling for x64 (64-bit)...
cl /O1 /GS- /GL /Gy /Gw /c /Fo"pipjumperx64.obj" pipjumper.c
if errorlevel 1 goto error

echo.
echo [2/2] Linking for x64 (64-bit)...
link pipjumperx64.obj /SUBSYSTEM:WINDOWS /ENTRY:EntryPoint /NODEFAULTLIB /LTCG /OPT:REF /OPT:ICF kernel32.lib user32.lib shell32.lib /OUT:pipjumperx64.exe
if errorlevel 1 goto error

echo.
echo ========================================
echo BUILD SUCCESSFUL! (x64 64-bit)
echo   - pipjumperx64.exe
echo ========================================
goto end

:error
echo.
echo ========================================
echo BUILD FAILED!
echo ========================================

:end
rem pause