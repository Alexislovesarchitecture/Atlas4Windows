@echo off
setlocal

set "ROOT=%~1"
if "%ROOT%"=="" set "ROOT=Y:\Desktop\CodexWorkspace\Atlas4Windows"
set "CONFIG=%~2"
if "%CONFIG%"=="" set "CONFIG=Debug"

if not exist "%ROOT%\scripts\build_windows.bat" (
  echo Could not find Atlas4Windows at:
  echo   %ROOT%
  echo Pass path as first arg, for example:
  echo   scripts\run_in_parallels.bat "Y:\Desktop\CodexWorkspace\Atlas4Windows" Release
  exit /b 1
)

cd /d "%ROOT%"
if errorlevel 1 exit /b 1

call scripts\build_windows.bat "%CONFIG%"
if errorlevel 1 exit /b 1

call scripts\run_windows.bat "%CONFIG%"
endlocal
