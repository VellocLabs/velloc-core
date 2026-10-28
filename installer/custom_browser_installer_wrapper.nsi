; Minimal NSIS wrapper for mini_installer.exe
; Usage:
;   makensis.exe /DPRODUCT_NAME="Custom Browser" /DMINI_INSTALLER_SOURCE="path\\to\\mini_installer.exe" custom_browser_installer_wrapper.nsi
;   CustomBrowserSetup.exe /system   ; for system-level (requires admin)

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "WordFunc.nsh"
!include "installer_channel.nsh"

; Signed copies of NSIS's plug-in DLLs (build.sh, velloc_sign_nsis_plugins).
; The installer unpacks its plug-ins into $PLUGINSDIR at run time, and the
; stock ones are unsigned; Store policy 10.2.9 wants every PE file of the
; installer signed. A dir added here outranks the stock one for a plug-in of
; the same name.
!ifdef VELLOC_NSIS_PLUGIN_DIR
  !addplugindir /x86-unicode "${VELLOC_NSIS_PLUGIN_DIR}"
!endif

; The installed browser executable's file name (custom_browser_exe_name +
; ".exe"); build.sh reads it from the out dir's args.gn.
!ifndef BROWSER_EXE
  !error "BROWSER_EXE is required, e.g. /DBROWSER_EXE=velloc.exe"
!endif
!include "running_product.nsh"

!ifndef PRODUCT_NAME
  !define PRODUCT_NAME "Velloc Code"
!endif
!ifndef PRODUCT_PUBLISHER
  !define PRODUCT_PUBLISHER "VellocLabs"
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

; Version info (defaults to PRODUCT_VERSION unless overridden).
!ifndef MINI_COMPANY_NAME
  !define MINI_COMPANY_NAME "VellocLabs"
!endif
!ifndef MINI_FILE_DESCRIPTION
  !define MINI_FILE_DESCRIPTION "Velloc Code Installer"
!endif
!ifndef MINI_PRODUCT_NAME
  !define MINI_PRODUCT_NAME "Velloc Code"
!endif
!ifndef MINI_COPYRIGHT
  !define MINI_COPYRIGHT "Copyright 2026 VellocLabs. All rights reserved."
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
Var InstallerChannel
Var ProductDir
Var RunningPids

; The Uninstall registry subkey is named after the install path component
; (kProductPathName = "Velloc"), not the display name ${PRODUCT_NAME} ("Velloc
; Code") — confirmed against the registered install.
!define PRODUCT_UNINSTALL_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Velloc"

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

; Chromium refuses to see this product's versions as newer than its own baked-in
; "last breaking installer version" — kLastBreakingInstallerVersion = "85.0.4169.0"
; (chrome/installer/setup/last_breaking_installer_version.cc). Our composed scheme's
; FIRST field is a small product major (documented < 100; currently 1), so
; base::Version compares 1.149.1.x BELOW 85.0.4169.0 and this guard in
; AddDowngradeCleanupItems never short-circuits:
;
;     if (GetLastBreakingInstallerVersion(reg_root) <= new_version) return false;
;
; So EVERY install is treated as a downgrade across a breaking installer version,
; and setup.exe runs the stored DowngradeCleanupCommand — the PREVIOUS version's
; setup.exe. Chromium deletes old version directories after an upgrade, so that
; path eventually stops existing; the launch then fails, the work-item list
; unwinds, and the whole install rolls back as the generic INSTALL_FAILED
; (exit code 7). Nothing in that message hints at any of it, and because the
; rollback restores the stale command, the NEXT install fails the same way —
; it presents as a machine that permanently stopped accepting updates.
;
; Deleting the value makes GetLastBreakingInstallerVersion return nullopt, which
; compares less than any version, so the guard short-circuits as intended. That is
; the correct behaviour rather than a workaround: the gate exists to force a clean
; install when downgrading across Chromium 85, and no Velloc version has ever been
; below its own 1.x scheme, so the comparison can only ever fire spuriously here.
; setup.exe rewrites the value later in the SAME install (AddFinalizeUpdateWorkItems
; writes it after AddDowngradeCleanupItems has already read it), so clearing it
; here applies to this install and leaves the registry as Chromium expects.
Function ClearBreakingInstallerVersionGate
  ${If} $InstallMode == "system"
    DeleteRegValue HKLM "Software\Velloc" "CleanInstallRequiredForVersionBelow"
  ${Else}
    DeleteRegValue HKCU "Software\Velloc" "CleanInstallRequiredForVersionBelow"
  ${EndIf}
  DetailPrint "Cleared the stale breaking-installer-version gate."
FunctionEnd

; Velloc has no browser first run: Chromium treats every launch as its first
; run while "First Run" (chrome::kFirstRunSentinel) is missing from the user
; data dir (%LOCALAPPDATA%\<kProductPathName>\User Data), and then opens its
; welcome window and first-run tabs. Writing the sentinel here skips both.
; It is written before the mini installer runs: a first install launches the
; browser from inside it unless do_not_launch_chrome is set, and that launch
; must already find the sentinel. A failed install leaves it in place, which
; only skips a first run Velloc never shows. It covers the account running
; the installer only; an existing sentinel is left untouched.
Function SkipBrowserFirstRun
  StrCpy $R0 "$LOCALAPPDATA\\Velloc\\User Data"
  ${If} ${FileExists} "$R0\\First Run"
    Return
  ${EndIf}
  CreateDirectory "$R0"
  ClearErrors
  FileOpen $R1 "$R0\\First Run" w
  ${If} ${Errors}
    DetailPrint "Could not write the first-run sentinel in $R0."
    Return
  ${EndIf}
  FileClose $R1
  DetailPrint "Wrote the first-run sentinel in $R0."
FunctionEnd

; An installer whose file name contains "beta" (any case) installs onto the
; beta runtime channel: it writes the channel name to "Installer Channel" in
; the user data dir, which the browser applies to its runtime URL at its next
; start and then deletes (G-3). Written before the mini installer runs, so no
; launch can miss it. Covers the account running the installer only.
!define INSTALLER_CHANNEL_MARKER "$LOCALAPPDATA\\Velloc\\User Data\\Installer Channel"

Function RecordInstallerChannel
  !insertmacro INSTALLER_CHANNEL_FROM_NAME $InstallerChannel "$EXEFILE"
  ${If} $InstallerChannel == ""
    Return
  ${EndIf}
  CreateDirectory "$LOCALAPPDATA\\Velloc\\User Data"
  ClearErrors
  FileOpen $R1 "${INSTALLER_CHANNEL_MARKER}" w
  ${If} ${Errors}
    DetailPrint "Could not write the channel marker ${INSTALLER_CHANNEL_MARKER}."
    StrCpy $InstallerChannel ""
    Return
  ${EndIf}
  FileWrite $R1 "$InstallerChannel"
  FileClose $R1
  DetailPrint "Installing on the $InstallerChannel channel."
FunctionEnd

; Running-instance handling. The mini installer replaces files side by side,
; so no reboot is needed, but the post-install launch only routes to a running
; old instance: the user would keep seeing the old version. Setup therefore
; closes the ${BROWSER_EXE} processes running from the directory it installs
; into first: it asks (interactive installs), requests a normal close, waits,
; force-ends what is left, and offers Retry / Cancel when something will not
; go (another user's session, an elevated process) instead of failing. A
; silent install closes without asking. A ${BROWSER_EXE} elsewhere (a
; developer build, a test run) is left alone.

; Sets $ProductDir to the directory this install writes to: the registry
; InstallLocation setup.exe recorded for the install mode, else that mode's
; default directory.
Function ResolveProductDir
  ${If} $InstallMode == "system"
    ReadRegStr $ProductDir HKLM "${PRODUCT_UNINSTALL_KEY}" "InstallLocation"
    ${If} $ProductDir == ""
      StrCpy $ProductDir "$PROGRAMFILES64\\Velloc\\Application"
    ${EndIf}
  ${Else}
    ReadRegStr $ProductDir HKCU "${PRODUCT_UNINSTALL_KEY}" "InstallLocation"
    ${If} $ProductDir == ""
      StrCpy $ProductDir "$LOCALAPPDATA\\Velloc\\Application"
    ${EndIf}
  ${EndIf}
FunctionEnd

; Sets $R0 to 1 when a ${BROWSER_EXE} runs from $ProductDir, else 0;
; $RunningPids holds the taskkill arguments that name them.
Function IsProductRunning
  StrCpy $R0 0
  Push $ProductDir
  Call FindProductPids
  Pop $RunningPids
  ${If} $RunningPids != ""
    StrCpy $R0 1
  ${EndIf}
FunctionEnd

; Checks up to 12 more times, half a second apart, for the processes to exit;
; leaves $R0 = 1 if any remain.
Function WaitProductExit
  StrCpy $R9 0
  ${Do}
    Call IsProductRunning
    ${If} $R0 == 0
      ${Break}
    ${EndIf}
    ${If} $R9 >= 12
      ${Break}
    ${EndIf}
    Sleep 500
    IntOp $R9 $R9 + 1
  ${Loop}
FunctionEnd

Function CloseRunningProduct
  Call ResolveProductDir
  ${Do}
    Call IsProductRunning
    ${If} $R0 == 0
      Return
    ${EndIf}
    ${IfNot} ${Silent}
      MessageBox MB_ICONQUESTION|MB_YESNO "${PRODUCT_NAME} is running. Setup has to close it to continue; running agent sessions will be interrupted and can be resumed from the session list. Close ${PRODUCT_NAME} and continue?" IDYES +2
      Goto cancelled
    ${EndIf}
    DetailPrint "Closing ${PRODUCT_NAME}..."
    nsExec::Exec 'taskkill $RunningPids'
    Pop $R1
    Call WaitProductExit
    ${If} $R0 == 1
      DetailPrint "${PRODUCT_NAME} did not close in time; ending it."
      nsExec::Exec 'taskkill /F /T $RunningPids'
      Pop $R1
      Call WaitProductExit
    ${EndIf}
    ${If} $R0 == 0
      DetailPrint "${PRODUCT_NAME} closed."
      Return
    ${EndIf}
    ${If} ${Silent}
      DetailPrint "${PRODUCT_NAME} is still running; continuing."
      Return
    ${EndIf}
    MessageBox MB_ICONEXCLAMATION|MB_RETRYCANCEL "Setup could not close ${PRODUCT_NAME}. It may be running in another user session or with higher privileges. Close it yourself, then choose Retry." IDRETRY retry
    Goto cancelled
    retry:
  ${Loop}
  cancelled:
  DetailPrint "Cancelled: ${PRODUCT_NAME} is still running."
  SetErrorLevel 1602
  Abort
FunctionEnd

Section "Install"
  Call CloseRunningProduct
  Call ClearBreakingInstallerVersionGate
  Call UninstallLegacySchemeInstall
  Call RecordInstallerChannel
  Call SkipBrowserFirstRun
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
    ${If} $InstallerChannel != ""
      Delete "${INSTALLER_CHANNEL_MARKER}"
    ${EndIf}
    MessageBox MB_ICONSTOP "Installation failed (exit code $MiniExit)." /SD IDOK
    SetErrorLevel $MiniExit
    Abort
  ${EndIf}
SectionEnd

; Resolves the installed ${BROWSER_EXE} path into $R7 from ResolveProductDir.
; Sets $R7 to "" if the exe cannot be found.
Function ResolveInstalledChromeExe
  Call ResolveProductDir
  StrCpy $R7 "$ProductDir\\${BROWSER_EXE}"
  ${IfNot} ${FileExists} "$R7"
    StrCpy $R7 ""
  ${EndIf}
FunctionEnd

; Brings up the Velloc main window after an install. If no instance is running
; this launches a new one; if one is running in the background the process
; singleton routes the launch to it and raises its existing main window
; (SetForegroundWindow + restore). Runs on every interactive per-user
; install, not just first install — this is what makes an overwrite install
; surface the window. Silent and system-level installs do not launch.
Function LaunchInstalledChrome
  ; A system-level install runs elevated, and Exec inherits this process's
  ; token: the browser, its renderers and the shell utility process would all
  ; run as administrator. setup.exe never launches on a system-level install
  ; for the same reason — mirror it and let the user start Velloc themselves.
  ${If} $InstallMode == "system"
    DetailPrint "System-level install: not launching ${PRODUCT_NAME} from the elevated installer."
    Return
  ${EndIf}
  ; A silent (/S) install is run by a tool — a deployment script, the
  ; Microsoft Store's install of the .exe — with no user waiting for a
  ; window; Store policy 10.2.9 wants it to finish without UI.
  ${If} ${Silent}
    DetailPrint "Silent install: not launching ${PRODUCT_NAME}."
    Return
  ${EndIf}
  Call ResolveInstalledChromeExe
  ${If} $R7 != ""
    DetailPrint "Launching ${PRODUCT_NAME}..."
    ; "--from-installer" marks the launch as installer-driven. setup.exe's
    ; own first-install launch is off (do_not_launch_chrome in
    ; master_preferences), so this is the only post-install launch.
    Exec '"$R7" --from-installer'
  ${Else}
    DetailPrint "Could not locate installed ${BROWSER_EXE}; skipping launch."
  ${EndIf}
FunctionEnd

Function .onInstSuccess
  Call LaunchInstalledChrome
FunctionEnd
