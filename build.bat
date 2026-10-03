@echo off
setlocal
REM ============================================================
REM  AIMP Prevent Resampling - build script for Windows (Visual Studio 2022)
REM
REM  Usage:
REM    build.bat                      builds 32 and 64 bit (the SDK is downloaded automatically)
REM    build.bat install              builds and installs to %ProgramFiles%\AIMP\Plugins
REM    build.bat "C:\aimp_sdk\Sources\Cpp" [install]   use your own SDK copy
REM
REM  Result:   dist\PreventResampling\PreventResampling.dll       (32 bit)
REM           dist\PreventResampling\x64\PreventResampling.dll   (64 bit)
REM  "install" copies the PreventResampling folder (close AIMP first, run as administrator).
REM ============================================================

set "SDKARG="
set "MODE=%~1"
if /i "%MODE%"=="install" goto :args_done
if "%MODE%"=="" goto :args_done
set "SDK=%~1"
set "MODE=%~2"
if not exist "%SDK%\apiPlugin.h" goto :badsdk
set "SDK=%SDK:\=/%"
set "SDKARG=-DAIMP_SDK_DIR=%SDK%"
:args_done

where cmake >nul 2>&1
if errorlevel 1 (
    echo [ERROR] CMake was not found. Download: https://cmake.org/download/
    exit /b 1
)

for %%A in (x64 Win32) do (
    echo.
    echo === %%A ===
    cmake -S "%~dp0." -B "%~dp0build\%%A" -G "Visual Studio 17 2022" -A %%A %SDKARG%
    if errorlevel 1 goto :cfgfail
    cmake --build "%~dp0build\%%A" --config Release
    if errorlevel 1 goto :buildfail
    cmake --install "%~dp0build\%%A" --config Release --prefix "%~dp0dist"
    if errorlevel 1 goto :buildfail
)

echo.
echo Done: %~dp0dist\PreventResampling
if /i "%MODE%"=="install" goto :install
echo To install, copy the folder dist\PreventResampling to AIMP\Plugins\ or run "build.bat install".
exit /b 0

:install
set "TARGET=%ProgramFiles%\AIMP\Plugins"
if not exist "%TARGET%" (
    echo [ERROR] %TARGET% not found. Please copy dist\PreventResampling to AIMP\Plugins manually.
    exit /b 1
)
xcopy /e /i /y "%~dp0dist\PreventResampling" "%TARGET%\PreventResampling" >nul
if errorlevel 1 goto :copyfail
echo Installed to %TARGET%\PreventResampling
exit /b 0

:cfgfail
echo [ERROR] Configuration failed. Are Visual Studio 2022 (C++ workload) and Git installed?
exit /b 1

:buildfail
echo [ERROR] Build failed. Please copy the message above when reporting the problem.
exit /b 1

:copyfail
echo [ERROR] Copying to %TARGET% failed. Close AIMP and run this script as administrator.
exit /b 1

:badsdk
echo [ERROR] apiPlugin.h not found in: %SDK%
echo The path must point to the "Sources\Cpp" folder of the extracted AIMP SDK (v6.00 or newer).
exit /b 1
