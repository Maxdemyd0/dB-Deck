; dB-Deck NSIS installer hooks (Tauri bundler).
; The engine runs in the background after the window closes, so it must be
; stopped before files are replaced or removed.

!macro NSIS_HOOK_PREINSTALL
  nsExec::Exec 'taskkill /IM dbdeck-engine.exe /F'
!macroend

!macro NSIS_HOOK_PREUNINSTALL
  nsExec::Exec 'taskkill /IM dB-Deck.exe /F'
  nsExec::Exec 'taskkill /IM dbdeck-engine.exe /F'
  ; Remove the Start-with-Windows entry created by the app (if enabled).
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "dB-Deck"
!macroend

!macro NSIS_HOOK_POSTUNINSTALL
  ; User settings live in %APPDATA%\dB-Deck and are kept unless the user
  ; chose to delete application data in the uninstaller.
!macroend
