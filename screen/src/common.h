/*
 * ts3-localaudio - TeamSpeak 3 client plugin
 * 把本地某个应用的声音（QQ音乐 / 网易云音乐 / 浏览器 ...）混入麦克风流，发给频道内其他人。
 */
#ifndef LA_COMMON_H
#define LA_COMMON_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "ring_buffer.h"

namespace la {

/* 输出到 TeamSpeak 客户端日志，未连接时静默 */
using LogSink = void (*)(const char* message);
extern LogSink g_logSink;
void Log(const char* fmt, ...);

std::string WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& s);
std::wstring ToLower(const std::wstring& s);

/* 全局共享状态：插件线程、WASAPI 采集线程、TS3 音频线程之间交换数据 */
struct SharedState {
    /* 目标应用（采集线程读取） */
    std::mutex targetMutex;
    std::wstring targetExe;   /* 小写 exe 名，例如 "qqmusic.exe" */
    std::wstring targetLabel; /* 显示名，例如 "QQ音乐" */

    /* 开关与参数（原子量，音频线程直接读） */
    std::atomic<bool> captureRunning{false}; /* 采集线程是否在跑 */
    std::atomic<bool> mixing{false};         /* 是否真的往麦克风流里混音 */
    std::atomic<bool> forceSend{true};       /* 未按键时也强制发送 */
    std::atomic<bool> keepMicActive{true};   /* 转播期间保持麦克风激活(PTT 场景) */
    std::atomic<float> gain{1.0f};           /* 0.0 - 2.0 */
    std::atomic<bool> dialogOpen{false};
    std::atomic<uint32_t> lastMixedTick{0};    /* 最近一次确实向 TS3 回调写入应用采样 */
    std::atomic<uint32_t> lastCallbackTick{0}; /* TS3 音频线程最近一次回调的时刻，用于丢弃积压数据 */

    /* 采集线程 -> 音频线程 的 48kHz 单声道 s16 数据 */
    SpscRing ring{1u << 17}; /* 131072 采样 ≈ 2.7 秒 */

    std::mutex statusMutex;
    std::wstring status = L"未开始";
    void SetStatus(const std::wstring& s)
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        status = s;
    }
    std::wstring GetStatus()
    {
        std::lock_guard<std::mutex> lock(statusMutex);
        return status;
    }
};

SharedState& State();

/* 采集输出统一格式：48kHz / 单声道 / s16 */
constexpr int kMixSampleRate = 48000;

} // namespace la

#endif /* LA_COMMON_H */
