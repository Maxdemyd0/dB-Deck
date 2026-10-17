@echo off
rem Run inside the test VM (no admin needed) after install-driver.cmd.
rem Plays a quiet 1 kHz tone into "dB-Deck Virtual Input" and checks it arrives on
rem "dB-Deck Virtual Output", then runs the rest of the Windows integration tests.
rem Master-volume tests are skipped so the VM's volume is left alone.
setlocal
cd /d "%~dp0"
set DBDECK_TEST_SKIP_MASTER=1
set DBDECK_TEST_RENDER_DEVICE=dB-Deck Virtual Cable
dbdeck-wintests.exe > test-results.txt 2>&1
type test-results.txt
echo.
echo Results saved to %~dp0test-results.txt - send that file back.
endlocal
