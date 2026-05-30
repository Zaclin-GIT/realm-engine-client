@echo off
REM ============================================================================
REM  One-time setup: mirror the repo from the WSL filesystem to a NATIVE
REM  Windows drive and install Windows npm deps, so the installer can be built.
REM
REM  Why: the \\wsl.localhost copy has Linux node_modules (Linux esbuild /
REM  electron, no .cmd shims) and is slow over the 9p share. The Windows build
REM  needs its own checkout + its own `npm install`.
REM
REM  Run this once (re-run anytime to re-sync source). Then build with
REM  build-installer.bat from the destination folder.
REM
REM  Usage:
REM    setup-windows-build.bat                 -> dest C:\realm-engine
REM    setup-windows-build.bat D:\path\here    -> custom dest
REM ============================================================================
setlocal

REM client dir = tools/../; repo root = tools/../../ (has client + internal)
for %%I in ("%~dp0..\..") do set "SRCROOT=%%~fI"
set "SRCCLIENT=%SRCROOT%\client"
set "SRCINTERNAL=%SRCROOT%\internal"

set "DEST=%~1"
if "%DEST%"=="" set "DEST=C:\realm-engine"
set "DESTCLIENT=%DEST%\client"
set "DESTINTERNAL=%DEST%\internal"

echo Source: "%SRCROOT%"
echo Dest:   "%DEST%"
echo.

if not exist "%SRCCLIENT%\package.json" (
  echo [ERROR] "%SRCCLIENT%\package.json" not found. Run this from the WSL client folder.
  goto :fail
)
if not exist "%SRCINTERNAL%\" (
  echo [ERROR] "%SRCINTERNAL%" not found. The internal DLL repo must sit next to client.
  goto :fail
)

REM Mirror client (skip platform/build/vcs dirs -- those are rebuilt on Windows).
echo === Copying client ===
robocopy "%SRCCLIENT%" "%DESTCLIENT%" /MIR /R:1 /W:1 /NFL /NDL /NP /NJH /NJS ^
  /XD node_modules dist release .git "electron\native\build"
if errorlevel 8 ( echo [ERROR] robocopy client failed. & goto :fail )

echo === Copying internal ===
robocopy "%SRCINTERNAL%" "%DESTINTERNAL%" /MIR /R:1 /W:1 /NFL /NDL /NP /NJH /NJS ^
  /XD .git x64 .vs Debug Release ipch
if errorlevel 8 ( echo [ERROR] robocopy internal failed. & goto :fail )

echo.
echo === Installing Windows npm deps in "%DESTCLIENT%" ===
cd /d "%DESTCLIENT%" || ( echo [ERROR] cannot cd to "%DESTCLIENT%". & goto :fail )

where npm >nul 2>nul
if errorlevel 1 ( echo [ERROR] npm not on PATH. Install Node.js. & goto :fail )

call npm install
if errorlevel 1 ( echo [ERROR] npm install failed. & goto :fail )

echo.
echo === SETUP OK ===
echo Native Windows checkout ready at: "%DEST%"
echo.
echo Next: build the installer from there:
echo     cd /d "%DESTCLIENT%"
echo     tools\build-installer.bat
echo.
pause
exit /b 0

:fail
echo.
pause
exit /b 1
