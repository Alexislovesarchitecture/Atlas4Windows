@echo off
setlocal

set "ROOT=%~dp0..\.."
set "ARCH=x64"
set "CHROMIUM_ROOT=%ROOT%\..\chromium"
set "CONFIG=Release"

if not "%~1"=="" set "ARCH=%~1"
if not "%~2"=="" set "CHROMIUM_ROOT=%~2"
if not "%~3"=="" set "CONFIG=%~3"

if not exist "%CHROMIUM_ROOT%\src" (
  echo Chromium root not found: %CHROMIUM_ROOT%
  echo Provide: run_in_parallels_owl.bat ^<arch^> ^<chromium_root^> ^<config^>
  exit /b 1
)

if /I "%ARCH%"=="x64" goto :okarch
if /I "%ARCH%"=="arm64" goto :okarch
echo Unsupported architecture: %ARCH%
echo Supported: x64 arm64
exit /b 1
:okarch

cd /d "%ROOT%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\scripts\chromium\build_owl.ps1" -ChromiumRoot "%CHROMIUM_ROOT%" -Archs "%ARCH%" -Config %CONFIG%
if errorlevel 1 exit /b 1

powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\scripts\chromium\run_owl.ps1" -ChromiumRoot "%CHROMIUM_ROOT%" -Arch "%ARCH%" -ProfileRoot "%ROOT%\build\profile"
endlocal
