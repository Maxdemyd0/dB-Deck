@echo off
rem Run INSIDE THE TEST VM ONLY, as Administrator, after enable-testsigning.cmd + restart.
rem Trusts the WDK test certificate and installs the dB-Deck virtual cable.
setlocal
cd /d "%~dp0"
net session >nul 2>&1 || (echo Run this as Administrator. & exit /b 1)
bcdedit /enum {current} | findstr /i "testsigning" | findstr /i "Yes" >nul || (
  echo Test signing is not enabled. Run enable-testsigning.cmd and restart first.
  exit /b 1
)
echo Trusting the dB-Deck test certificate (this VM only)...
certutil -f -addstore Root package.cer >nul || exit /b 1
certutil -f -addstore TrustedPublisher package.cer >nul || exit /b 1
echo Installing the driver...
devcon.exe install driver\dbdeckvad.inf ROOT\dBDeckVirtualAudio || exit /b 1
devcon.exe status ROOT\dBDeckVirtualAudio
echo.
echo Done. Windows should now list "Speakers (dB-Deck Virtual Cable 1)" and
echo "Microphone Array (dB-Deck Virtual Cable 1)". Next: run run-tests.cmd.
endlocal
