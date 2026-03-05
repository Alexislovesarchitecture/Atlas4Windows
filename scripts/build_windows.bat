@echo off
setlocal
set "ROOT=%~dp0\.."
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Release"
if not exist "%ROOT%\build" mkdir "%ROOT%\build"

where cmake >nul 2>nul
if errorlevel 1 goto missing_cmake

where cl >nul 2>nul
if errorlevel 1 goto missing_cl

where msbuild >nul 2>nul
if errorlevel 1 goto missing_msbuild

:have_tools
cmake -S "%ROOT%" -B "%ROOT%\build"
if errorlevel 1 exit /b 1
cmake --build "%ROOT%\build" --config "%CONFIG%"
if errorlevel 1 exit /b 1
exit /b 0

:missing_cmake
echo Missing required Windows build tool: cmake
echo   Install CMake from https://cmake.org/download and ensure it is in PATH.
goto missing_tools

:missing_cl
echo Missing required Windows build tool: cl
echo   Install Visual Studio C++ Build Tools with the C++ workload (VC++ compilers).
goto missing_tools

:missing_msbuild
echo Missing required Windows build tool: msbuild
echo   Install Visual Studio or Visual Studio Build Tools with MSBuild components.
goto missing_tools

:missing_tools
echo.
echo Tip: run from "Developer Command Prompt for VS 2022" or an initialized VS shell.
exit /b 1

endlocal
