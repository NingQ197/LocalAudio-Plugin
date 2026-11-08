#pragma once
#include <string>
namespace la {
bool VerifyShareWindowInstallation();
// Returns false if runtime/assets are unavailable; caller opens the native fallback.
bool ShowShareWindow();
void CloseShareWindow();
void ShareWindowPostJson(std::string json);
void SetShareWindowConfigDirectory(const std::wstring& directory);
#ifdef LA_SHARE_TEST
bool ShareWindowExecuteScriptForTest(const std::wstring& script);
#endif
}
