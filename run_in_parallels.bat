@echo off
setlocal

set "ROOT=%~1"
if "%ROOT%"=="" set "ROOT=Y:\Desktop\CodexWorkspace\Atlas4Windows"

set "ARCH=%~2"
if "%ARCH%"=="" set "ARCH=x64"

set "CHROMIUM_ROOT=%~3"
if "%CHROMIUM_ROOT%"=="" set "CHROMIUM_ROOT=%ROOT%\..\chromium"
if not exist "%CHROMIUM_ROOT%\src" (
  if exist "%ROOT%\chromium\src" set "CHROMIUM_ROOT=%ROOT%\chromium"
)

set "CONFIG=%~4"
if "%CONFIG%"=="" set "CONFIG=Release"

if not exist "%CHROMIUM_ROOT%\src" (
  echo Chromium root not found: %CHROMIUM_ROOT%
  echo Expected Chromium checkout layout with %CHROMIUM_ROOT%\src
  echo Try one of:
  echo   1) Run setup script:
  echo      .\scripts\chromium\setup_owl_workspace.ps1 -WorkspaceRoot Y:\Desktop\CodexWorkspace -ChromiumDir chromium
  echo   2) Or pass the explicit Chromium root that has a src directory:
  echo      .\run_in_parallels.bat "%ROOT%" x64 "Y:\path\to\chromium" Release
  exit /b 1
)

cd /d "%ROOT%"
if errorlevel 1 exit /b 1

echo Using Atlas root: %ROOT%
echo Using Chromium root: %CHROMIUM_ROOT%
echo Config: %CONFIG%

call "scripts\chromium\run_in_parallels_owl.bat" "%ARCH%" "%CHROMIUM_ROOT%" "%CONFIG%"
endlocal
