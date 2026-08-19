; installer.nsi — build with: makensis installer.nsi
!include "MUI2.nsh"

!define APPNAME   "SerialPortTerminal"
!define COMPANY   "Example"
!define VERSION   "1.0.0"

Name "${APPNAME}"
OutFile "SerialTerm-${VERSION}-Setup.exe"
InstallDir "$PROGRAMFILES\${COMPANY}\${APPNAME}"
InstallDirRegKey HKCU "Software\${COMPANY}\${APPNAME}" ""
RequestExecutionLevel admin

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_FINISHPAGE_RUN "$INSTDIR\serialterm.exe"
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "$INSTDIR"
  File "serialterm.exe"
  File "window1.glade"
  File "config.cfg"
  File /r "win-runtime\*.*"          ; GTK3 + libconfig DLLs and data

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\${COMPANY}\${APPNAME}" "" "$INSTDIR"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" \
               "DisplayName" "${APPNAME}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}" \
               "UninstallString" '"$INSTDIR\Uninstall.exe"'

  CreateDirectory "$SMPROGRAMS\${COMPANY}"
  CreateShortCut "$SMPROGRAMS\${COMPANY}\${APPNAME}.lnk" "$INSTDIR\serialterm.exe"
  CreateShortCut "$DESKTOP\${APPNAME}.lnk"               "$INSTDIR\serialterm.exe"
SectionEnd

Section "Uninstall"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir /r "$INSTDIR"
  Delete "$SMPROGRAMS\${COMPANY}\${APPNAME}.lnk"
  Delete "$DESKTOP\${APPNAME}.lnk"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"
  DeleteRegKey HKCU "Software\${COMPANY}\${APPNAME}"
  RMDir "$SMPROGRAMS\${COMPANY}"
SectionEnd
