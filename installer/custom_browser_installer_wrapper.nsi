; Minimal NSIS wrapper for mini_installer.exe
; Usage:
;   makensis.exe /DPRODUCT_NAME="Custom Browser" /DMINI_INSTALLER_SOURCE="path\\to\\mini_installer.exe" custom_browser_installer_wrapper.nsi
;   CustomBrowserSetup.exe /system   ; for system-level (requires admin)

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "WordFunc.nsh"

!ifndef PRODUCT_NAME
  !define PRODUCT_NAME "Velloc Agent"
!endif
!ifndef PRODUCT_PUBLISHER
  !define PRODUCT_PUBLISHER "Velloc Agent"
!endif
!define MINI_INSTALLER_NAME "mini_installer.exe"
!ifndef MINI_INSTALLER_SOURCE
  !define MINI_INSTALLER_SOURCE "..\\..\\out\\Velloc\\mini_installer.exe"
!endif
!ifndef PRODUCT_VERSION
  ; Read version from the mini_installer.exe file version resource.
  !getdllversion /NoErrors /Packed "${MINI_INSTALLER_SOURCE}" MINI_VER_
  !ifndef MINI_VER_HIGH
    !error "Missing or unreadable mini_installer.exe at ${MINI_INSTALLER_SOURCE}"
  !endif
  !define /math MINI_VER_MAJOR ${MINI_VER_HIGH} >> 16
  !define /math MINI_VER_MINOR ${MINI_VER_HIGH} & 0xFFFF
  !define /math MINI_VER_BUILD ${MINI_VER_LOW} >> 16
  !define /math MINI_VER_PATCH ${MINI_VER_LOW} & 0xFFFF
  !define PRODUCT_VERSION "${MINI_VER_MAJOR}.${MINI_VER_MINOR}.${MINI_VER_BUILD}.${MINI_VER_PATCH}"
  ; Packaging guard: the wrapper EMBEDS mini_installer.exe at compile time,
  ; so a stale artifact ships silently. A first field >= 100 is the legacy
  ; {chromium major}-first scheme — this wrapper would then uninstall the
  ; user's legacy install only to reinstall another legacy build (looks
  ; exactly like "the installer did not overwrite"). Rebuild it first:
  ;   autoninja -C out/Velloc mini_installer
  !if ${MINI_VER_MAJOR} >= 100
    !error "mini_installer.exe at ${MINI_INSTALLER_SOURCE} is version ${MINI_VER_MAJOR}.${MINI_VER_MINOR}.${MINI_VER_BUILD}.${MINI_VER_PATCH} — a legacy-scheme build. Rebuild mini_installer before packaging the wrapper."
  !endif
  !undef MINI_VER_HIGH
  !undef MINI_VER_LOW
  !undef MINI_VER_MAJOR
  !undef MINI_VER_MINOR
  !undef MINI_VER_BUILD
  !undef MINI_VER_PATCH
!endif

!define MASTER_PREFERENCES_NAME "master_preferences"
!ifndef MASTER_PREFERENCES_SOURCE
  !define MASTER_PREFERENCES_SOURCE ".\\master_preferences"
!endif
!define TARGET_APP_EXE "C:\\Users\\myyyb\\AppData\\Local\\Velloc\\Application\\chrome.exe"

; Version info (defaults to PRODUCT_VERSION unless overridden).
!ifndef MINI_COMPANY_NAME
  !define MINI_COMPANY_NAME "Velloc"
!endif
!ifndef MINI_FILE_DESCRIPTION
  !define MINI_FILE_DESCRIPTION "Velloc Agent Installer"
!endif
!ifndef MINI_PRODUCT_NAME
  !define MINI_PRODUCT_NAME "Velloc Agent Installer"
!endif
!ifndef MINI_COPYRIGHT
  !define MINI_COPYRIGHT "Copyright 2026 Velloc. All rights reserved."
!endif
!ifndef MINI_FILE_VERSION
  !define MINI_FILE_VERSION "${PRODUCT_VERSION}"
!endif
!ifndef MINI_PRODUCT_VERSION
  !define MINI_PRODUCT_VERSION "${PRODUCT_VERSION}"
!endif

Name "${PRODUCT_NAME}"
OutFile "${PRODUCT_NAME} Setup.exe"
BrandingText "${PRODUCT_NAME} Installer"
VIProductVersion "${MINI_FILE_VERSION}"
VIAddVersionKey "CompanyName" "${MINI_COMPANY_NAME}"
VIAddVersionKey "FileDescription" "${MINI_FILE_DESCRIPTION}"
VIAddVersionKey "FileVersion" "${MINI_FILE_VERSION}"
VIAddVersionKey "ProductName" "${MINI_PRODUCT_NAME}"
VIAddVersionKey "ProductVersion" "${MINI_PRODUCT_VERSION}"
VIAddVersionKey "LegalCopyright" "${MINI_COPYRIGHT}"
; RequestExecutionLevel highest
RequestExecutionLevel user
ShowInstDetails show
AutoCloseWindow true

!define MUI_ABORTWARNING
; Optional branding assets:
!define MUI_ICON "..\\chromium_overrides\\velloc\\symlink\\chrome\\app\\theme\\velloc\\win\\velloc.ico"
; !define MUI_HEADERIMAGE
; !define MUI_HEADERIMAGE_BITMAP "branding\\header.bmp"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var InstallMode
Var MiniArgs
Var MiniExit

Function .onInit
  StrCpy $InstallMode "user"
  ${GetParameters} $R0
  ${GetOptions} $R0 "/system" $R1
  ${IfNot} ${Errors}
    StrCpy $InstallMode "system"
  ${EndIf}
FunctionEnd

; Reads the installed product version (registry `pv`) for the current
; install mode into $R2 ("" if not installed).
!macro READ_INSTALL_INFO
  ${If} $InstallMode == "system"
    ReadRegStr $R2 HKLM "Software\\Velloc" "pv"
    ReadRegStr $R3 HKLM "Software\\Velloc" "UninstallString"
    ReadRegStr $R4 HKLM "Software\\Velloc" "UninstallArguments"
  ${Else}
    ReadRegStr $R2 HKCU "Software\\Velloc" "pv"
    ReadRegStr $R3 HKCU "Software\\Velloc" "UninstallString"
    ReadRegStr $R4 HKCU "Software\\Velloc" "UninstallArguments"
  ${EndIf}
!macroend

; Legacy-scheme migration. Releases up to custom_browser-v149.0.3.x were
; versioned {chromium major}.{minor}.{build}.{patch} — their first dotted
; field is a chromium major (144..149). The composed scheme
; ({CUSTOM_BROWSER_MAJOR}.{chromium major}.{interface}.{patch}, first field
; a small product major, documented to stay < 100) sorts BELOW them, so
; setup.exe would refuse the install as a downgrade (HIGHER_VERSION_EXISTS,
; exit code 4). Detect a legacy install (first field >= 100) and silently
; run its own recorded uninstaller first. User data is preserved (no
; --delete-profile). Success is verified by re-reading `pv` rather than by
; trusting setup.exe exit codes.
Function UninstallLegacySchemeInstall
  !insertmacro READ_INSTALL_INFO
  ${If} $R2 == ""
    Return                          ; nothing installed
  ${EndIf}
  ${WordFind} "$R2" "." "+1" $R5    ; first dotted field ("" -> 0 below)
  ${If} $R5 < 100
    Return                          ; already on the composed scheme
  ${EndIf}

  DetailPrint "Found legacy install $R2 — uninstalling it first..."
  ${If} $R3 == ""
    DetailPrint "Legacy install has no UninstallString; continuing (the mini installer will report the conflict)."
    Return
  ${EndIf}
  ; $R4 (UninstallArguments) is registry-recorded, e.g. " --uninstall".
  ; --force-uninstall makes Chromium's setup.exe uninstall without prompting.
  ExecWait '"$R3"$R4 --force-uninstall' $R6

  !insertmacro READ_INSTALL_INFO
  ${If} $R2 != ""
    ${WordFind} "$R2" "." "+1" $R5
    ${If} $R5 >= 100
      MessageBox MB_ICONSTOP "Could not remove the previous ${PRODUCT_NAME} version ($R2, uninstaller exit $R6). Close ${PRODUCT_NAME} and run this installer again, or uninstall it from Windows Settings first." /SD IDOK
      SetErrorLevel $R6
      Abort
    ${EndIf}
  ${EndIf}
  DetailPrint "Legacy install removed."
FunctionEnd

Section "Install"
  Call UninstallLegacySchemeInstall
  InitPluginsDir
  SetOutPath "$PLUGINSDIR"
  SetCompress off
  File "/oname=$PLUGINSDIR\\${MINI_INSTALLER_NAME}" "${MINI_INSTALLER_SOURCE}"
  File "/oname=$PLUGINSDIR\\${MASTER_PREFERENCES_NAME}" "${MASTER_PREFERENCES_SOURCE}"
  SetCompress auto

  StrCpy $MiniArgs "--installerdata=$\"$PLUGINSDIR\\${MASTER_PREFERENCES_NAME}$\""
  ${If} $InstallMode == "system"
    StrCpy $MiniArgs "$MiniArgs --system-level"
    DetailPrint "Running system-level mini installer..."
  ${Else}
    DetailPrint "Running per-user mini installer..."
  ${EndIf}

  ExecWait '"$PLUGINSDIR\\${MINI_INSTALLER_NAME}" $MiniArgs' $MiniExit

  ${If} $MiniExit == 0
    DetailPrint "Mini installer completed successfully."
  ${Else}
    DetailPrint "Mini installer failed with exit code $MiniExit."
    MessageBox MB_ICONSTOP "Installation failed (exit code $MiniExit)." /SD IDOK
    SetErrorLevel $MiniExit
    Abort
  ${EndIf}
SectionEnd

Function .onInstSuccess
FunctionEnd
