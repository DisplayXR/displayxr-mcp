; DisplayXR MCP Tools Installer
; Copyright 2026, DisplayXR
; SPDX-License-Identifier: BSL-1.0
;
; Standalone installer for the DisplayXR MCP (Model Context Protocol)
; tools. Ships the displayxr-mcp.exe stdio↔pipe adapter and writes a
; capability registry key that the runtime + shell read at startup to
; decide whether to spawn their MCP server threads.
;
; Discovery contract: writes
;   HKLM\Software\DisplayXR\Capabilities\MCP\Enabled = 1
;   HKLM\Software\DisplayXR\Capabilities\MCP\AdapterPath = <path-to-exe>
;   HKLM\Software\DisplayXR\Capabilities\MCP\Version = <version>
;
; The runtime DLL (Phase A introspection) and displayxr-shell.exe
; (Phase B workspace control) each query this key at their startup.
; Install order vs. shell does not matter — they check independently.
;
; The DISPLAYXR_MCP env var still works as a force-enable / force-
; disable override for dev / CI workflows.
;
; The DisplayXR runtime is a soft prereq — without it there are no MCP
; servers to attach to — but this installer does not require it; agents
; / tools can land first and the capability flips on as soon as the
; runtime is present.

;--------------------------------
; Build-time definitions (passed from CMake)
; VERSION, VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH
; BIN_DIR, SOURCE_DIR, OUTPUT_DIR

!ifndef VERSION
	!define VERSION "0.0.0"
!endif
!ifndef VERSION_MAJOR
	!define VERSION_MAJOR "0"
!endif
!ifndef VERSION_MINOR
	!define VERSION_MINOR "0"
!endif
!ifndef VERSION_PATCH
	!define VERSION_PATCH "0"
!endif
!ifndef BUILD_NUM
	!define BUILD_NUM "0"
!endif

;--------------------------------
; General Attributes

Name "DisplayXR MCP Tools ${VERSION}"
OutFile "${OUTPUT_DIR}\DisplayXRMCPSetup-${VERSION}.${BUILD_NUM}.exe"
InstallDir "$PROGRAMFILES64\DisplayXR\MCP"
RequestExecutionLevel admin
ShowInstDetails show
ShowUninstDetails show

; Modern UI
!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"
!include "LogicLib.nsh"

;--------------------------------
; Interface Settings

!define MUI_ABORTWARNING

;--------------------------------
; Pages

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SOURCE_DIR}\LICENSE"
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

;--------------------------------
; Languages

!insertmacro MUI_LANGUAGE "English"

;--------------------------------
; Installer Section

Section "DisplayXR MCP Tools" SecMCP
	SectionIn RO  ; Required section

	; Force 64-bit registry view so HKLM\Software\DisplayXR\* lands in
	; the non-redirected view that the runtime + shell read via
	; KEY_WOW64_64KEY. Matches the runtime + shell installers.
	SetRegView 64

	SetOutPath "$INSTDIR\bin"

	; Stop any running adapter to release the exe (typically not held
	; long, but a long-lived voice CLI session could be holding it).
	DetailPrint "Stopping any running displayxr-mcp adapter..."
	nsExec::ExecToLog 'taskkill /f /im displayxr-mcp.exe'
	Pop $0
	Sleep 500

	; Install the adapter binary.
	File "${BIN_DIR}\displayxr-mcp.exe"

	; Self-uninstaller. Distinct name so it doesn't collide with the
	; runtime's Uninstall.exe or the shell's Uninstall-Shell.exe.
	WriteUninstaller "$INSTDIR\Uninstall-MCP.exe"

	; Capability registration — the runtime + shell read this at
	; startup. Sibling extension point to
	; HKLM\Software\DisplayXR\WorkspaceControllers\<name> (used by the
	; shell installer for process plugins). Capabilities\<name> is for
	; flag-style features that gate behavior in already-running binaries.
	WriteRegDWORD HKLM "Software\DisplayXR\Capabilities\MCP" \
		"Enabled" 1
	WriteRegStr   HKLM "Software\DisplayXR\Capabilities\MCP" \
		"AdapterPath" "$INSTDIR\bin\displayxr-mcp.exe"
	WriteRegStr   HKLM "Software\DisplayXR\Capabilities\MCP" \
		"Version" "${VERSION}"
	WriteRegStr   HKLM "Software\DisplayXR\Capabilities\MCP" \
		"InstallPath" "$INSTDIR"
	WriteRegStr   HKLM "Software\DisplayXR\Capabilities\MCP" \
		"UninstallString" "$\"$INSTDIR\Uninstall-MCP.exe$\""

	; Add/Remove Programs entry.
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRMCP" \
		"DisplayName" "DisplayXR MCP Tools"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRMCP" \
		"UninstallString" "$\"$INSTDIR\Uninstall-MCP.exe$\""
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRMCP" \
		"QuietUninstallString" "$\"$INSTDIR\Uninstall-MCP.exe$\" /S"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRMCP" \
		"InstallLocation" "$INSTDIR"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRMCP" \
		"DisplayIcon" "$INSTDIR\bin\displayxr-mcp.exe"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRMCP" \
		"Publisher" "DisplayXR"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRMCP" \
		"DisplayVersion" "${VERSION}"
	WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRMCP" \
		"URLInfoAbout" "https://github.com/DisplayXR/displayxr-mcp"
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRMCP" \
		"VersionMajor" ${VERSION_MAJOR}
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRMCP" \
		"VersionMinor" ${VERSION_MINOR}
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRMCP" \
		"NoModify" 1
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRMCP" \
		"NoRepair" 1

	; Calculate installed size (for Add/Remove Programs).
	${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
	IntFmt $0 "0x%08X" $0
	WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRMCP" \
		"EstimatedSize" "$0"
SectionEnd

;--------------------------------
; Uninstaller Section

Section "Uninstall"
	; Same 64-bit view as the install section.
	SetRegView 64

	; Stop any running adapter.
	DetailPrint "Stopping any running displayxr-mcp adapter..."
	nsExec::ExecToLog 'taskkill /f /im displayxr-mcp.exe'
	Sleep 500

	; Remove only files we installed.
	Delete "$INSTDIR\bin\displayxr-mcp.exe"
	Delete "$INSTDIR\Uninstall-MCP.exe"
	RMDir "$INSTDIR\bin"
	RMDir "$INSTDIR"

	; Capability key — drop the entire subkey so the runtime + shell
	; both see the capability go away on next launch.
	DeleteRegKey HKLM "Software\DisplayXR\Capabilities\MCP"
	; Prune the Capabilities and DisplayXR parents only if no siblings
	; (other Capabilities\<name> or other DisplayXR\<component> entries)
	; remain. /ifempty is the safe form — it's a no-op if subkeys exist.
	; Without this, `reg query HKLM\Software\DisplayXR` after a full
	; orchestrator --uninstall returns an empty Capabilities parent
	; instead of the expected "key not found".
	DeleteRegKey /ifempty HKLM "Software\DisplayXR\Capabilities"
	DeleteRegKey /ifempty HKLM "Software\DisplayXR"

	DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DisplayXRMCP"
SectionEnd

;--------------------------------
; Section Descriptions

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
	!insertmacro MUI_DESCRIPTION_TEXT ${SecMCP} "Adapter binary + capability registration so the runtime + shell expose MCP servers for agent / voice control."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

;--------------------------------
; Installer Functions

Function .onInit
	; 64-bit Windows check.
	${IfNot} ${RunningX64}
		MessageBox MB_ICONSTOP "DisplayXR MCP Tools requires 64-bit Windows."
		Abort
	${EndIf}

	; Force 64-bit registry view — see install section comment for rationale.
	SetRegView 64

	; Soft prereq notice — without the runtime there's nothing for MCP
	; clients to attach to. Don't abort: tools can land first and the
	; capability flips on automatically when the runtime arrives.
	ReadRegStr $R0 HKLM "Software\DisplayXR\Runtime" "InstallPath"
	${If} $R0 == ""
		MessageBox MB_OKCANCEL|MB_ICONINFORMATION \
			"DisplayXR Runtime is not installed yet.$\r$\n$\r$\n\
			The MCP adapter on its own has nothing to attach to. \
			Install the runtime (and the shell, if you want workspace control) for the capability to do anything.$\r$\n$\r$\n\
			Continue installing MCP Tools anyway?" \
			IDOK +2
		Abort
	${EndIf}
FunctionEnd

Function un.onInit
	${IfNot} ${RunningX64}
		MessageBox MB_ICONSTOP "Uninstall requires 64-bit Windows."
		Abort
	${EndIf}
	SetRegView 64
FunctionEnd

;--------------------------------
; Version Information

VIProductVersion "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}.0"
VIAddVersionKey "ProductName" "DisplayXR MCP Tools"
VIAddVersionKey "CompanyName" "DisplayXR"
VIAddVersionKey "LegalCopyright" "Copyright (c) 2026 DisplayXR"
VIAddVersionKey "FileDescription" "DisplayXR MCP Tools Installer"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
