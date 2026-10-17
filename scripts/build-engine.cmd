@echo off
rem Builds the dB-Deck native engine and tests with MSVC + Ninja.
rem Usage: scripts\build-engine.cmd [Debug|Release]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSPATH=%%i
if "%VSPATH%"=="" (
  echo Visual Studio with C++ tools not found.
  exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set ROOT=%~dp0..
cmake -S "%ROOT%\engine" -B "%ROOT%\build\engine-%CONFIG%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
cmake --build "%ROOT%\build\engine-%CONFIG%" || exit /b 1
endlocal
