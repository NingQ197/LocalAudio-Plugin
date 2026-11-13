"""Build an independent installer from a locally built plugin."""
from pathlib import Path
import argparse,hashlib,json,re,shutil,stat,subprocess
R=Path(__file__).resolve().parent.parent
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
UI_NAMES=('index.html','styles.css','app.js')
SCREEN_UI_NAMES=('rtc.js','pcm.js','pcm-worklet.js')
RUNTIME_NAMES=('ningq_denoise_ui.dll','sherpa-onnx-c-api.dll','onnxruntime.dll','onnxruntime_providers_shared.dll','gtcrn_simple.onnx')
LICENSE_NAMES=('GTCRN-MIT.txt','Microsoft-WebView2.txt','nlohmann-json-MIT.txt','ONNX-Runtime-MIT.txt','ONNX-Runtime-ThirdPartyNotices.txt','Qt-GPL-3.0.txt','Qt-LGPL-3.0.txt','Qt-NOTICE.txt','RNNoise-BSD-3-Clause.txt','Sherpa-ONNX-Apache-2.0.txt','WebView2-NOTICE.txt')

def checked_path(path,directory=False):
 """Reject links and Windows reparse points before reading build inputs."""
 path=Path(path).absolute()
 if '..' in path.parts:raise RuntimeError('Parent traversal is not allowed in bundle paths')
 for ancestor in (*reversed(path.parents),path):
  info=ancestor.lstat()
  if stat.S_ISLNK(info.st_mode) or getattr(info,'st_file_attributes',0)&getattr(stat,'FILE_ATTRIBUTE_REPARSE_POINT',0x400):
   raise RuntimeError('Links and reparse points are not allowed in bundle paths')
 info=path.lstat()
 if not (stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode)):
  raise RuntimeError('Expected a regular '+('directory' if directory else 'file'))
 return path

def exact_directory(path,names):
 path=checked_path(path,True)
 if {p.name for p in path.iterdir()}!=set(names):raise RuntimeError('Unexpected or missing bundle files in '+path.name)
 return {name:checked_path(path/name) for name in names}

def unique_object(pairs):
 result={}
 for key,value in pairs:
  if key in result:raise RuntimeError('Duplicate JSON field in resource manifest')
  result[key]=value
 return result

def validate_bundle(edition,bundle,project=R):
 """Validate all copied resources before creating the installer stage.

 Generated includes and an optional DLL are not copied from the bundle. Only
 the returned UI/runtime/license whitelist may enter the installed payload.
 This cannot protect a build running under an already compromised OS user.
 """
 if edition not in ('audio','screen'):raise RuntimeError('Unknown installer edition')
 bundle=checked_path(bundle,True);project=Path(project)
 manifest=checked_path(bundle/'release-info.json')
 if manifest.stat().st_size>256*1024:raise RuntimeError('Resource manifest is too large')
 info=json.loads(manifest.read_text(encoding='utf-8'),object_pairs_hook=unique_object)
 cmake=checked_path(project/'CMakeLists.txt').read_text(encoding='utf-8')
 match=re.search(r'project\(LocalAudio\s+VERSION\s+(\d+\.\d+\.\d+)\b',cmake)
 if not match or not isinstance(info,dict) or info.get('version')!=match[1] or info.get('variant')!=edition:
  raise RuntimeError('Resource bundle version or edition mismatch')
 if 'installerVersion' in info and info['installerVersion']!=match[1]:raise RuntimeError('Resource installer version mismatch')
 if 'screenSharing' in info and info['screenSharing'] is not (edition=='screen'):raise RuntimeError('Resource feature declaration mismatch')
 names=UI_NAMES+(SCREEN_UI_NAMES if edition=='screen' else ())
 entries=info.get('files')
 if not isinstance(entries,list) or len(entries)!=len(names) or not all(isinstance(e,dict) for e in entries):
  raise RuntimeError('Invalid UI resource declarations')
 paths=[e.get('path') for e in entries]
 plain={'ui/'+n for n in names};encrypted={n+'.enc' for n in plain}
 if not all(isinstance(n,str) for n in paths) or len(set(paths))!=len(paths) or set(paths) not in (plain,encrypted):
  raise RuntimeError('UI resource list does not match the selected edition')
 files=exact_directory(bundle/'ui',[Path(n).name for n in paths]);copied={}
 for entry in entries:
  name=entry['path'].split('/',1)[1];original=name.removesuffix('.enc')
  stored=entry.get('storedSha256');decoded=entry.get('plainSha256')
  if entry.get('url')!='https://ts3share.local/'+original or not isinstance(stored,str) or not re.fullmatch('[0-9a-f]{64}',stored) or not isinstance(decoded,str) or not re.fullmatch('[0-9a-f]{64}',decoded):
   raise RuntimeError('Invalid UI resource metadata')
  if sha(files[name])!=stored or (not name.endswith('.enc') and stored!=decoded):raise RuntimeError('UI resource hash mismatch')
  copied[entry['path']]=stored
 runtime=info.get('runtimeFiles')
 if not isinstance(runtime,list) or len(runtime)!=len(RUNTIME_NAMES) or not all(isinstance(e,dict) for e in runtime):
  raise RuntimeError('Invalid runtime declarations')
 runtime_paths=[e.get('path') for e in runtime]
 if not all(isinstance(n,str) for n in runtime_paths) or len(set(runtime_paths))!=len(runtime_paths) or set(runtime_paths)!={'denoise/'+n for n in RUNTIME_NAMES}:
  raise RuntimeError('Runtime list does not match the whitelist')
 denoise=exact_directory(bundle/'denoise',RUNTIME_NAMES+LICENSE_NAMES)
 for entry in runtime:
  digest=entry.get('storedSha256');name=entry['path'].split('/',1)[1]
  if entry.get('protected') is not True or not isinstance(digest,str) or not re.fullmatch('[0-9a-f]{64}',digest) or sha(denoise[name])!=digest:
   raise RuntimeError('Runtime resource hash mismatch')
  copied[entry['path']]=digest
 licenses=exact_directory(project/'third_party/licenses',LICENSE_NAMES)
 for name in LICENSE_NAMES:
  digest=sha(licenses[name])
  if sha(denoise[name])!=digest:raise RuntimeError('Runtime license mismatch')
  copied['denoise/'+name]=digest
 loader=checked_path(project/'third_party/webview2/sdk/build/native/x64/WebView2Loader.dll')
 if info.get('loaderSha256')!=sha(loader):raise RuntimeError('Resource loader hash mismatch')
 return copied

def copy_bundle(edition,bundle,destination,project=R):
 files=validate_bundle(edition,bundle,project)
 for relative,digest in files.items():
  source=checked_path(Path(bundle)/relative);target=destination/relative
  target.parent.mkdir(parents=True,exist_ok=True)
  shutil.copyfile(source,target)
  if sha(target)!=digest:raise RuntimeError('Resource changed while staging installer')

def validate_makensis(executable):
 result=subprocess.run([str(executable),'/VERSION'],check=True,capture_output=True,text=True)
 match=re.fullmatch(r'v(\d+)\.(\d+)(?:\.(\d+))?',result.stdout.strip())
 if not match or tuple(map(int,match.groups(default='0'))) < (3,13,0):
  raise RuntimeError('NSIS 3.13 or newer is required')
 return result.stdout.strip()

def main():
 p=argparse.ArgumentParser()
 for name in ['edition','bundle','dll','helper','webview-installer','vc-installer','output','makensis']:
  p.add_argument('--'+name,required=True,type=Path if name!='edition' else str)
 a=p.parse_args();assert a.edition in ['audio','screen']
 validate_bundle(a.edition,a.bundle)
 validate_makensis(a.makensis)
 expected=[(a.webview_installer,'9b0142e22e8daa92855d746ae4d1e9eeef561ccaf67d1dd452165c4af3df3191'),(a.vc_installer,'843068991daaa1f73ad9f6239bce4d0f6a07a51f18c37ea2a867e9beca71295c')]
 for f,h in expected:
  if sha(f)!=h:raise RuntimeError('Pinned Microsoft redistributable hash mismatch')
 stage=R/'build'/('installer-'+a.edition)
 if stage.exists():raise RuntimeError('Fresh installer stage required')
 res=stage/'package/plugins/ts3localaudio';res.mkdir(parents=True)
 copy_bundle(a.edition,a.bundle,res)
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
