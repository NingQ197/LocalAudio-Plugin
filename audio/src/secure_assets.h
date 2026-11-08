#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

namespace la {
struct ShareAsset {
    std::vector<unsigned char> bytes;
    std::wstring mime;
};
using ShareAssetBundle = std::map<std::wstring, ShareAsset>;
// A release-generated manifest is compiled into the native DLL. All six UI
// resources are checked before any JavaScript executes, then served from memory.
std::shared_ptr<const ShareAssetBundle> LoadVerifiedShareAssets(const std::wstring& root);
// Keep this read handle alive until the browser loader has been loaded. It denies
// writes/deletion between the hash check and LoadLibraryEx.
HANDLE OpenVerifiedWebViewLoader(const std::wstring& path);
const char* ShareResourceProtection();
}
