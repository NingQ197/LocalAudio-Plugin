/*
 * ts3-localaudio - TeamSpeak 3 client plugin
 *
 * 把本地指定应用（QQ音乐 / 网易云音乐 / 浏览器 ...）的声音混入麦克风流，
 * 频道里的其他人就能听到你在放的音乐，而你自己的麦克风说话照常可用。
 *
 * 技术要点：TeamSpeak 客户端插件没有"直接播放音频到频道"的 API，
 * 官方提供的是 ts3plugin_onEditCapturedVoiceDataEvent —— 在采集到的麦克风
 * 数据编码之前回调，插件可以修改这段 PCM。我们把应用声音混进去即可。
 */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <algorithm>

#include <windows.h>
#include <shellapi.h>

#include "teamspeak/public_definitions.h"
#include "teamspeak/public_errors.h"
#include "teamlog/logtypes.h"
#include "ts3_functions.h"
#include "plugin.h"

#include "common.h"
#include "config_dialog.h"
#include "denoise_engine.h"
#include "denoise_runtime.h"
#include "process_loopback.h"
#include "settings.h"
#include "ts3_bridge.h"
#include "share_bridge.h"
#include "share_window.h"

#define PLUGIN_API_VERSION 26

static struct TS3Functions ts3Functions;
static char* g_pluginID = nullptr;
static std::wstring g_configFile;
static la::Settings g_settings;
// A tab switch can overlap the old and new server's audio callbacks. Never let
// two callbacks consume the SPSC ring at once; contention skips one frame.
static std::atomic_flag g_audioCallbackBusy = ATOMIC_FLAG_INIT;
static std::atomic<bool> g_pluginStopping{false};
struct AudioCallbackGuard {
    ~AudioCallbackGuard() { g_audioCallbackBusy.clear(std::memory_order_release); }
};

static std::string Utf8(const wchar_t* text)
{
    return la::WideToUtf8(text);
}

/* 插件 DLL 所在目录：降噪运行库与模型就放在它下面的 ts3localaudio\denoise */
static std::wstring PluginDirectory()
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&g_settings), &module))
        return std::wstring();
    wchar_t path[32768] = {0};
    if (!GetModuleFileNameW(module, path, (DWORD)(sizeof(path) / sizeof(path[0]))))
        return std::wstring();
    std::wstring full(path);
    const size_t slash = full.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : full.substr(0, slash);
}

/*********************************** Required functions ************************************/

const char* ts3plugin_name()
{
    static std::string name = Utf8(L"LocalAudio · 屏幕版");
    return name.c_str();
}

const char* ts3plugin_version()
{
    return "2.5.4";
}

int ts3plugin_apiVersion()
{
    return PLUGIN_API_VERSION;
}

const char* ts3plugin_author()
{
    return "ningq";
}

const char* ts3plugin_description()
{
    static std::string desc = Utf8(L"独立插件：GTCRN / RNNoise 麦克风降噪、本机应用音频共享，以及 P2P / 可选中继屏幕共享。Design by ningq。");
    return desc.c_str();
}

void ts3plugin_setFunctionPointers(const struct TS3Functions funcs)
{
    ts3Functions = funcs;
}

int ts3plugin_init()
{
    g_pluginStopping=false;
    la::g_logSink = &la::Ts3LogLine;
    if(!la::VerifyShareWindowInstallation()) {
        la::Log("LocalAudio installation integrity check failed; plugin initialization stopped.");
        return 1;
    }
    char configPath[1024] = {0};
    ts3Functions.getConfigPath(configPath, sizeof(configPath));

    std::wstring dir = la::Utf8ToWide(configPath);
    if (dir.empty())
        dir = L".";
    g_configFile = dir + L"\\ts3-localaudio.ini";
    g_settings = la::LoadSettings(g_configFile);

    la::g_logSink = &la::Ts3LogLine;
    la::SetConfigPathForDialog(g_configFile);

    la::CaptureController& controller = la::CaptureController::Get();
    controller.ApplySettings(g_settings);
    la::TS3ScreenInitialize(ts3Functions, g_pluginID, dir);
    la::SetShareWindowConfigDirectory(dir);

    /* Verify and hold runtime files before any model or UI DLL is loaded. */
    {
        const std::wstring runtime=PluginDirectory()+L"\\ts3localaudio\\denoise";
        if(!la::VerifyDenoiseRuntime(runtime)){la::TS3ScreenShutdown();return 1;}
        if(!la::InitializeDenoise(runtime,g_settings.denoiseModel,g_settings.denoiseStrength)){
            la::ReleaseDenoiseRuntime();la::TS3ScreenShutdown();return 1;
        }
        if(!la::OpenDenoiseUi(runtime,g_configFile))la::Log("Native audio settings UI unavailable in this host.");
    }

    la::Log("plugin initialized, config=%ls, target=%ls", g_configFile.c_str(), g_settings.targetExe.c_str());

    if (g_settings.autoStart && g_settings.HasTarget())
        controller.Start();

    return 0;
}

void ts3plugin_shutdown()
{
    g_pluginStopping=true;
    la::CloseShareWindow();
    la::TS3ScreenShutdown();
    la::CloseConfigDialog(); // Joins the UI worker before any code may be unloaded.
    la::CaptureController::Get().Stop();
    la::CloseDenoiseUi();
    while(g_audioCallbackBusy.test_and_set(std::memory_order_acquire))Sleep(1);
    la::ShutdownDenoise();
    g_audioCallbackBusy.clear(std::memory_order_release);
    la::ReleaseDenoiseRuntime();
    la::Ts3TransmitHoldEnd();
    la::g_logSink = nullptr;

    if (g_pluginID) {
        free(g_pluginID);
        g_pluginID = nullptr;
    }
}

void ts3plugin_registerPluginID(const char* id)
{
    if (!id)
        return;
    free(g_pluginID);
    g_pluginID = nullptr;
    size_t len = strlen(id) + 1;
    g_pluginID = (char*)malloc(len);
    if (g_pluginID)
        memcpy(g_pluginID, id, len);
    la::TS3ScreenSetPluginID(g_pluginID);
}

void ts3plugin_freeMemory(void* data)
{
    free(data);
}

/* Request first-install loading. This never starts capture; autoStart defaults to false. */
int ts3plugin_requestAutoload()
{
    return 1;
}

/*********************************** 配置窗口 ************************************/

int ts3plugin_offersConfigure()
{
    return PLUGIN_OFFERS_CONFIGURE_NEW_THREAD;
}

static void OpenMainWindow()
{
    if (!la::ShowShareWindow())
        la::ShowConfigDialog();
}

void ts3plugin_configure(void* /*handle*/, void* /*qParentWidget*/)
{
    OpenMainWindow();
}

/*********************************** 菜单 ************************************/

enum MenuId { MENU_OPEN_WINDOW = 1, MENU_TOGGLE = 2, MENU_OPEN_CONFIG = 3, MENU_NATIVE_AUDIO = 4, MENU_TUTORIAL = 5 };

void ts3plugin_initMenus(struct PluginMenuItem*** menuItems, char** menuIcon)
{
    const int count = 5;
    *menuItems = (struct PluginMenuItem**)malloc(sizeof(struct PluginMenuItem*) * (count + 1));
    *menuIcon = nullptr;

    const wchar_t* texts[count] = {
        L"LocalAudio：声音与屏幕共享…",
        L"本地声音：开始 / 停止转播",
        L"本地声音：打开配置文件夹",
        L"本地声音：经典音频设置（兼容模式）…",
        L"LocalAudio：使用教程…",
    };

    for (int i = 0; i < count; ++i) {
        struct PluginMenuItem* item = (struct PluginMenuItem*)malloc(sizeof(struct PluginMenuItem));
        item->type = PLUGIN_MENU_TYPE_GLOBAL;
        item->id = i + 1;
        std::string text = Utf8(texts[i]);
        strncpy(item->text, text.c_str(), PLUGIN_MENU_BUFSZ - 1);
        item->text[PLUGIN_MENU_BUFSZ - 1] = '\0';
        item->icon[0] = '\0';
        (*menuItems)[i] = item;
    }
    (*menuItems)[count] = nullptr;
}

static void ToggleCapture()
{
    la::CaptureController& controller = la::CaptureController::Get();
    if (controller.Running()) {
        controller.Stop();
        return;
    }

    g_settings = la::LoadSettings(g_configFile);
    if (!g_settings.HasTarget()) {
        OpenMainWindow();
        return;
    }
    controller.ApplySettings(g_settings);
    controller.Start();
}

static void NotifyCurrentTab(const std::wstring& text)
{
    if (ts3Functions.printMessageToCurrentTab)
        ts3Functions.printMessageToCurrentTab(Utf8(text.c_str()).c_str());
}

void ts3plugin_onMenuItemEvent(uint64 serverConnectionHandlerID, enum PluginMenuType type, int menuItemID, uint64 selectedItemID)
{
    (void)serverConnectionHandlerID;
    (void)selectedItemID;
    if (type != PLUGIN_MENU_TYPE_GLOBAL)
        return;

    switch (menuItemID) {
    case MENU_OPEN_WINDOW:
        OpenMainWindow();
        break;
    case MENU_TUTORIAL:
        la::ShowDenoiseTutorial();
        break;
    case MENU_NATIVE_AUDIO:
        la::ShowConfigDialog();
        break;
    case MENU_TOGGLE: {
        bool wasRunning = la::CaptureController::Get().Running();
        ToggleCapture();
        if (wasRunning)
            NotifyCurrentTab(L"[本地声音] 已停止转播");
        else if (la::CaptureController::Get().Running())
            NotifyCurrentTab(L"[本地声音] 开始采集：" + g_settings.targetLabel);
        break;
    }
    case MENU_OPEN_CONFIG: {
        std::wstring dir = g_configFile.substr(0, g_configFile.find_last_of(L'\\'));
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    default:
        break;
    }
}

/*********************************** 热键 ************************************/

void ts3plugin_initHotkeys(struct PluginHotkey*** hotkeys)
{
    *hotkeys = (struct PluginHotkey**)malloc(sizeof(struct PluginHotkey*) * 2);

    struct PluginHotkey* item = (struct PluginHotkey*)malloc(sizeof(struct PluginHotkey));
    strncpy(item->keyword, "toggle_capture", PLUGIN_HOTKEY_BUFSZ - 1);
    item->keyword[PLUGIN_HOTKEY_BUFSZ - 1] = '\0';
    std::string desc = Utf8(L"开始 / 停止转播本地应用声音");
    strncpy(item->description, desc.c_str(), PLUGIN_HOTKEY_BUFSZ - 1);
    item->description[PLUGIN_HOTKEY_BUFSZ - 1] = '\0';

    (*hotkeys)[0] = item;
    (*hotkeys)[1] = nullptr;
}

void ts3plugin_onHotkeyEvent(const char* keyword)
{
    if (keyword && strcmp(keyword, "toggle_capture") == 0)
        ToggleCapture();
}

void ts3plugin_onPluginCommandEvent(uint64 handler, const char* pluginName, const char* command,
                                   anyID invoker, const char* invokerName, const char* invokerUid)
{
    la::TS3ScreenOnPluginCommand(handler, pluginName, command, invoker, invokerName, invokerUid);
}

void ts3plugin_currentServerConnectionChanged(uint64 handler)
{
    la::TS3ScreenCurrentHandlerChange(handler);
}

void ts3plugin_onConnectStatusChangeEvent(uint64 handler, int status, unsigned int /*error*/)
{
    const uint64 current=ts3Functions.getCurrentServerConnectionHandlerID?ts3Functions.getCurrentServerConnectionHandlerID():0;
    if (status == STATUS_DISCONNECTED && (!current || current==handler))
        la::TS3ScreenCurrentHandlerChange(handler);
}

static void NotifyMembershipChange(uint64 handler,anyID client)
{
    if(!client || !ts3Functions.getCurrentServerConnectionHandlerID ||
       ts3Functions.getCurrentServerConnectionHandlerID()!=handler)return;
    anyID self=0;
    if(ts3Functions.getClientID&&ts3Functions.getClientID(handler,&self)==ERROR_ok&&self==client){
        la::TS3ScreenCurrentHandlerChange(handler);return;
    }
    la::TS3ScreenPeerMembershipChanged(handler,client);
}

void ts3plugin_onClientMoveEvent(uint64 handler, anyID client, uint64 /*oldChannel*/, uint64 /*newChannel*/,
                               int /*visibility*/, const char* /*message*/)
{
    NotifyMembershipChange(handler,client);
}

void ts3plugin_onClientMoveSubscriptionEvent(uint64 handler,anyID client,uint64,uint64,int)
{NotifyMembershipChange(handler,client);}
void ts3plugin_onClientMoveTimeoutEvent(uint64 handler,anyID client,uint64,uint64,int,const char*)
{NotifyMembershipChange(handler,client);}
void ts3plugin_onClientMoveMovedEvent(uint64 handler,anyID client,uint64,uint64,int,anyID,const char*,const char*,const char*)
{NotifyMembershipChange(handler,client);}
void ts3plugin_onClientKickFromChannelEvent(uint64 handler,anyID client,uint64,uint64,int,anyID,const char*,const char*,const char*)
{NotifyMembershipChange(handler,client);}
void ts3plugin_onClientKickFromServerEvent(uint64 handler,anyID client,uint64,uint64,int,anyID,const char*,const char*,const char*)
{NotifyMembershipChange(handler,client);}
void ts3plugin_onClientBanFromServerEvent(uint64 handler,anyID client,uint64,uint64,int,anyID,const char*,const char*,uint64,const char*)
{NotifyMembershipChange(handler,client);}

/*********************************** 核心：把应用声音混进麦克风 ************************************/

void ts3plugin_onEditCapturedVoiceDataEvent(uint64 serverConnectionHandlerID, short* samples, int sampleCount, int channels, int* edited)
{
    if (g_pluginStopping.load(std::memory_order_acquire)||!samples || !edited || channels <= 0 || sampleCount <= 0)
        return;
    if (g_audioCallbackBusy.test_and_set(std::memory_order_acquire))
        return;
    AudioCallbackGuard callbackGuard;
    if(g_pluginStopping.load(std::memory_order_acquire))return;
    const bool mixing = la::State().mixing.load(std::memory_order_relaxed);
    if (!mixing && !la::DenoiseActive())return;

    /* 先处理人声，再混音乐：顺序反了会把歌当成噪声一起去噪。
       降噪关闭时 ProcessDenoise 不碰 samples，直接返回 false。 */
    if (la::ProcessDenoise(serverConnectionHandlerID, samples, sampleCount, channels))
        *edited |= 1;

    if (!mixing)
        return;
    if (!la::Ts3ShouldMixOnHandler(serverConnectionHandlerID))
        return;

    la::State().lastCallbackTick.store((uint32_t)GetTickCount(), std::memory_order_relaxed);

    const float rawGain = la::State().gain.load(std::memory_order_relaxed);
    const float targetGain = std::isfinite(rawGain) ? std::max(0.0f, std::min(2.0f, rawGain)) : 1.0f;
    static thread_local float currentGain = 0.0f;
    static thread_local int16_t chunk[2048];

    int frames = sampleCount;
    int offset = 0;
    bool wrote = false;

    while (offset < frames) {
        int want = frames - offset;
        if (want > 2048)
            want = 2048;

        size_t got = la::State().ring.read(chunk, (size_t)want);
        if (got == 0)
            break;

        for (size_t i = 0; i < got; ++i) {
            currentGain += (targetGain - currentGain) * 0.002f; /* 约 10ms 的平滑，避免咔哒声 */
            int32_t music = (int32_t)lrintf((float)chunk[i] * currentGain);
            const size_t frameIndex = (size_t)offset + i;
            for (int c = 0; c < channels; ++c) {
                size_t index = frameIndex * (size_t)channels + (size_t)c;
                int32_t mixed = (int32_t)samples[index] + music;
                if (mixed > 32767)
                    mixed = 32767;
                else if (mixed < -32768)
                    mixed = -32768;
                samples[index] = (short)mixed;
            }
        }

        wrote = true;
        offset += (int)got;
        if ((int)got < want)
            break;
    }

    if (wrote) {
        la::State().lastMixedTick.store((uint32_t)GetTickCount(), std::memory_order_relaxed);
        *edited |= 1; /* 告诉客户端：数据被修改过 */
        if (la::State().forceSend.load(std::memory_order_relaxed))
            *edited |= 2; /* 请求发送修改后的帧；不绕过客户端/服务器静音 */
    }
}

/*********************************** 与采集线程的桥接 ************************************/

namespace la {

void Ts3LogLine(const char* message)
{
    if (ts3Functions.logMessage && message)
        ts3Functions.logMessage(message, LogLevel_INFO, "LocalAudio", 0);
}

namespace {

std::mutex g_holdMutex;
bool g_holdRequested = false;
uint64 g_holdHandler = 0;
std::atomic<uint64> g_mixHandler{0};
bool g_inputChanged = false;
std::string g_vadOriginal;
std::string g_vadApplied;

uint64 CurrentHandler()
{
    return ts3Functions.getCurrentServerConnectionHandlerID ?
        ts3Functions.getCurrentServerConnectionHandlerID() : 0;
}

std::string ReadVad(uint64 handler)
{
    char* value = nullptr;
    std::string result;
    if (ts3Functions.getPreProcessorConfigValue &&
        ts3Functions.getPreProcessorConfigValue(handler, "vad", &value) == ERROR_ok && value) {
        result = value;
        if (ts3Functions.freeMemory)
            ts3Functions.freeMemory(value);
    }
    return result;
}

std::string VadOffValue(const std::string& current)
{
    std::string lowered = current;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return (char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    });
    if (lowered == "1") return "0";
    if (lowered == "true") return "false";
    return current;
}

/* Always restore the handler we actually modified, even after a tab change.
 * Do not overwrite a new value explicitly selected by the user while capturing. */
void ReleaseHoldLocked()
{
    if (g_holdHandler) {
        if (!g_vadApplied.empty() && ts3Functions.setPreProcessorConfigValue &&
            ReadVad(g_holdHandler) == g_vadApplied) {
            ts3Functions.setPreProcessorConfigValue(g_holdHandler, "vad", g_vadOriginal.c_str());
        }
        int state = INPUT_DEACTIVATED;
        if (g_inputChanged && ts3Functions.getClientSelfVariableAsInt &&
            ts3Functions.setClientSelfVariableAsInt && ts3Functions.flushClientSelfUpdates &&
            ts3Functions.getClientSelfVariableAsInt(g_holdHandler, CLIENT_INPUT_DEACTIVATED, &state) == ERROR_ok &&
            state == INPUT_ACTIVE) {
            ts3Functions.setClientSelfVariableAsInt(g_holdHandler, CLIENT_INPUT_DEACTIVATED, INPUT_DEACTIVATED);
            ts3Functions.flushClientSelfUpdates(g_holdHandler, nullptr);
        }
    }
    g_holdHandler = 0;
    g_inputChanged = false;
    g_vadOriginal.clear();
    g_vadApplied.clear();
}

} // namespace

bool Ts3ShouldMixOnHandler(uint64_t handler)
{
    return handler != 0 && g_mixHandler.load(std::memory_order_acquire) == handler;
}

void Ts3TransmitHoldBegin()
{
    {
        std::lock_guard<std::mutex> lock(g_holdMutex);
        ReleaseHoldLocked();
        g_holdRequested = true;
    }
    Ts3TransmitHoldTick();
}

void Ts3TransmitHoldTick()
{
    std::lock_guard<std::mutex> lock(g_holdMutex);
    if (!g_holdRequested) return;

    uint64 handler = CurrentHandler();
    int status = STATUS_DISCONNECTED;
    if (!handler || !ts3Functions.getConnectionStatus ||
        ts3Functions.getConnectionStatus(handler, &status) != ERROR_ok || status < STATUS_CONNECTED)
        handler = 0;
    g_mixHandler.store(handler, std::memory_order_release);

    if (handler != g_holdHandler || !State().keepMicActive.load())
        ReleaseHoldLocked();
    if (!handler || !State().keepMicActive.load()) return;

    /* Respect explicit microphone mute. The UI explains that server/client mute wins. */
    int muted = 0;
    if (ts3Functions.getClientSelfVariableAsInt &&
        ts3Functions.getClientSelfVariableAsInt(handler, CLIENT_INPUT_MUTED, &muted) == ERROR_ok && muted)
        return;

    if (!g_holdHandler) {
        g_holdHandler = handler;
        g_vadOriginal = ReadVad(handler);
        std::string off = VadOffValue(g_vadOriginal);
        if (off != g_vadOriginal && ts3Functions.setPreProcessorConfigValue &&
            ts3Functions.setPreProcessorConfigValue(handler, "vad", off.c_str()) == ERROR_ok)
            g_vadApplied = off;
    }

    int state = INPUT_ACTIVE;
    if (ts3Functions.getClientSelfVariableAsInt && ts3Functions.setClientSelfVariableAsInt &&
        ts3Functions.flushClientSelfUpdates &&
        ts3Functions.getClientSelfVariableAsInt(handler, CLIENT_INPUT_DEACTIVATED, &state) == ERROR_ok &&
        state == INPUT_DEACTIVATED) {
        if (ts3Functions.setClientSelfVariableAsInt(handler, CLIENT_INPUT_DEACTIVATED, INPUT_ACTIVE) == ERROR_ok) {
            g_inputChanged = true;
            ts3Functions.flushClientSelfUpdates(handler, nullptr);
        }
    }
}

void Ts3TransmitHoldEnd()
{
    std::lock_guard<std::mutex> lock(g_holdMutex);
    g_holdRequested = false;
    g_mixHandler.store(0, std::memory_order_release);
    ReleaseHoldLocked();
}

} // namespace la
