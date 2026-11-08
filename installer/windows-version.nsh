; Numeric Windows build policy shared by installer and isolated gate tests.
!macro LA_CheckWindowsBuild BUILD
 ${If} ${BUILD} < 19044
  MessageBox MB_ICONSTOP "需要 Windows 10 21H2（19044）或更新的 64 位 Windows。" /SD IDOK
  SetErrorLevel 2
  Quit
 ${EndIf}
!macroend
