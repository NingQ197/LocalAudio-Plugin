"""Build an independent installer from a locally built plugin."""
from pathlib import Path
import argparse,hashlib,json,shutil,subprocess
R=Path(__file__).resolve().parent.parent
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser()
 for name in ['edition','bundle','dll','helper','webview-installer','vc-installer','output','makensis']:
  p.add_argument('--'+name,required=True,type=Path if name!='edition' else str)
 a=p.parse_args();assert a.edition in ['audio','screen']
 expected=[(a.webview_installer,'9b0142e22e8daa92855d746ae4d1e9eeef561ccaf67d1dd452165c4af3df3191'),(a.vc_installer,'843068991daaa1f73ad9f6239bce4d0f6a07a51f18c37ea2a867e9beca71295c')]
 for f,h in expected:
  if sha(f)!=h:raise RuntimeError('Pinned Microsoft redistributable hash mismatch')
 stage=R/'build'/('installer-'+a.edition)
 if stage.exists():raise RuntimeError('Fresh installer stage required')
 res=stage/'package/plugins/ts3localaudio';res.mkdir(parents=True)
 for folder in ['ui','denoise']:shutil.copytree(a.bundle/folder,res/folder)
 shutil.copyfile(a.dll,stage/'package/plugins/ts3localaudio.dll')
 shutil.copyfile(R/'third_party/webview2/sdk/build/native/x64/WebView2Loader.dll',res/'WebView2Loader.dll')
 source=res/'source';source.mkdir()
 for name in ['denoise_qt_ui.cpp','denoise_ui_api.h']:shutil.copyfile(R/'audio/src'/name,source/name)
 shutil.copytree(R/'third_party/licenses',res/'licenses')
 shutil.copyfile(R/'LICENSE',res/'licenses/LocalAudio-MIT.txt')
 shutil.copyfile(R/'THIRD_PARTY_NOTICES.txt',res/'licenses/LocalAudio-ThirdPartyNotices.txt')
 (res/'README.txt').write_text('LocalAudio independent TS3 plugin. Design by ningq. Requires official TeamSpeak 3.6.2 x64, Windows 10 21H2 or newer.\n',encoding='utf-8')
 for name in ['installer.nsi','windows-version.nsh']:shutil.copyfile(R/'installer'/name,stage/name)
 owned={f.relative_to(stage/'package/plugins').as_posix() for f in (stage/'package/plugins').rglob('*') if f.is_file()}
 for name in ['index.html','styles.css','app.js','rtc.js','pcm.js','pcm-worklet.js']:
  owned|={'ts3localaudio/ui/'+name,'ts3localaudio/ui/'+name+'.enc'}
 owned.add('ts3localaudio_win64.dll')
 # Previous release-owned documentation is also removed during uninstall.
 owned|={'ts3localaudio/README-zh-CN.txt','ts3localaudio/release-manifest.json','ts3localaudio/source/BUILD.txt'}
 dirs=sorted({Path(f).parent.as_posix() for f in owned if '/' in f},key=lambda x:(-x.count('/'),x))
 remove='\n'.join('Delete "$INSTDIR\\plugins\\'+f.replace('/','\\')+'"' for f in sorted(owned))+'\n'+'\n'.join('RMDir "$INSTDIR\\plugins\\'+d.replace('/','\\')+'"' for d in dirs)+'\n'
 (stage/'remove-files.nsh').write_text(remove,encoding='utf-8')
 verify=''
 for f in sorted(owned):verify+='${If} ${FileExists} "$INSTDIR\\plugins\\'+f.replace('/','\\')+'"\n SetErrorLevel 2\n Abort "Plugin files remain in use; uninstall entrypoint retained."\n${EndIf}\n'
 (stage/'verify-removed.nsh').write_text(verify,encoding='utf-8')
 backup='CreateDirectory "$BackupDir\\installer"\n';restore=''
 for name in ['install.ini','uninstall.exe']:
  backup+='${If} ${FileExists} "$INSTDIR\\LocalAudio-Plugin\\'+name+'"\n ClearErrors\n CopyFiles /SILENT "$INSTDIR\\LocalAudio-Plugin\\'+name+'" "$BackupDir\\installer\\'+name+'"\n ${If} ${Errors}\n SetErrorLevel 2\n Abort "Cannot back up installer records."\n ${EndIf}\n${EndIf}\n'
  restore+='Delete "$INSTDIR\\LocalAudio-Plugin\\'+name+'"\n${If} ${FileExists} "$BackupDir\\installer\\'+name+'"\n CopyFiles /SILENT "$BackupDir\\installer\\'+name+'" "$INSTDIR\\LocalAudio-Plugin\\'+name+'"\n${EndIf}\n'
 for name in ['ts3localaudio.dll','ts3localaudio_win64.dll','ts3localaudio']:
  backup+='${If} ${FileExists} "$INSTDIR\\plugins\\'+name+'"\n ClearErrors\n Rename "$INSTDIR\\plugins\\'+name+'" "$BackupDir\\'+name+'"\n ${If} ${Errors}\n !include "restore-files.nsh"\n SetErrorLevel 2\n Abort "Cannot back up previous plugin."\n ${EndIf}\n${EndIf}\n'
  restore='${If} ${FileExists} "$BackupDir\\'+name+'"\n Rename "$BackupDir\\'+name+'" "$INSTDIR\\plugins\\'+name+'"\n${EndIf}\n'+restore
 (stage/'backup-files.nsh').write_text(backup,encoding='utf-8');(stage/'restore-files.nsh').write_text(restore,encoding='utf-8')
 def path(x):return str(x.resolve()).replace('/','\\')
 label='音频版' if a.edition=='audio' else '屏幕版'
 args=[str(a.makensis),'/V3','/INPUTCHARSET','UTF8','/DOUTPUT_FILE='+path(a.output),'/DPACKAGE_FILES='+path(stage/'package'),'/DCLIENT_CHECK='+path(a.helper),'/DWEBVIEW_PAYLOAD='+path(a.webview_installer),'/DVCREDIST_PAYLOAD='+path(a.vc_installer),'/DEDITION='+label,'/DEDITION_DESCRIPTION=LocalAudio independent plugin',str(stage/'installer.nsi')]
 subprocess.run(args,check=True)
 print('Built',a.output.name,'SHA256',sha(a.output))
if __name__=='__main__':main()
