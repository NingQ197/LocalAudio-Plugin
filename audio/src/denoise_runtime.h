#pragma once
#include <string>
namespace la {
bool VerifyDenoiseRuntime(const std::wstring& root);
bool OpenDenoiseUi(const std::wstring& root,const std::wstring& configFile);
void ShowDenoiseTutorial();
void CloseDenoiseUi();
void ReleaseDenoiseRuntime();
}
