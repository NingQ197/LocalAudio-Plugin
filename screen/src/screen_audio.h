#ifndef LA_SCREEN_AUDIO_H
#define LA_SCREEN_AUDIO_H
#include <functional>
#include <string>

namespace la {
using ScreenPcmEmitter = std::function<void(std::string)>;
// A separate digital screen audio stream. It NEVER modifies the microphone.
// true = all playback EXCEPT this TS process tree (including WebView children).
// false = only the selected non-TS application's tree. Callback: raw base64 s16.
bool ScreenAudioStart(bool excludeTs, const std::wstring& exe, ScreenPcmEmitter emitBase64);
void ScreenAudioStop();
bool ScreenAudioRunning();
std::wstring ScreenAudioStatus();
} // namespace la
#endif
