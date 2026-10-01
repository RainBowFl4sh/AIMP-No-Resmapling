@echo off
setlocal
REM ============================================================
REM  AIMP AutoRate - Build-Skript (64-bit)
REM
REM  Aufruf:
REM    build.bat "C:\Pfad\zum\aimp_sdk\Sources\Cpp"
REM    build.bat "C:\Pfad\zum\aimp_sdk\Sources\Cpp" install
REM
REM  Alternativ Umgebungsvariable AIMP_SDK_DIR setzen und ohne Argument starten.
REM  "install" kopiert die DLL nach %ProgramFiles%\AIMP\Plugins\AutoRate
REM  (AIMP vorher beenden, Skript als Administrator starten).
REM ============================================================

set "SDK=%~1"
if "%SDK%"=="" set "SDK=%AIMP_SDK_DIR%"
if "%SDK%"=="" goto :nosdk

if not exist "%SDK%\apiPlugin.h" goto :badsdk

where cmake >nul 2>&1
if errorlevel 1 (
    echo [FEHLER] CMake wurde nicht gefunden. Installation: https://cmake.org/download/
    exit /b 1
)

REM Backslashes fuer CMake in Slashes umwandeln
set "SDK=%SDK:\=/%"

echo [1/2] Konfiguriere Projekt ...
cmake -S "%~dp0." -B "%~dp0build" -G "Visual Studio 17 2022" -A x64 -DAIMP_SDK_DIR="%SDK%"
if errorlevel 1 (
    echo [FEHLER] Konfiguration fehlgeschlagen. Ist Visual Studio 2022 mit C++-Workload installiert?
    exit /b 1
)

echo [2/2] Baue Release ...
cmake --build "%~dp0build" --config Release
if errorlevel 1 (
    echo [FEHLER] Build fehlgeschlagen. Meldung kopieren und weitergeben.
    exit /b 1
)

set "DLL=%~dp0build\Release\AIMP_AutoRate.dll"
if not exist "%DLL%" (
    echo [FEHLER] DLL nicht gefunden: %DLL%
    exit /b 1
)
echo.
echo Fertig: %DLL%

if /i "%~2"=="install" goto :install
echo Zum Installieren die DLL nach AIMP\Plugins\AutoRate\ kopieren oder "build.bat SDK-Pfad install" nutzen.
exit /b 0

:install
set "TARGET=%ProgramFiles%\AIMP\Plugins\AutoRate"
mkdir "%TARGET%" >nul 2>&1
copy /y "%DLL%" "%TARGET%\" >nul
if errorlevel 1 goto :copyfail
echo Installiert nach %TARGET%
exit /b 0

:copyfail
echo [FEHLER] Kopieren nach %TARGET% fehlgeschlagen. AIMP beenden und als Administrator starten.
exit /b 1

:nosdk
echo [FEHLER] Kein SDK-Pfad angegeben.
echo Aufruf: build.bat "C:\Pfad\zum\aimp_sdk\Sources\Cpp"
exit /b 1

:badsdk
echo [FEHLER] apiPlugin.h nicht gefunden in: %SDK%
echo Der Pfad muss auf den Ordner "Sources\Cpp" im entpackten AIMP-SDK zeigen.
exit /b 1
