@echo off
rem Run inside the test VM as Administrator. Removes the dB-Deck virtual cable
rem device and its driver package from the driver store.
setlocal
cd /d "%~dp0"
net session >nul 2>&1 || (echo Run this as Administrator. & exit /b 1)
devcon.exe remove ROOT\dBDeckVirtualAudio
powershell -NoProfile -Command ^
  "$blocks = (pnputil /enum-drivers | Out-String) -split '(?=Published Name)';" ^
  "foreach ($b in $blocks) { if ($b -match 'dbdeckvad\.inf' -and $b -match 'Published Name:\s*(oem\d+\.inf)') { pnputil /delete-driver $matches[1] /uninstall /force } }"
echo.
echo Removed. To restore normal boot security: bcdedit /set testsigning off  (then restart)
endlocal
