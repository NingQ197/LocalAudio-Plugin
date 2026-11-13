"""Synthetic packaging boundary regressions; never uses real server settings."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

SPEC=importlib.util.spec_from_file_location('installer_builder',Path(__file__).resolve().parents[1]/'tools/build_installer.py')
builder=importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(builder)

def digest(data):return hashlib.sha256(data).hexdigest()

class BundleTests(unittest.TestCase):
 def setUp(self):
  self.temp=tempfile.TemporaryDirectory(prefix='localaudio-bundle-')
  self.root=Path(self.temp.name);self.project=self.root/'project';self.bundle=self.root/'bundle';self.junctions=[]
  (self.project/'third_party/licenses').mkdir(parents=True)
  (self.project/'CMakeLists.txt').write_text('project(LocalAudio VERSION 1.2.3 LANGUAGES C CXX)\n',encoding='utf-8')
  loader=self.project/'third_party/webview2/sdk/build/native/x64/WebView2Loader.dll'
  loader.parent.mkdir(parents=True);loader.write_bytes(b'synthetic-loader')
  for name in builder.LICENSE_NAMES:(self.project/'third_party/licenses'/name).write_bytes(('license:'+name).encode())
  self.make_bundle()

 def tearDown(self):
  for path in self.junctions:
   if path.exists():path.rmdir()
  self.temp.cleanup()

 def make_bundle(self,edition='audio',encrypted=True):
  if self.bundle.exists():shutil.rmtree(self.bundle)
  (self.bundle/'ui').mkdir(parents=True);(self.bundle/'denoise').mkdir()
  self.info={'version':'1.2.3','variant':edition,'files':[],'runtimeFiles':[],'loaderSha256':digest(b'synthetic-loader')}
  names=builder.UI_NAMES+(builder.SCREEN_UI_NAMES if edition=='screen' else ())
  for name in names:
   plain=('synthetic-ui:'+name).encode();stored=b'synthetic-encrypted:'+plain if encrypted else plain
   relative='ui/'+name+('.enc' if encrypted else '')
   (self.bundle/relative).write_bytes(stored)
   self.info['files'].append({'path':relative,'url':'https://ts3share.local/'+name,'storedSha256':digest(stored),'plainSha256':digest(plain)})
  for name in builder.RUNTIME_NAMES:
   stored=('runtime:'+name).encode();relative='denoise/'+name;(self.bundle/relative).write_bytes(stored)
   self.info['runtimeFiles'].append({'path':relative,'storedSha256':digest(stored),'protected':True})
  for name in builder.LICENSE_NAMES:shutil.copyfile(self.project/'third_party/licenses'/name,self.bundle/'denoise'/name)
  self.save()

 def save(self):
  (self.bundle/'release-info.json').write_text(json.dumps(self.info),encoding='utf-8')

 def validate(self,edition='audio'):
  return builder.validate_bundle(edition,self.bundle,self.project)

 def rejected(self,edition='audio'):
  with self.assertRaises((RuntimeError,OSError,ValueError)):self.validate(edition)

 def test_audio_and_screen_plain_and_encrypted(self):
  for edition in ('audio','screen'):
   for encrypted in (False,True):
    with self.subTest(edition=edition,encrypted=encrypted):
     self.make_bundle(edition,encrypted);files=self.validate(edition)
     self.assertEqual(len(files),len(builder.LICENSE_NAMES)+len(builder.RUNTIME_NAMES)+(3 if edition=='audio' else 6))

 def test_screen_bundle_cannot_be_packaged_as_audio(self):
  self.make_bundle('screen');self.rejected('audio')

 def test_audio_bundle_cannot_be_packaged_as_screen(self):self.rejected('screen')

 def test_stale_version(self):
  self.info['version']='1.2.2';self.save();self.rejected()

 def test_stale_installer_version(self):
  self.info['installerVersion']='1.2.2';self.save();self.rejected()

 def test_feature_declaration_mismatch(self):
  self.info['screenSharing']=True;self.save();self.rejected()

 def test_unknown_edition(self):self.rejected('unknown')

 def test_extra_screen_resource_in_audio(self):
  (self.bundle/'ui/rtc.js.enc').write_bytes(b'old-screen-code');self.rejected()

 def test_undeclared_runtime(self):
  (self.bundle/'denoise/unexpected.dll').write_bytes(b'unexpected');self.rejected()

 def test_missing_ui(self):
  (self.bundle/self.info['files'][0]['path']).unlink();self.rejected()

 def test_ui_bytes_tampered(self):
  (self.bundle/self.info['files'][0]['path']).write_bytes(b'tampered');self.rejected()

 def test_runtime_bytes_tampered(self):
  (self.bundle/self.info['runtimeFiles'][0]['path']).write_bytes(b'tampered');self.rejected()

 def test_runtime_no_longer_protected(self):
  self.info['runtimeFiles'][0]['protected']=False;self.save();self.rejected()

 def test_runtime_path_traversal(self):
  self.info['runtimeFiles'][0]['path']='denoise/../unexpected.dll';self.save();self.rejected()

 def test_missing_license(self):
  (self.bundle/'denoise'/builder.LICENSE_NAMES[0]).unlink();self.rejected()

 def test_altered_license(self):
  (self.bundle/'denoise'/builder.LICENSE_NAMES[0]).write_bytes(b'wrong-license');self.rejected()

 def test_unexpected_project_license(self):
  (self.project/'third_party/licenses/personal.txt').write_bytes(b'private');self.rejected()

 def test_loader_hash_mismatch(self):
  self.info['loaderSha256']='0'*64;self.save();self.rejected()

 def test_duplicate_ui_declaration(self):
  self.info['files'][1]=dict(self.info['files'][0]);self.save();self.rejected()

 def test_duplicate_runtime_declaration(self):
  self.info['runtimeFiles'][1]=dict(self.info['runtimeFiles'][0]);self.save();self.rejected()

 def test_ui_path_traversal(self):
  self.info['files'][0]['path']='ui/../index.html.enc';self.save();self.rejected()

 def test_ui_url_mismatch(self):
  self.info['files'][0]['url']='https://example.invalid/index.html';self.save();self.rejected()

 def test_invalid_plain_hash(self):
  self.info['files'][0]['plainSha256']='invalid';self.save();self.rejected()

 def test_plain_payload_must_match_plain_hash(self):
  self.make_bundle(encrypted=False);self.info['files'][0]['plainSha256']='0'*64;self.save();self.rejected()

 def test_mixed_encryption_modes(self):
  first=self.info['files'][0];source=self.bundle/first['path'];target=source.with_suffix('');source.rename(target)
  first['path']=first['path'].removesuffix('.enc');self.save();self.rejected()

 def test_nested_directory_in_ui(self):
  (self.bundle/'ui/extra').mkdir();self.rejected()

 def test_expected_file_replaced_by_directory(self):
  path=self.bundle/self.info['files'][0]['path'];path.unlink();path.mkdir();self.rejected()

 def test_duplicate_json_fields(self):
  path=self.bundle/'release-info.json';path.write_text('{"variant":"screen",'+path.read_text(encoding='utf-8')[1:],encoding='utf-8');self.rejected()

 def test_oversized_manifest(self):
  (self.bundle/'release-info.json').write_bytes(b' '*262145);self.rejected()

 def test_ui_symlink_is_rejected(self):
  path=self.bundle/self.info['files'][0]['path'];target=self.root/'external-ui';target.write_bytes(path.read_bytes());path.unlink()
  try:path.symlink_to(target)
  except OSError as error:self.skipTest('File symlinks are unavailable: '+str(error.winerror if hasattr(error,'winerror') else error.errno))
  self.rejected()

 @unittest.skipUnless(os.name=='nt','Windows junction test')
 def test_ui_junction_is_rejected(self):
  path=self.bundle/'ui';target=self.root/'outside-ui';path.rename(target)
  subprocess.run(['cmd','/c','mklink','/J',str(path),str(target)],check=True,capture_output=True)
  self.junctions.append(path);self.rejected()

 @unittest.skipUnless(os.name=='nt','Windows junction test')
 def test_ancestor_junction_is_rejected(self):
  path=self.root/'alias';subprocess.run(['cmd','/c','mklink','/J',str(path),str(self.bundle)],check=True,capture_output=True)
  self.junctions.append(path)
  with self.assertRaises(RuntimeError):builder.validate_bundle('audio',path,self.project)

 def test_copies_only_validated_payload(self):
  (self.bundle/'include').mkdir();(self.bundle/'include/private-key.h').write_bytes(b'synthetic-not-for-installation')
  (self.bundle/'ts3localaudio.dll').write_bytes(b'not-the-explicit-build-output')
  destination=self.root/'stage';expected=self.validate()
  builder.copy_bundle('audio',self.bundle,destination,self.project)
  actual={p.relative_to(destination).as_posix():digest(p.read_bytes()) for p in destination.rglob('*') if p.is_file()}
  self.assertEqual(actual,expected)

 def test_copy_detects_changed_source(self):
  destination=self.root/'stage';real_copy=shutil.copyfile
  def changed_copy(source,target):
   result=real_copy(source,target);Path(target).write_bytes(b'changed-during-copy');return result
  with mock.patch.object(builder.shutil,'copyfile',side_effect=changed_copy):
   with self.assertRaisesRegex(RuntimeError,'changed while staging'):builder.copy_bundle('audio',self.bundle,destination,self.project)

if __name__=='__main__':unittest.main()
