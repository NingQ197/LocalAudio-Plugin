"""Fetch pinned public upstream build inputs; never downloads a TS client."""
from pathlib import Path
import hashlib,io,shutil,tarfile,urllib.request,zipfile
R=Path(__file__).resolve().parent.parent
CACHE=R/'deps/downloads';CACHE.mkdir(parents=True,exist_ok=True)
def fetch(url,name,expected):
    p=CACHE/name
    if not p.exists():
        with urllib.request.urlopen(url,timeout=120) as response:p.write_bytes(response.read())
    if hashlib.sha256(p.read_bytes()).hexdigest()!=expected:raise RuntimeError('Upstream hash mismatch: '+name)
    return p
def copy_bytes(dest,data):
    dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
def main():
    archive=fetch('https://codeload.github.com/xiph/rnnoise/zip/refs/tags/v0.2','rnnoise-v0.2.zip','4ab6117f45e1101c4044c6d301f600afb15ebaa9a3819d81c1eccd20de95d5da')
    with zipfile.ZipFile(archive) as z:
        for n in z.namelist():
            rel=Path(n).parts[1:]
            if not rel or n.endswith('/') or '..' in rel:continue
            if rel[0] in ['src','include'] or rel==('COPYING',):copy_bytes(R/'third_party/rnnoise'/Path(*rel),z.read(n))
    model=fetch('https://media.xiph.org/rnnoise/models/rnnoise_data-0b50c45.tar.gz','rnnoise-models.tar.gz','4ac81c5c0884ec4bd5907026aaae16209b7b76cd9d7f71af582094a2f98f4b43')
    with tarfile.open(model) as t:
        for member in t.getmembers():
            if member.isfile() and Path(member.name).name in ['rnnoise_data.c','rnnoise_data.h','rnnoise_data_little.c']:
                copy_bytes(R/'third_party/rnnoise/src'/Path(member.name).name,t.extractfile(member).read())
    header=fetch('https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp','json.hpp','aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63')
    copy_bytes(R/'third_party/json/json.hpp',header.read_bytes())
    sdk=fetch('https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4314.35/microsoft.web.webview2.1.0.4314.35.nupkg','webview2.nupkg','90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4')
    with zipfile.ZipFile(sdk) as z:
        for n in z.namelist():
            if n in ['LICENSE.txt','build/native/include/WebView2.h','build/native/x64/WebView2Loader.dll']:
                copy_bytes(R/'third_party/webview2/sdk'/n,z.read(n))
    runtime=fetch('https://github.com/k2-fsa/sherpa-onnx/releases/download/v2.0.1/sherpa-onnx-v2.0.1-win-x64-shared-MD-Release-no-tts-lib.tar.bz2','sherpa.tar.bz2','3aea6fc2360ac67b349f61b5cbe237f34b2bb53dc805aa4b3df7ab23ba7ab35c')
    wanted={'sherpa-onnx-c-api.dll','onnxruntime.dll','onnxruntime_providers_shared.dll'}
    with tarfile.open(runtime) as t:
        for member in t.getmembers():
            if member.isfile() and Path(member.name).name in wanted:copy_bytes(R/'deps/runtime'/Path(member.name).name,t.extractfile(member).read())
    model=fetch('https://github.com/k2-fsa/sherpa-onnx/releases/download/speech-enhancement-models/gtcrn_simple.onnx','gtcrn_simple.onnx','e77603ac0c23dac3227dd2d7135b3a585cbee2679048aecfa886657d3ae1b534')
    copy_bytes(R/'deps/runtime/gtcrn_simple.onnx',model.read_bytes())
    assert all((R/'deps/runtime'/n).is_file() for n in wanted)
    print('PASS pinned dependencies prepared; build the native Qt bridge separately')
if __name__=='__main__':main()
