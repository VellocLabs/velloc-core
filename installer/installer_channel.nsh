; Copyright 2026 VellocLabs
; Use of this source code is governed by the Apache License, Version 2.0,
; which can be found in the LICENSE file.
;
; The runtime channel an installer's own file name selects (G-3).

!include "LogicLib.nsh"
!include "StrFunc.nsh"
${StrStr}

; Sets _out to "beta" when _name contains "beta" in any case, else to "".
!macro INSTALLER_CHANNEL_FROM_NAME _out _name
  ${StrStr} ${_out} "${_name}" "beta"
  ${If} ${_out} != ""
    StrCpy ${_out} "beta"
  ${EndIf}
!macroend
