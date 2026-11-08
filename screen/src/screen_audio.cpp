#include "screen_audio.h"
#include "process_loopback.h"
#include "app_scanner.h"
#include "common.h"
#include <array>
#include <atomic>
#include <mutex>
#include <thread>

namespace la {
namespace {
std::mutex lifecycle;
std::mutex statusMutex;
std::thread captureThread;
std::atomic<bool> stop{true}, running{false};
std::wstring status = L"屏幕音频未开启";

void SetStatus(std::wstring message)
{
    std::lock_guard<std::mutex> lock(statusMutex);
    status = std::move(message);
}

std::string Encode(const int16_t* samples, size_t count)
{
    static constexpr char chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* bytes = reinterpret_cast<const unsigned char*>(samples);
    const size_t size = count * sizeof(int16_t);
    std::string result;
    result.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        unsigned value = unsigned(bytes[i]) << 16;
        if (i + 1 < size) value |= unsigned(bytes[i + 1]) << 8;
        if (i + 2 < size) value |= unsigned(bytes[i + 2]);
        result.push_back(chars[(value >> 18) & 63]);
        result.push_back(chars[(value >> 12) & 63]);
        result.push_back(i + 1 < size ? chars[(value >> 6) & 63] : '=');
        result.push_back(i + 2 < size ? chars[value & 63] : '=');
    }
    return result;
}

bool BlockedTarget(const std::wstring& exe)
{
    const auto name = ToLower(exe);
    return name.empty() || name.find(L'\\') != std::wstring::npos || name.find(L'/') != std::wstring::npos ||
        name == L"ts3client_win64.exe" || name == L"ts3client_win32.exe" ||
        name == L"msedgewebview2.exe";
}

void StopLocked()
{
    stop.store(true, std::memory_order_release);
    if (captureThread.joinable()) captureThread.join();
    running.store(false, std::memory_order_release);
}
} // namespace

bool ScreenAudioStart(bool excludeTs, const std::wstring& exe, ScreenPcmEmitter emitBase64)
{
    // Reusing TS voice music and the separate screen soundtrack would make the
    // same music arrive twice. Screen mode owns sound until explicitly stopped.
    CaptureController::Get().Stop();
    std::lock_guard<std::mutex> lock(lifecycle);
    StopLocked();
    if (!emitBase64 || (!excludeTs && BlockedTarget(exe))) {
        SetStatus(L"请选择音乐或应用进程；不能回采 TS3 或共享窗口的声音");
        return false;
    }
    DWORD pid = GetCurrentProcessId();
    if (!excludeTs && !FindPidByExe(ToLower(exe), &pid)) {
        SetStatus(L"未找到所选应用，请先启动它再开始共享音频");
        return false;
    }
    stop.store(false, std::memory_order_release);
    running.store(true, std::memory_order_release);
    SetStatus(excludeTs ? L"正在启动系统音频（排除 TS3 及其子进程）" : L"正在启动所选应用音频");
    try {
        captureThread = std::thread([pid, excludeTs, emit = std::move(emitBase64)] {
            bool emitted = false;
            std::wstring error;
            const bool ok = RunProcessAudioCapture(pid, excludeTs, stop,
                [&](const int16_t* samples, size_t count) {
                    if (stop.load(std::memory_order_relaxed)) return;
                    if (!emitted) {
                        emitted = true;
                        SetStatus(excludeTs ? L"系统音频已采集，TS3 连麦播放输出已排除" : L"所选应用音频已采集");
                    }
                    emit(Encode(samples, count));
                }, &error);
            if (!ok && !stop.load()) SetStatus(error.empty() ? L"共享音频采集失败" : error);
            running.store(false, std::memory_order_release);
        });
    } catch (const std::exception&) {
        stop.store(true);
        running.store(false);
        SetStatus(L"无法创建共享音频线程");
        return false;
    }
    return true;
}

void ScreenAudioStop()
{
    std::lock_guard<std::mutex> lock(lifecycle);
    StopLocked();
    SetStatus(L"屏幕音频已停止");
}
bool ScreenAudioRunning() { return running.load(std::memory_order_acquire); }
std::wstring ScreenAudioStatus()
{
    std::lock_guard<std::mutex> lock(statusMutex);
    return status;
}
} // namespace la
