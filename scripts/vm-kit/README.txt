dB-Deck virtual cable - VM test kit
===================================

Use ONLY inside a disposable test VM. These steps turn on Windows test signing.

1. Copy this whole folder into the VM (e.g. C:\vm-kit).
2. Right-click enable-testsigning.cmd > Run as administrator. Restart the VM.
3. Right-click install-driver.cmd > Run as administrator.
   Windows may ask whether to install software from "dB-Deck": choose Install.
4. Double-click run-tests.cmd. It writes test-results.txt.
5. Optional: run dB-Deck_1.0.0_x64-setup.exe and try the app (Virtual Devices page).
6. Send back test-results.txt.

Remove: uninstall-driver.cmd (as administrator), then
        bcdedit /set testsigning off  and restart.

Contents
  driver\dbdeckvad.sys/.inf/.cat  the driver package (test-signed)
  package.cer                     WDK test certificate that signed it
  devcon.exe                      WDK device installer tool
  dbdeck-wintests.exe             dB-Deck integration tests
  dB-Deck_1.0.0_x64-setup.exe     the dB-Deck app
