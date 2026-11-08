Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "windows-version.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"
!include "WordFunc.nsh"
!include "nsDialogs.nsh"
!ifdef INSTALLER_TEST
SetCompressor zlib
!else
SetCompressor zlib
!endif
CRCCheck force
Name "LocalAudio ${EDITION} · Design by ningq"
OutFile "${OUTPUT_FILE}"
RequestExecutionLevel user
InstallDir ""
BrandingText "Design by ningq"
ShowInstDetails show
ShowUninstDetails show
VIProductVersion "2.5.2.0"
VIAddVersionKey "ProductName" "LocalAudio 独立插件"
VIAddVersionKey "FileDescription" "LocalAudio ${EDITION}独立插件安装包"
VIAddVersionKey "LegalCopyright" "Design by ningq; third-party notices included"
VIAddVersionKey "FileVersion" "2.5.2"
VIAddVersionKey "Comments" "Design by ningq"
!define MUI_WELCOMEPAGE_TITLE "安装 LocalAudio ${EDITION}"
!define MUI_WELCOMEPAGE_TEXT "Design by ningq。$\r$\n$\r$\n独立插件：AI 麦克风降噪与应用音频共享；屏幕版另含屏幕共享。$\r$\n$\r$\n安装前完全退出 TS。需要已有的官方 TeamSpeak 3.6.2（64 位）。首次启动有两步纯文字教程，安装不会自动开启采集。"
!define MUI_FINISHPAGE_TEXT "LocalAudio 已安装。Design by ningq。$\r$\n$\r$\n重启 TS 后按教程设置 AI 降噪和音频共享。可从插件菜单重新打开教程。"
!define MUI_DIRECTORYPAGE_TEXT_TOP "选择 TeamSpeak 的用户设置目录。标准安装保留默认目录；使用 -localconfig 的便携客户端应选择该客户端的 config 子目录。插件写入此目录下的 plugins，客户端主程序保持不变。"
!insertmacro MUI_PAGE_WELCOME
Page custom SelectClient LeaveClient
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
Var ClientRoot
Var ClientInput
Var ExitCode
Var BackupDir
Var StageDir
Var RuntimeMajor
Var RuntimeCandidate
!define REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalAudio-Plugin"
!ifdef INSTALLER_TEST
!undef REGKEY
!define REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalAudio-Plugin-IsolatedTest"
!endif

!macro Candidate P
 ${If} $ClientRoot == ""
 ${AndIf} ${FileExists} "${P}\ts3client_win64.exe"
  StrCpy $ClientRoot "${P}"
 ${EndIf}
!macroend
Function .onInit
 ${IfNot} ${RunningX64}
  MessageBox MB_ICONSTOP "需要 64 位 Windows 和 TeamSpeak 3。" /SD IDOK
  SetErrorLevel 2
  Quit
 ${EndIf}
 SetRegView 64
 ReadRegStr $0 HKLM "SOFTWARE\Microsoft\Windows NT\CurrentVersion" "CurrentBuildNumber"
 !insertmacro LA_CheckWindowsBuild $0
 ${If} $INSTDIR == ""
  StrCpy $INSTDIR "$APPDATA\TS3Client"
 ${EndIf}
 StrCpy $ClientRoot ""
 ${GetParameters} $0
 ${GetOptions} $0 "/CLIENT=" $ClientRoot
 ${If} $ClientRoot == ""
  ReadRegStr $1 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TeamSpeak 3 Client" "InstallLocation"
  !insertmacro Candidate "$1"
  ReadRegStr $1 HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\TeamSpeak 3 Client" "InstallLocation"
  !insertmacro Candidate "$1"
  !insertmacro Candidate "$PROGRAMFILES64\TeamSpeak 3 Client"
  !insertmacro Candidate "$LOCALAPPDATA\Programs\TeamSpeak 3 Client"
 ${EndIf}
FunctionEnd

Function SelectClient
 !insertmacro MUI_HEADER_TEXT "选择已有的 TS 客户端" "这里只检查版本，不安装或修改客户端。"
 nsDialogs::Create 1018
 Pop $0
 ${NSD_CreateLabel} 0 0 100% 45u "选择已经安装的官方 TeamSpeak 3.6.2（64 位）目录。此步骤只检查版本，不会安装或更改客户端文件。"
 Pop $0
 ${NSD_CreateDirRequest} 0 55u 78% 14u "$ClientRoot"
 Pop $ClientInput
 ${NSD_CreateBrowseButton} 80% 55u 20% 14u "浏览…"
 Pop $0
 ${NSD_OnClick} $0 BrowseClient
 nsDialogs::Show
FunctionEnd
Function BrowseClient
 nsDialogs::SelectFolderDialog "选择含 ts3client_win64.exe 的文件夹" "$ClientRoot"
 Pop $0
 ${If} $0 != error
  StrCpy $ClientRoot $0
  ${NSD_SetText} $ClientInput $0
 ${EndIf}
FunctionEnd
Function LeaveClient
 ${NSD_GetText} $ClientInput $ClientRoot
 Call ValidateClient
FunctionEnd
Function ValidateClient
 ${If} ${FileExists} "$ClientRoot\plugins\ts3localaudio.dll"
 ${OrIf} ${FileExists} "$ClientRoot\plugins\ts3localaudio_win64.dll"
  MessageBox MB_ICONSTOP "已有客户端目录中存在旧版 LocalAudio 全局插件。请先用旧安装器卸载该插件，再安装本独立版；不要同时加载两份。" /SD IDOK
  SetErrorLevel 2
  Abort
 ${EndIf}
 ${IfNot} ${FileExists} "$ClientRoot\ts3client_win64.exe"
  MessageBox MB_ICONSTOP "未找到已有的 TeamSpeak 3 64 位客户端。本安装包只安装插件。" /SD IDOK
  SetErrorLevel 2
  Abort
 ${EndIf}
 GetDLLVersion "$ClientRoot\ts3client_win64.exe" $0 $1
 ${If} $0 != 196614
 ${OrIf} $1 != 131072
  MessageBox MB_ICONSTOP "此版本仅验证并支持 TeamSpeak 3.6.2（64 位），请选择该版本。" /SD IDOK
  SetErrorLevel 2
  Abort
 ${EndIf}
 ; Refuse accidental installation into the executable directory itself.
 ${If} ${FileExists} "$INSTDIR\ts3client_win64.exe"
  MessageBox MB_ICONSTOP "这里应选择 TS 用户设置目录，而不是客户端主程序目录。便携版请选择其 config 子目录。" /SD IDOK
  SetErrorLevel 2
  Abort
 ${EndIf}
FunctionEnd

!macro SafeProfile PREFIX
Function ${PREFIX}SafeProfile
 ; Refuse directory reparse points before backup, installation or uninstall.
 ${ForEach} $0 0 8 + 1
  StrCpy $1 "$INSTDIR"
  ${If} $0 == 1
   StrCpy $1 "$INSTDIR\plugins"
  ${ElseIf} $0 == 2
   StrCpy $1 "$INSTDIR\plugins\ts3localaudio"
  ${ElseIf} $0 == 3
   StrCpy $1 "$INSTDIR\LocalAudio-Plugin"
  ${ElseIf} $0 == 4
   StrCpy $1 "$INSTDIR\plugins\ts3localaudio\ui"
  ${ElseIf} $0 == 5
   StrCpy $1 "$INSTDIR\plugins\ts3localaudio\denoise"
  ${ElseIf} $0 == 6
   StrCpy $1 "$INSTDIR\plugins\ts3localaudio\licenses"
  ${ElseIf} $0 == 7
   StrCpy $1 "$INSTDIR\plugins\ts3localaudio\source"
  ${ElseIf} $0 == 8
   StrCpy $1 "$INSTDIR\LocalAudio-Plugin\backups"
  ${EndIf}
  System::Call 'kernel32::GetFileAttributesW(w r1) i.r2'
  ${If} $2 != -1
   IntOp $2 $2 & 1024
   ${If} $2 != 0
    MessageBox MB_ICONSTOP "目标目录包含链接或重解析点，已停止安装/卸载。" /SD IDOK
    SetErrorLevel 2
    Abort
   ${EndIf}
  ${EndIf}
 ${Next}
FunctionEnd
!macroend
!insertmacro SafeProfile ""
!insertmacro SafeProfile "un."
!macro LockCheck PREFIX
Function ${PREFIX}LockCheck
 Call ${PREFIX}SafeProfile
 InitPluginsDir
 SetOutPath "$PLUGINSDIR"
 File /oname=client-check.exe "${CLIENT_CHECK}"
 ClearErrors
 ExecWait '"$PLUGINSDIR\client-check.exe" "$ClientRoot\ts3client_win64.exe"' $ExitCode
 ${If} ${Errors}
  SetErrorLevel 2
  Abort "无法检查客户端运行状态，尚未更改插件。"
 ${EndIf}
 ${If} $ExitCode != 0
  MessageBox MB_ICONSTOP "所选 TS 客户端仍在运行，或无法确认运行状态。请退出该客户端及托盘图标后重试。" /SD IDOK
  SetErrorLevel 2
  Abort
 ${EndIf}
 ${If} ${FileExists} "$INSTDIR\plugins\ts3localaudio.dll"
  ClearErrors
  FileOpen $0 "$INSTDIR\plugins\ts3localaudio.dll" a
  ${If} ${Errors}
   MessageBox MB_ICONSTOP "插件文件被使用或不可写，请退出对应 TS 客户端并检查目录权限。" /SD IDOK
   SetErrorLevel 2
   Abort
  ${EndIf}
  FileClose $0
 ${EndIf}
 ${If} ${FileExists} "$INSTDIR\plugins\ts3localaudio_win64.dll"
  ClearErrors
  FileOpen $0 "$INSTDIR\plugins\ts3localaudio_win64.dll" a
  ${If} ${Errors}
   MessageBox MB_ICONSTOP "插件文件被使用或不可写，请退出对应 TS 客户端并检查目录权限。" /SD IDOK
   SetErrorLevel 2
   Abort
  ${EndIf}
  FileClose $0
 ${EndIf}
FunctionEnd
!macroend
!insertmacro LockCheck ""
!insertmacro LockCheck "un."

Function CheckRuntimeCandidate
 ${WordFind} "$RuntimeCandidate" "." "+1" $0
 ${If} $0 > $RuntimeMajor
  StrCpy $RuntimeMajor $0
 ${EndIf}
FunctionEnd
Function DetectWebView
 StrCpy $RuntimeMajor 0
 SetRegView 32
 ReadRegStr $RuntimeCandidate HKCU "Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" "pv"
 Call CheckRuntimeCandidate
 ReadRegStr $RuntimeCandidate HKLM "Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" "pv"
 Call CheckRuntimeCandidate
 SetRegView 64
 ReadRegStr $RuntimeCandidate HKCU "Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" "pv"
 Call CheckRuntimeCandidate
 ReadRegStr $RuntimeCandidate HKLM "Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}" "pv"
 Call CheckRuntimeCandidate
FunctionEnd
Function EnsureRuntimes
 Call DetectWebView
 ${If} $RuntimeMajor < 120
  !ifdef INSTALLER_TEST
   SetErrorLevel 2
   Abort "隔离测试不安装全局运行库。"
  !else
   SetOutPath "$PLUGINSDIR"
   File /oname=webview2.exe "${WEBVIEW_PAYLOAD}"
   ClearErrors
   ExecWait '"$PLUGINSDIR\webview2.exe" /silent /install' $ExitCode
   ${If} ${Errors}
    SetErrorLevel 2
    Abort "无法启动 WebView2 安装程序。"
   ${EndIf}
   ${If} $ExitCode != 0
   ${AndIf} $ExitCode != 3010
   ${AndIf} $ExitCode != -2147219198
    SetErrorLevel 2
    Abort "WebView2 安装失败；尚未更改插件文件。"
   ${EndIf}
   ${If} $ExitCode == 3010
    SetRebootFlag true
   ${EndIf}
   Call DetectWebView
   ${If} $RuntimeMajor < 120
    SetErrorLevel 2
    Abort "WebView2 安装后版本验证失败；尚未更改插件。"
   ${EndIf}
  !endif
 ${EndIf}
 ReadRegDWORD $0 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" "Installed"
 ReadRegDWORD $1 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" "Minor"
 ${If} $0 != 1
 ${OrIf} $1 < 51
  !ifdef INSTALLER_TEST
   SetErrorLevel 2
   Abort "隔离测试不安装全局运行库。"
  !else
   SetOutPath "$PLUGINSDIR"
   File /oname=vc_redist.x64.exe "${VCREDIST_PAYLOAD}"
   ClearErrors
   ExecWait '"$PLUGINSDIR\vc_redist.x64.exe" /install /quiet /norestart' $ExitCode
   ${If} ${Errors}
    SetErrorLevel 2
    Abort "无法启动 VC++ 安装程序。"
   ${EndIf}
   ${If} $ExitCode != 0
   ${AndIf} $ExitCode != 3010
    SetErrorLevel 2
    Abort "VC++ 安装失败；尚未更改插件文件。"
   ${EndIf}
   ${If} $ExitCode == 3010
    SetRebootFlag true
   ${EndIf}
   ReadRegDWORD $0 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" "Installed"
   ReadRegDWORD $1 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" "Minor"
   ${If} $0 != 1
   ${OrIf} $1 < 51
    SetErrorLevel 2
    Abort "VC++ 安装后版本验证失败；尚未更改插件。"
   ${EndIf}
  !endif
 ${EndIf}
FunctionEnd

Section "LocalAudio 独立插件"
 Call ValidateClient
 Call LockCheck
 InitPluginsDir
 Call EnsureRuntimes
 System::Call 'kernel32::GetTickCount64() l.r0'
 System::Call 'kernel32::GetCurrentProcessId() i.r1'
 StrCpy $BackupDir "$INSTDIR\LocalAudio-Plugin\backups\$0-$1"
 StrCpy $StageDir "$INSTDIR\LocalAudio-Plugin\stage-$0-$1"
 ClearErrors
 SetOutPath "$StageDir"
 File /r "${PACKAGE_FILES}\plugins\*.*"
 ${If} ${Errors}
  SetErrorLevel 2
  Abort "无法写入暂存目录，旧插件未更改。"
 ${EndIf}
 ClearErrors
 WriteINIStr "$StageDir\install.ini" "Install" "ClientRoot" "$ClientRoot"
 WriteINIStr "$StageDir\install.ini" "Install" "ProfileRoot" "$INSTDIR"
 WriteINIStr "$StageDir\install.ini" "Install" "Backup" "$BackupDir"
 WriteINIStr "$StageDir\install.ini" "Install" "Edition" "${EDITION}"
 WriteUninstaller "$StageDir\uninstall.exe"
 ${If} ${Errors}
  SetErrorLevel 2
  Abort "无法暂存卸载记录，旧插件未更改。"
 ${EndIf}
 CreateDirectory "$INSTDIR\plugins"
 CreateDirectory "$BackupDir"
 !include "backup-files.nsh"
 ClearErrors
 Rename "$StageDir\ts3localaudio.dll" "$INSTDIR\plugins\ts3localaudio.dll"
 Rename "$StageDir\ts3localaudio" "$INSTDIR\plugins\ts3localaudio"
 ${If} ${Errors}
  !include "remove-files.nsh"
  !include "restore-files.nsh"
  SetErrorLevel 2
  Abort "安装失败，已尝试恢复旧插件；请查看备份目录。"
 ${EndIf}
 SetOutPath "$INSTDIR\LocalAudio-Plugin"
 ClearErrors
 CopyFiles /SILENT "$StageDir\install.ini" "$INSTDIR\LocalAudio-Plugin\install.ini"
 CopyFiles /SILENT "$StageDir\uninstall.exe" "$INSTDIR\LocalAudio-Plugin\uninstall.exe"
 ${If} ${Errors}
  !include "remove-files.nsh"
  !include "restore-files.nsh"
  SetErrorLevel 2
  Abort "无法写入卸载记录，已尝试恢复插件。"
 ${EndIf}
 Delete "$StageDir\install.ini"
 Delete "$StageDir\uninstall.exe"
 RMDir "$StageDir"
 WriteRegStr HKCU "${REGKEY}" "DisplayName" "LocalAudio ${EDITION} · Design by ningq"
 ReadRegStr $0 HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalAudio-Plugin-2.4" "InstallLocation"
 ${If} $0 == "$INSTDIR\LocalAudio-Plugin"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\LocalAudio-Plugin-2.4"
 ${EndIf}
 WriteRegStr HKCU "${REGKEY}" "DisplayVersion" "2.5.2"
 WriteRegStr HKCU "${REGKEY}" "Publisher" "ningq"
 WriteRegStr HKCU "${REGKEY}" "InstallLocation" "$INSTDIR\LocalAudio-Plugin"
 WriteRegStr HKCU "${REGKEY}" "UninstallString" '$\"$INSTDIR\LocalAudio-Plugin\uninstall.exe$\"'
 WriteRegDWORD HKCU "${REGKEY}" "NoModify" 1
 WriteRegDWORD HKCU "${REGKEY}" "NoRepair" 1
 DetailPrint "已安装独立插件。旧文件备份：$BackupDir"
SectionEnd
Function un.onInit
 ; NSIS sets INSTDIR to the directory containing the uninstaller.
 ReadINIStr $ClientRoot "$INSTDIR\install.ini" "Install" "ClientRoot"
 ${GetParent} "$INSTDIR" $0
 ReadINIStr $1 "$INSTDIR\install.ini" "Install" "ProfileRoot"
 ${If} $0 != $1
  SetErrorLevel 2
  Quit
 ${EndIf}
 StrCpy $INSTDIR $0
FunctionEnd
Section "Uninstall"
 Call un.LockCheck
 ClearErrors
 !include "remove-files.nsh"
 !include "verify-removed.nsh"
 Delete "$INSTDIR\LocalAudio-Plugin\install.ini"
 Delete "$INSTDIR\LocalAudio-Plugin\uninstall.exe"
 ReadRegStr $0 HKCU "${REGKEY}" "InstallLocation"
 ${If} $0 == "$INSTDIR\LocalAudio-Plugin"
  DeleteRegKey HKCU "${REGKEY}"
 ${EndIf}
 RMDir "$INSTDIR\LocalAudio-Plugin"
 DetailPrint "插件已移除。TS 身份、书签和语音设置已保留；历史插件备份未删除。"
SectionEnd
