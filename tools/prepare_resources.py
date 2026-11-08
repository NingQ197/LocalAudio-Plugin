"""Generate a NEW local resource manifest. Generated keys must never be committed."""
from pathlib import Path
import argparse,hashlib,json,secrets,shutil
R=Path(__file__).resolve().parent.parent
def cb(b):return '{'+','.join(f'0x{x:02x}' for x in b)+'}'
def digest(b):return hashlib.sha256(b).digest()
def main():
    p=argparse.ArgumentParser();p.add_argument('edition',choices=['audio','screen']);p.add_argument('--runtime',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--encrypt',action='store_true');a=p.parse_args()
    out=a.output.resolve()
    if out.exists():raise RuntimeError('Use a new output directory to avoid mixed resources')
    (out/'include').mkdir(parents=True);(out/'ui').mkdir();(out/'denoise').mkdir()
    key=secrets.token_bytes(32) if a.encrypt else bytes(32);rows=[];entries=[]
    if a.encrypt:from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    names=['index.html','styles.css','app.js']+(['rtc.js','pcm.js','pcm-worklet.js'] if a.edition=='screen' else [])
    for name in names:
        b=(R/a.edition/'frontend'/name).read_bytes();aad=f'TS3LocalAudio/2.5.4/ui/{name}';stored=b
        if a.encrypt:
            nonce=secrets.token_bytes(12);stored=nonce+AESGCM(key).encrypt(nonce,b,aad.encode())
        disk='ui/'+name+('.enc' if a.encrypt else '');(out/disk).write_bytes(stored)
        mime='text/html; charset=utf-8' if name.endswith('.html') else 'text/css; charset=utf-8' if name.endswith('.css') else 'application/javascript; charset=utf-8'
        url='https://ts3share.local/'+name
        rows.append('{L"'+disk+'",L"'+url+'",L"'+mime+'","'+aad+'",'+str(a.encrypt).lower()+','+cb(digest(stored))+','+cb(digest(b))+'}')
        entries.append(dict(path=disk,url=url,plainSha256=digest(b).hex(),storedSha256=digest(stored).hex()))
    loader=(R/'third_party/webview2/sdk/build/native/x64/WebView2Loader.dll').read_bytes()
    header='#pragma once\nstruct ReleaseAssetEntry {const wchar_t* disk;const wchar_t* url;const wchar_t* mime;const char* aad;bool encrypted;unsigned char storedHash[32];unsigned char plainHash[32];};\n'
    header+='static const unsigned char kReleaseResourceKey[32]='+cb(key)+';\nstatic const unsigned char kReleaseLoaderHash[32]='+cb(digest(loader))+';\nstatic constexpr const char* kReleaseProtection="'+('encrypted' if a.encrypt else 'plain')+'";\nstatic const ReleaseAssetEntry kReleaseAssets[]={\n'+',\n'.join(rows)+'\n};\n'
    (out/'include/release_assets_manifest.h').write_text(header,encoding='utf-8')
    rows=[];runtime=[]
    for name in ['ningq_denoise_ui.dll','sherpa-onnx-c-api.dll','onnxruntime.dll','onnxruntime_providers_shared.dll','gtcrn_simple.onnx']:
        b=(a.runtime/name).read_bytes();(out/'denoise'/name).write_bytes(b);h=digest(b)
        rows.append('{L"'+name+'",'+cb(h)+'}');runtime.append(dict(path='denoise/'+name,storedSha256=h.hex(),protected=True))
    for f in (R/'third_party/licenses').glob('*.txt'):shutil.copyfile(f,out/'denoise'/f.name)
    (out/'include/release_denoise_manifest.h').write_text('#pragma once\nstruct DenoiseRuntimeEntry {const wchar_t* path;unsigned char sha256[32];};\nstatic const DenoiseRuntimeEntry kDenoiseRuntimeFiles[]={\n'+',\n'.join(rows)+'\n};\n',encoding='utf-8')
    (out/'release-info.json').write_text(json.dumps(dict(version='2.5.4',variant=a.edition,files=entries,runtimeFiles=runtime,loaderSha256=digest(loader).hex()),indent=2),encoding='utf-8')
    print('Generated resource bundle:', out.name)
if __name__=='__main__':main()
