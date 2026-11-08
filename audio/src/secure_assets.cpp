#include "secure_assets.h"
#include "common.h"
#include <bcrypt.h>
#include <array>
#include <cstring>
#include <limits>
#include "release_assets_manifest.h"

namespace la {
namespace {
struct FileHandle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~FileHandle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
struct Algorithm {
    BCRYPT_ALG_HANDLE value=nullptr;
    ~Algorithm(){if(value)BCryptCloseAlgorithmProvider(value,0);}
};
struct HashHandle {
    BCRYPT_HASH_HANDLE value=nullptr;
    ~HashHandle(){if(value)BCryptDestroyHash(value);}
};
struct KeyHandle {
    BCRYPT_KEY_HANDLE value=nullptr;
    ~KeyHandle(){if(value)BCryptDestroyKey(value);}
};
bool ReadRegular(HANDLE file,std::vector<unsigned char>& data) {
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    if(file==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(file,&info) ||
       (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) ||
       !GetFileSizeEx(file,&size) || size.QuadPart<1 || size.QuadPart>4*1024*1024)return false;
    data.resize(static_cast<size_t>(size.QuadPart));
    DWORD got=0;
    return ReadFile(file,data.data(),static_cast<DWORD>(data.size()),&got,nullptr)&&got==data.size();
}
bool DigestMatches(const std::vector<unsigned char>& bytes,const unsigned char* expected) {
    Algorithm alg;std::vector<unsigned char> object;HashHandle hash;
    if(BCryptOpenAlgorithmProvider(&alg.value,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    DWORD objectSize=0,returned=0;
    if(BCryptGetProperty(alg.value,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&returned,0)<0)return false;
    object.resize(objectSize);
    if(BCryptCreateHash(alg.value,&hash.value,object.data(),objectSize,nullptr,0,0)<0 ||
       BCryptHashData(hash.value,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0)<0)return false;
    std::array<unsigned char,32> digest{};
    if(BCryptFinishHash(hash.value,digest.data(),static_cast<ULONG>(digest.size()),0)<0)return false;
    unsigned int different=0;for(size_t i=0;i<digest.size();++i)different|=digest[i]^expected[i];
    return different==0;
}
bool Decrypt(const std::vector<unsigned char>& encoded,const char* aad,std::vector<unsigned char>& plain) {
    if(encoded.size()<29)return false;
    Algorithm alg;std::vector<unsigned char> object;KeyHandle key;
    if(BCryptOpenAlgorithmProvider(&alg.value,BCRYPT_AES_ALGORITHM,nullptr,0)<0)return false;
    if(BCryptSetProperty(alg.value,BCRYPT_CHAINING_MODE,reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),sizeof(BCRYPT_CHAIN_MODE_GCM),0)<0)return false;
    DWORD objectSize=0,returned=0;
    if(BCryptGetProperty(alg.value,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&returned,0)<0)return false;
    object.resize(objectSize);
    if(BCryptGenerateSymmetricKey(alg.value,&key.value,object.data(),objectSize,const_cast<PUCHAR>(kReleaseResourceKey),sizeof(kReleaseResourceKey),0)<0)return false;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth;
    BCRYPT_INIT_AUTH_MODE_INFO(auth);
    auth.pbNonce=const_cast<PUCHAR>(encoded.data());auth.cbNonce=12;
    auth.pbTag=const_cast<PUCHAR>(encoded.data()+encoded.size()-16);auth.cbTag=16;
    auth.pbAuthData=reinterpret_cast<PUCHAR>(const_cast<char*>(aad));auth.cbAuthData=static_cast<ULONG>(strlen(aad));
    const ULONG cipherSize=static_cast<ULONG>(encoded.size()-28);
    plain.resize(cipherSize);ULONG length=0;
    if(BCryptDecrypt(key.value,const_cast<PUCHAR>(encoded.data()+12),cipherSize,&auth,nullptr,0,plain.data(),cipherSize,&length,0)<0){SecureZeroMemory(plain.data(),plain.size());plain.clear();return false;}
    plain.resize(length);return true;
}
}
std::shared_ptr<const ShareAssetBundle> LoadVerifiedShareAssets(const std::wstring& root) {
    auto bundle=std::make_shared<ShareAssetBundle>();
    for(const auto& entry:kReleaseAssets) {
        FileHandle file;
        file.value=CreateFileW((root+L"\\"+entry.disk).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        std::vector<unsigned char> bytes,plain;
        if(!ReadRegular(file.value,bytes) || !DigestMatches(bytes,entry.storedHash)) {
            Log("LocalAudio resource verification failed; reinstall the official package.");return {};
        }
        if(entry.encrypted) {
            if(!Decrypt(bytes,entry.aad,plain)){Log("LocalAudio resource authentication failed.");return {};}
        }else plain=std::move(bytes);
        if(!DigestMatches(plain,entry.plainHash)){Log("LocalAudio resource content verification failed.");return {};}
        bundle->emplace(entry.url,ShareAsset{std::move(plain),entry.mime});
    }
    return bundle;
}
HANDLE OpenVerifiedWebViewLoader(const std::wstring& path) {
    FileHandle file;
    file.value=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    std::vector<unsigned char> bytes;
    if(!ReadRegular(file.value,bytes)||!DigestMatches(bytes,kReleaseLoaderHash)){Log("WebView2 loader verification failed.");return INVALID_HANDLE_VALUE;}
    HANDLE result=file.value;file.value=INVALID_HANDLE_VALUE;return result;
}
const char* ShareResourceProtection(){return kReleaseProtection;}
}
