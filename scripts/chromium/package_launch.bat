@echo off
setlocal

set "ROOT=%~dp0\..\.."
set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x64"
set "CHROMIUM_ROOT=%~2"
if "%CHROMIUM_ROOT%"=="" set "CHROMIUM_ROOT=%ROOT%\..\chromium"
set "PKG=%ROOT%\out\atlas-owl-launch-%ARCH%"
set "OUT=%CHROMIUM_ROOT%\out\owl_%ARCH%"

if /I "%ARCH%" neq "x64" if /I "%ARCH%" neq "arm64" (
  echo Unsupported architecture: %ARCH%
  echo Supported: x64 arm64
  exit /b 1
)

if not exist "%OUT%\owl_client.exe" (
  echo Missing %OUT%\owl_client.exe
  echo Build first using: scripts\chromium\build_owl.ps1
  exit /b 1
)
if not exist "%OUT%\owl_host.exe" (
  echo Missing %OUT%\owl_host.exe
  echo Build first using: scripts\chromium\build_owl.ps1
  exit /b 1
)

if exist "%PKG%" rmdir /s /q "%PKG%"
mkdir "%PKG%\bin"

copy /Y "%OUT%\owl_client.exe" "%PKG%\bin\owl_client.exe" >nul
copy /Y "%OUT%\owl_host.exe" "%PKG%\bin\owl_host.exe" >nul
copy /Y "%ROOT%\scripts\chromium\run_owl.ps1" "%PKG%\run_owl.ps1" >nul
copy /Y "%ROOT%\README.md" "%PKG%\README.md" >nul

if exist "%PKG%\atlas-owl-launch.zip" del "%PKG%\atlas-owl-launch.zip"
powershell -NoProfile -Command "Compress-Archive -Path '%PKG%\\*' -DestinationPath '%PKG%\\atlas-owl-launch.zip' -Force"

echo Packaged launch bundle in: %PKG%\atlas-owl-launch.zip
endlocal
