; Copyright 2026 VellocLabs
; Use of this source code is governed by the Apache License, Version 2.0,
; which can be found in the LICENSE file.
;
; Finds the running ${BROWSER_EXE} processes of one install directory.

!include "LogicLib.nsh"
!include "WordFunc.nsh"

!ifndef BROWSER_EXE
  !error "BROWSER_EXE is required, e.g. /DBROWSER_EXE=velloc.exe"
!endif

; The PowerShell expression that lists processes; a test overrides it to
; make the query fail.
!ifndef RUNNING_PRODUCT_QUERY
  !define RUNNING_PRODUCT_QUERY "Get-CimInstance Win32_Process"
!endif

; Contract: pops a string, pushes 1 when it is "" or "/PID <n>" repeated
; with single spaces between, else 0.
Function IsPidArgList
  Exch $0
  Push $1
  Push $2
  Push $3
  Push $4
  Push $5
  ${If} $0 == ""
    StrCpy $0 1
    Goto done
  ${EndIf}
  ClearErrors
  ${WordFind} "$0" " " "E#" $5
  IfErrors invalid
  IntOp $1 $5 % 2
  ${If} $1 != 0
  ${OrIf} $5 < 2
    Goto invalid
  ${EndIf}
  ; Rebuilt from its words, the string must come back unchanged, so no word
  ; is empty and every separator is one space.
  StrCpy $4 ""
  ${For} $2 1 $5
    ${WordFind} "$0" " " "+$2" $3
    IntOp $1 $2 % 2
    ${If} $1 == 1
      StrCmpS $3 "/PID" 0 invalid
    ${Else}
      StrCpy $1 $3 1
      StrCmpS $1 "-" invalid
      IntOp $1 $3 + 0
      StrCmpS $1 $3 0 invalid
    ${EndIf}
    ${If} $2 == 1
      StrCpy $4 $3
    ${Else}
      StrCpy $4 "$4 $3"
    ${EndIf}
  ${Next}
  StrCmpS $4 $0 0 invalid
  StrCpy $0 1
  Goto done
  invalid:
  StrCpy $0 0
  done:
  ClearErrors
  Pop $5
  Pop $4
  Pop $3
  Pop $2
  Pop $1
  Exch $0
FunctionEnd

; Contract: pops an install directory, pushes the running ${BROWSER_EXE}
; processes whose executable lies under it as taskkill arguments
; ("/PID 12 /PID 34"), or "" when there are none. Only tree roots are listed
; (a matching process whose parent also matches is left to "/T"), so the
; result stays short. A copy of the same file name elsewhere (a developer
; build, a test run) is not this install and is never listed.
; Security: a process whose path cannot be read (another user's, an elevated
; one) is listed, so Setup never replaces files a process it cannot inspect
; may hold. When the query fails, or returns anything but such a list, the
; result is "/IM ${BROWSER_EXE}", so taskkill only ever gets arguments built
; here.
Function FindProductPids
  Exch $0
  Push $1
  Push $2
  ${WordReplace} "$0" "'" "''" "+" $0
  nsExec::ExecToStack `"$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "$$ErrorActionPreference='Stop'; $$ProgressPreference='SilentlyContinue'; $$d=([IO.Path]::GetFullPath('$0')).TrimEnd([char]92)+[char]92; $$p=@(${RUNNING_PRODUCT_QUERY} | Where-Object { $$_.Name -eq '${BROWSER_EXE}' -and (-not $$_.ExecutablePath -or $$_.ExecutablePath.StartsWith($$d,[StringComparison]::OrdinalIgnoreCase)) }); $$ids=@($$p | ForEach-Object { $$_.ProcessId }); [Console]::Out.Write((@($$p | Where-Object { $$ids -notcontains $$_.ParentProcessId } | ForEach-Object { '/PID ' + $$_.ProcessId }) -join ' '))"`
  Pop $1
  Pop $2
  ${If} $1 == 0
    Push $2
    Call IsPidArgList
    Pop $1
    ${If} $1 != 1
      StrCpy $2 '/IM "${BROWSER_EXE}"'
    ${EndIf}
  ${Else}
    StrCpy $2 '/IM "${BROWSER_EXE}"'
  ${EndIf}
  StrCpy $0 $2
  Pop $2
  Pop $1
  Exch $0
FunctionEnd
