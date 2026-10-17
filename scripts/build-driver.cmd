@echo off
rem Builds the dB-Deck virtual cable driver package (requires the WDK).
rem Output: driver\windows\dbdeck-vad\x64\Release\package\{dbdeckvad.sys,.inf,.cat}
rem The package is TEST-SIGNED only. Building does not install anything.
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -find MSBuild\**\Bin\amd64\MSBuild.exe`) do set MSBUILD=%%i
if "%MSBUILD%"=="" (
  echo MSBuild not found.
  exit /b 1
)
set ROOT=%~dp0..
"%MSBUILD%" "%ROOT%\driver\windows\dbdeck-vad\SimpleAudioSample.sln" /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo || exit /b 1
for /f "usebackq tokens=*" %%i in (`dir /b /s "%ProgramFiles(x86)%\Windows Kits\10\Tools\*infverif.exe" ^| findstr x64`) do set INFVERIF=%%i
if not "%INFVERIF%"=="" "%INFVERIF%" /w /v "%ROOT%\driver\windows\dbdeck-vad\x64\Release\package\dbdeckvad.inf" || exit /b 1
endlocal
