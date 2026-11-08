#ifndef LA_PROCESS_LOOPBACK_H
#define LA_PROCESS_LOOPBACK_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <functional>
#include <cstdint>

#include "settings.h"

namespace la {

// Dedicated screen-share audio. Does not touch the TS voice mixing ring or
// microphone/PTT settings. excludeTree=true implements digital mix-minus.
using PcmSink = std::function<void(const int16_t*, size_t)>;
bool RunProcessAudioCapture(DWORD pid, bool excludeTree, const std::atomic<bool>& stop,
    const PcmSink& sink, std::wstring* error);

/*
 * 采集控制器：
 * - 后台线程负责用 WASAPI 的“进程环回”抓取指定应用（含其子进程）的声音
 * - 统一转换成 48kHz 单声道 s16 写入 SharedState::ring
 * - 目标应用退出后自动重连，等待它重启
 */
class CaptureController {
public:
    static CaptureController& Get();

    void ApplySettings(const Settings& settings); /* 音量/目标/开关 */
    void SetTarget(const std::wstring& exe, const std::wstring& label);
    bool Start();
    void Stop();
    bool Running() const { return running_.load(); }

private:
    CaptureController() = default;
    ~CaptureController();
    CaptureController(const CaptureController&) = delete;
    CaptureController& operator=(const CaptureController&) = delete;

    void ThreadMain();
    bool WaitWake(DWORD milliseconds);

    std::thread thread_;
    std::mutex lifecycleMutex_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> retarget_{false};
    HANDLE wake_ = nullptr;

    std::mutex mutex_;
    std::atomic<int> sampleRate_{48000}, bitDepth_{16};
    std::wstring exe_;
    std::wstring label_;
};

} // namespace la

#endif /* LA_PROCESS_LOOPBACK_H */
