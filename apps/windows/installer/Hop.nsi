Unicode true
RequestExecutionLevel user
SetCompressor /SOLID lzma
CRCCheck force
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "WinVer.nsh"
!include "x64.nsh"

!ifndef HOP_EXECUTABLE
  !error "Set HOP_EXECUTABLE to the absolute path of the validated TLS-enabled tray executable"
!endif
!ifndef HOP_INSTALLER
  !error "Set HOP_INSTALLER to the desired output file"
!endif
!ifndef HOP_LICENSE
  !error "Set HOP_LICENSE to the repository LICENSE file"
!endif
!ifndef HOP_TLS_LICENSE
  !error "Set HOP_TLS_LICENSE to the LICENSE from the mbedTLS source used by the build"
!endif
!ifndef HOP_NOTICES
  !error "Set HOP_NOTICES to the repository THIRD_PARTY_NOTICES.md"
!endif
!ifndef HOP_DISPLAY_VERSION
  !define HOP_DISPLAY_VERSION "2.0.0-rc.1"
!endif
!ifndef HOP_FILE_VERSION
  !define HOP_FILE_VERSION "2.0.0.1"
!endif

!define HOP_INSTALL_PATH "$LOCALAPPDATA\Programs\Hop"
!define HOP_SHORTCUT_DIR "$SMPROGRAMS\Hop"
!define HOP_UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Hop"
; Keep this name in sync with lifecycle_mutex in src/tray_app.cpp.
!define HOP_LIFECYCLE_MUTEX "Local\HopNativeLifecycle-{6D941BF5-C595-430E-8242-A63E8429D60A}"

Name "Hop"
OutFile "${HOP_INSTALLER}"
InstallDir "${HOP_INSTALL_PATH}"
ShowInstDetails show
ShowUninstDetails show
VIProductVersion "${HOP_FILE_VERSION}"
VIAddVersionKey /LANG=1033 "ProductName" "Hop"
VIAddVersionKey /LANG=1033 "ProductVersion" "${HOP_DISPLAY_VERSION}"
VIAddVersionKey /LANG=1033 "FileDescription" "Hop x64 Setup"
VIAddVersionKey /LANG=1033 "FileVersion" "${HOP_FILE_VERSION}"
VIAddVersionKey /LANG=1033 "CompanyName" "Hop"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright (c) 2026 Hop contributors"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "SimpChinese"

LangString HopBusy ${LANG_ENGLISH} "Quit Hop or the other Hop setup window, then try again."
LangString HopBusy ${LANG_SIMPCHINESE} "请退出 Hop 或关闭另一个 Hop 安装窗口，然后重试。"
LangString HopPlatform ${LANG_ENGLISH} "This installer requires an x64 version of Windows 10 (build 15063 or newer) or Windows 11."
LangString HopPlatform ${LANG_SIMPCHINESE} "此安装包要求 x64 版 Windows 10（内部版本 15063 或更高）或 Windows 11。"
LangString HopInstallFailed ${LANG_ENGLISH} "Setup could not finish. Check that the installation folder is writable, then run setup again."
LangString HopInstallFailed ${LANG_SIMPCHINESE} "未能完成安装。请确认安装文件夹可写，然后重试。"
LangString HopUninstallFailed ${LANG_ENGLISH} "Hop could not finish uninstalling. Quit Hop and check the installation folder, then try again."
LangString HopUninstallFailed ${LANG_SIMPCHINESE} "未能完成卸载。请退出 Hop 并检查安装文件夹，然后重试。"

Var LifecycleMutex

!macro ReserveHopLifecycle
  System::Call 'kernel32::CreateMutexW(p 0, i 0, w "${HOP_LIFECYCLE_MUTEX}") p.r0 ?e'
  Pop $1
  StrCpy $LifecycleMutex $0
  ${If} $LifecycleMutex == 0
  ${OrIf} $1 == 183 ; ERROR_ALREADY_EXISTS
    MessageBox MB_OK|MB_ICONEXCLAMATION "$(HopBusy)" /SD IDOK
    SetErrorLevel 10
    Abort
  ${EndIf}
  ; The process holds the handle until exit, including during rollback.
!macroend

Function .onInit
  ${IfNot} ${IsNativeAMD64}
  ${OrIfNot} ${AtLeastWin10}
  ${OrIfNot} ${AtLeastBuild} 15063
    MessageBox MB_OK|MB_ICONSTOP "$(HopPlatform)" /SD IDOK
    SetErrorLevel 11
    Quit
  ${EndIf}
  ; Use one fixed per-user path for install, upgrades and uninstall.
  StrCpy $INSTDIR "${HOP_INSTALL_PATH}"
FunctionEnd

Section "Hop"
  SetShellVarContext current
  !insertmacro ReserveHopLifecycle

  ClearErrors
  SetOutPath "$INSTDIR"
  IfErrors install_failed
  File /oname=Hop.new.exe "${HOP_EXECUTABLE}"
  IfErrors install_failed
  WriteUninstaller "$INSTDIR\Uninstall.new.exe"
  IfErrors install_failed

  ; Publish staged executables without truncating the prior copies. Publish
  ; the compatible uninstaller first so a replaced app is always removable.
  System::Call 'kernel32::MoveFileExW(w "$INSTDIR\Uninstall.new.exe", w "$INSTDIR\Uninstall.exe", i 9) i.r0'
  StrCmp $0 0 install_failed
  System::Call 'kernel32::MoveFileExW(w "$INSTDIR\Hop.new.exe", w "$INSTDIR\Hop.exe", i 9) i.r0'
  StrCmp $0 0 install_failed

  SetOutPath "$INSTDIR\licenses"
  IfErrors install_failed
  File /oname=Hop.txt "${HOP_LICENSE}"
  IfErrors install_failed
  File /oname=mbedTLS.txt "${HOP_TLS_LICENSE}"
  IfErrors install_failed
  File /oname=THIRD_PARTY_NOTICES.md "${HOP_NOTICES}"
  IfErrors install_failed
  CreateDirectory "${HOP_SHORTCUT_DIR}"
  SetOutPath "$INSTDIR"
  CreateShortcut "${HOP_SHORTCUT_DIR}\Hop.lnk" "$INSTDIR\Hop.exe"
  WriteRegStr HKCU "${HOP_UNINSTALL_KEY}" "DisplayName" "Hop"
  WriteRegStr HKCU "${HOP_UNINSTALL_KEY}" "DisplayVersion" "${HOP_DISPLAY_VERSION}"
  WriteRegStr HKCU "${HOP_UNINSTALL_KEY}" "Publisher" "Hop"
  WriteRegStr HKCU "${HOP_UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${HOP_UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\Hop.exe"
  WriteRegStr HKCU "${HOP_UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${HOP_UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${HOP_UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${HOP_UNINSTALL_KEY}" "NoRepair" 1
  IfErrors install_failed
  Goto install_done

  install_failed:
    Delete "$INSTDIR\Hop.new.exe"
    Delete "$INSTDIR\Uninstall.new.exe"
    MessageBox MB_OK|MB_ICONSTOP "$(HopInstallFailed)" /SD IDOK
    SetErrorLevel 12
    Abort
  install_done:
SectionEnd

Section "Uninstall"
  SetShellVarContext current
  ; A copy of the uninstaller outside the registered directory cannot remove
  ; its neighbouring files or the actual application's registration.
  StrCmp "$INSTDIR" "${HOP_INSTALL_PATH}" 0 uninstall_failed
  !insertmacro ReserveHopLifecycle
  ClearErrors
  Delete "$INSTDIR\Hop.exe"
  IfErrors uninstall_failed
  Delete "$INSTDIR\Hop.new.exe"
  Delete "$INSTDIR\Uninstall.new.exe"
  Delete "${HOP_SHORTCUT_DIR}\Hop.lnk"
  Delete "${HOP_SHORTCUT_DIR}\Uninstall Hop.lnk"
  Delete "$INSTDIR\licenses\Hop.txt"
  Delete "$INSTDIR\licenses\mbedTLS.txt"
  Delete "$INSTDIR\licenses\THIRD_PARTY_NOTICES.md"
  DeleteRegKey HKCU "${HOP_UNINSTALL_KEY}"
  IfErrors uninstall_failed
  Delete "$INSTDIR\Uninstall.exe"
  IfErrors uninstall_failed
  RMDir "${HOP_SHORTCUT_DIR}"
  RMDir "$INSTDIR\licenses"
  RMDir "$INSTDIR"
  ; Keep the user's trusted peers and settings.
  Goto uninstall_done

  uninstall_failed:
    MessageBox MB_OK|MB_ICONSTOP "$(HopUninstallFailed)" /SD IDOK
    SetErrorLevel 13
    Abort
  uninstall_done:
SectionEnd
