@echo off
setlocal
set "ROOT=%~dp0\.."
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
if not exist "%ROOT%\build" mkdir "%ROOT%\build"
where cmake >nul 2>nul
if errorlevel 1 (
  echo cmake not found in PATH. Run this script from a Visual Studio Developer Command Prompt.
  exit /b 1
)
cmake -S "%ROOT%" -B "%ROOT%\build"
cmake --build "%ROOT%\build" --config "%CONFIG%"
endlocal
