@echo off
setlocal

set "ROOT=%~dp0\.."
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
set "OUT=%ROOT%\out"
set "PKG=%OUT%\atlas-windows-v0"

if not exist "%ROOT%\build\%CONFIG%\atlas-client.exe" (
  echo Missing atlas-client.exe at:
  echo   %ROOT%\build\%CONFIG%\atlas-client.exe
  echo Build first with scripts\build_windows.bat.
  exit /b 1
)

if not exist "%ROOT%\build\%CONFIG%\atlas-host.exe" (
  echo Missing atlas-host.exe at:
  echo   %ROOT%\build\%CONFIG%\atlas-host.exe
  echo Build first with scripts\build_windows.bat.
  exit /b 1
)

if exist "%PKG%" rmdir /s /q "%PKG%"
mkdir "%OUT%"
mkdir "%PKG%"
mkdir "%PKG%\bin"

copy /Y "%ROOT%\build\%CONFIG%\atlas-client.exe" "%PKG%\bin\atlas-client.exe" >nul
copy /Y "%ROOT%\build\%CONFIG%\atlas-host.exe" "%PKG%\bin\atlas-host.exe" >nul
copy /Y "%ROOT%\README.md" "%PKG%\README.md" >nul
copy /Y "%ROOT%\scripts\run_windows.bat" "%PKG%\run_windows.bat" >nul

if exist "%PKG%\AtlasWindowsV0.zip" del "%PKG%\AtlasWindowsV0.zip"
powershell -NoProfile -Command "Compress-Archive -Path '%PKG%\*' -DestinationPath '%PKG%\\AtlasWindowsV0.zip' -Force"

echo Packaged Windows bundle to %PKG%\AtlasWindowsV0.zip
endlocal
