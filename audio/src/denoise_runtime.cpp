#include "denoise_runtime.h"
#include "denoise_engine.h"
#include "denoise_ui_api.h"
#include "share_window.h"
#include "config_dialog.h"
#include "settings.h"
#include "common.h"
#include "release_denoise_manifest.h"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <vector>

namespace la {
namespace {
std::vector<HANDLE> leases;
HMODULE ui=nullptr;
NingqNoiseUiStopFn stopUi=nullptr;
using TutorialStartFn=int(*)(const wchar_t*,const wchar_t*,void(*)());
using TutorialShowFn=void(*)();
TutorialShowFn showTutorial=nullptr;
void OpenMusicFromTutorial(){if(!ShowShareWindow())ShowConfigDialog();}
std::wstring settingsFile;
void ReadUi(int* model,int* strength){const auto s=GetDenoiseConfig();*model=s.model;*strength=s.strength;}
int WriteUi(int model,int strength){
    if(model<0||model>2||strength<0||strength>100)return 0;
    if(!SaveDenoiseSettings(settingsFile,model,strength))return 0;
    return SetDenoiseConfig(model,strength)?1:0;
}
void StatusUi(wchar_t* text,int capacity){if(!text||capacity<1)return;const auto s=DenoiseStatus();const auto n=std::min(s.size(),static_cast<size_t>(capacity-1));std::copy_n(s.data(),n,text);text[n]=0;}
bool VerifyFile(HANDLE file,const unsigned char* expected){
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER length{};
    if(!GetFileInformationByHandle(file,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||!GetFileSizeEx(file,&length)||length.QuadPart<1||length.QuadPart>64*1024*1024)return false;
    BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    bool ok=false;
    if(BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)>=0){
        std::array<unsigned char,65536> buffer{};DWORD got=0;bool valid=true;
        for(;;){if(!ReadFile(file,buffer.data(),static_cast<DWORD>(buffer.size()),&got,nullptr)){valid=false;break;}if(!got)break;if(BCryptHashData(hash,buffer.data(),got,0)<0){valid=false;break;}}
        unsigned char digest[32]={};
        if(valid&&BCryptFinishHash(hash,digest,32,0)>=0){unsigned diff=0;for(int i=0;i<32;++i)diff|=digest[i]^expected[i];ok=diff==0;}
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg,0);return ok;
}
}
bool VerifyDenoiseRuntime(const std::wstring& root){
    if(!leases.empty())return true;
    for(const auto& entry:kDenoiseRuntimeFiles){
        HANDLE file=CreateFileW((root+L"\\"+entry.path).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(file==INVALID_HANDLE_VALUE||!VerifyFile(file,entry.sha256)){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);ReleaseDenoiseRuntime();Log("AI denoise integrity check failed; reinstall the official package.");return false;}
        leases.push_back(file);
    }
    return true;
}
bool OpenDenoiseUi(const std::wstring& root,const std::wstring& configFile){
    if(ui)return true;settingsFile=configFile;
    ui=LoadLibraryExW((root+L"\\ningq_denoise_ui.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!ui)return false;
    auto start=reinterpret_cast<NingqNoiseUiStartFn>(GetProcAddress(ui,"NingqNoiseUiStart"));
    stopUi=reinterpret_cast<NingqNoiseUiStopFn>(GetProcAddress(ui,"NingqNoiseUiStop"));
    const NingqNoiseUiCallbacks callbacks{sizeof(NingqNoiseUiCallbacks),ReadUi,WriteUi,StatusUi};
    if(!start||!stopUi||!start(&callbacks)){stopUi=nullptr;FreeLibrary(ui);ui=nullptr;return false;}
    auto startTutorial=reinterpret_cast<TutorialStartFn>(GetProcAddress(ui,"NingqTutorialStart"));
    showTutorial=reinterpret_cast<TutorialShowFn>(GetProcAddress(ui,"NingqTutorialShow"));
    const auto separator=configFile.find_last_of(L"\\/");
    const std::wstring profile=separator==std::wstring::npos?L".":configFile.substr(0,separator);
    if(!startTutorial||!showTutorial||!startTutorial(profile.c_str(),nullptr,OpenMusicFromTutorial)){
        showTutorial=nullptr;Log("First-run tutorial unavailable; AI denoise remains available.");
    }
    return true;
}
void ShowDenoiseTutorial(){if(showTutorial)showTutorial();}
void CloseDenoiseUi(){if(ui){if(stopUi)stopUi();stopUi=nullptr;showTutorial=nullptr;FreeLibrary(ui);ui=nullptr;}}
void ReleaseDenoiseRuntime(){for(HANDLE file:leases)CloseHandle(file);leases.clear();}
}
