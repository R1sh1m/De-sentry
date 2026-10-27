; De-Sentry NSIS Installer Hooks
; Ensures running sidecars (desentryd.exe) and desktop app instances are terminated
; before copying or removing files, avoiding Windows file-in-use write errors.

!macro NSIS_HOOK_PREINSTALL
  DetailPrint "Stopping existing De-Sentry processes..."
  nsExec::Exec 'taskkill /F /IM desentryd.exe /T'
  nsExec::Exec 'taskkill /F /IM De-Sentry.exe /T'
  nsExec::Exec 'taskkill /F /IM de-sentry-app.exe /T'
  Sleep 1000
!macroend

!macro NSIS_HOOK_PREUNINSTALL
  DetailPrint "Stopping existing De-Sentry processes..."
  nsExec::Exec 'taskkill /F /IM desentryd.exe /T'
  nsExec::Exec 'taskkill /F /IM De-Sentry.exe /T'
  nsExec::Exec 'taskkill /F /IM de-sentry-app.exe /T'
  Sleep 1000
!macroend
