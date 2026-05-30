@echo off
setlocal

cd /d "%~dp0.."

if not exist "node_modules" (
  echo node_modules not found — running npm install...
  call npm install
  if errorlevel 1 (
    echo.
    echo ERROR: npm install failed. Make sure Node.js is installed.
    pause
    exit /b 1
  )
)

echo Preparing SDK and native dev stubs...
call npm run build:sdk
if errorlevel 1 (
  echo.
  echo ERROR: SDK build failed.
  pause
  exit /b 1
)

call npm run build:native
if errorlevel 1 (
  echo.
  echo ERROR: native dev preparation failed.
  pause
  exit /b 1
)

echo Starting Realm Engine in Electron dev mode...
call npm run electron
if errorlevel 1 (
  echo.
  echo ERROR: electron failed to start.
  pause
  exit /b 1
)

endlocal
