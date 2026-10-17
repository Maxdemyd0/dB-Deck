@echo off
rem Run INSIDE THE TEST VM ONLY, as Administrator.
rem Lets Windows load test-signed drivers. Requires Secure Boot to be OFF in the VM firmware.
net session >nul 2>&1 || (echo Run this as Administrator. & exit /b 1)
bcdedit /set testsigning on || (echo bcdedit failed. Is Secure Boot turned off in the VM settings? & exit /b 1)
echo.
echo Test signing enabled. Restart the VM, then run install-driver.cmd as Administrator.
