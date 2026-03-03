@echo off
setlocal

set "ROOT=%~1"
if "%ROOT%"=="" set "ROOT=%~dp0"

set "ARCH=%~2"
if "%ARCH%"=="" set "ARCH=x64"

set "CHROMIUM_ROOT=%~3"
if "%CHROMIUM_ROOT%"=="" set "CHROMIUM_ROOT=%ROOT%\..\chromium"
if exist "%ROOT%\chromium\src" set "CHROMIUM_ROOT=%ROOT%\chromium"

set "CONFIG=%~4"
if "%CONFIG%"=="" set "CONFIG=Release"

if not exist "%CHROMIUM_ROOT%\src" goto :missing_chromium_root

cd /d "%ROOT%"
if errorlevel 1 exit /b 1

echo Using Atlas root: %ROOT%
echo Using Chromium root: %CHROMIUM_ROOT%
echo Config: %CONFIG%

call "scripts\chromium\run_in_parallels_owl.bat" "%ARCH%" "%CHROMIUM_ROOT%" "%CONFIG%"
goto :done

:missing_chromium_root
echo Chromium root not found: %CHROMIUM_ROOT%
echo Expected Chromium checkout layout with %CHROMIUM_ROOT%\src
echo Try one of:
echo   1^) Run setup script:
echo      .\scripts\chromium\setup_owl_workspace.ps1 -WorkspaceRoot "%ROOT%\.." -ChromiumDir chromium
echo   2^) Or pass the explicit Chromium root that has a src directory:
echo      .\run_in_parallels.bat "%ROOT%" x64 "%CHROMIUM_ROOT%" Release
exit /b 1

:done
endlocal
