@echo off
setlocal

set "ROOT=%~dp0\.."
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"

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

start "" "%ROOT%\build\%CONFIG%\atlas-client.exe"
endlocal
