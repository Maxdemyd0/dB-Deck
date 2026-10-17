@echo off
rem Assembles build\vm-kit: everything needed to install and test the dB-Deck
rem virtual cable driver inside a disposable test VM. Builds nothing new;
rem run build-engine.cmd, build-driver.cmd and build-release.cmd first.
setlocal
set ROOT=%~dp0..
set KIT=%ROOT%\build\vm-kit
set PKG=%ROOT%\driver\windows\dbdeck-vad\x64\Release
if exist "%KIT%" rmdir /s /q "%KIT%"
mkdir "%KIT%\driver" || exit /b 1
copy /y "%PKG%\package\dbdeckvad.sys" "%KIT%\driver\" >nul || exit /b 1
copy /y "%PKG%\package\dbdeckvad.inf" "%KIT%\driver\" >nul || exit /b 1
copy /y "%PKG%\package\dbdeckvad.cat" "%KIT%\driver\" >nul || exit /b 1
copy /y "%PKG%\package.cer" "%KIT%\" >nul || exit /b 1
for /f "usebackq tokens=*" %%i in (`dir /b /s "%ProgramFiles(x86)%\Windows Kits\10\Tools\*devcon.exe" ^| findstr \\x64\\`) do set DEVCON=%%i
copy /y "%DEVCON%" "%KIT%\" >nul || exit /b 1
copy /y "%ROOT%\build\engine-Release\dbdeck-wintests.exe" "%KIT%\" >nul || exit /b 1
for %%f in ("%ROOT%\apps\desktop\src-tauri\target\release\bundle\nsis\*-setup.exe") do copy /y "%%f" "%KIT%\" >nul
copy /y "%~dp0vm-kit\*.cmd" "%KIT%\" >nul || exit /b 1
copy /y "%~dp0vm-kit\README.txt" "%KIT%\" >nul || exit /b 1
echo VM kit ready: %KIT%
endlocal
