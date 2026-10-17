@echo off
rem Full release build: engine (Release) -> sidecar -> Tauri NSIS installer.
rem Output: apps\desktop\src-tauri\target\release\bundle\nsis\dB-Deck_*_x64-setup.exe
setlocal
set ROOT=%~dp0..
call "%~dp0build-engine.cmd" Release || exit /b 1
"%ROOT%\build\engine-Release\dbdeck-tests.exe" || exit /b 1
if not exist "%ROOT%\apps\desktop\src-tauri\binaries" mkdir "%ROOT%\apps\desktop\src-tauri\binaries"
copy /y "%ROOT%\build\engine-Release\dbdeck-engine.exe" "%ROOT%\apps\desktop\src-tauri\binaries\dbdeck-engine-x86_64-pc-windows-msvc.exe" >nul || exit /b 1
pushd "%ROOT%\apps\desktop"
call npm install --no-fund --no-audit || (popd & exit /b 1)
call npx tauri build || (popd & exit /b 1)
popd
endlocal
