#include "process_loopback.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <audioclient.h>
#include <mmreg.h>
#include <objbase.h>

#include "app_scanner.h"
#include "common.h"
#include "ts3_bridge.h"
#include "wasapi_compat.h"

namespace la {

namespace {

std::wstring HresultText(HRESULT hr)
{
    switch (static_cast<unsigned long>(hr)) {
    case 0x80070005UL:
        return L"拒绝访问（目标程序可能以管理员权限运行，请用管理员身份启动 TeamSpeak 再试）";
    case 0x80070490UL:
        return L"找不到该进程的音频会话（应用当前没有在播放声音）";
    case 0x88890008UL:
        return L"音频格式不被支持";
    case 0x88890004UL:
        return L"音频设备无效";
    case 0x8007001FUL:
        return L"设备忙";
    default:
        break;
    }
    wchar_t buf[64];
    swprintf(buf, 64, L"HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

// COM can retain this callback beyond a timeout or SetEvent. Pin the containing
// module once so a late callback/Release never executes in an unloaded DLL.
// Updating this plugin therefore requires fully exiting TeamSpeak.
void ActivationModuleAnchor() {}
bool PinActivationModule()
{
    static const bool pinned = [] {
        HMODULE module = nullptr;
        return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&ActivationModuleAnchor), &module) != FALSE;
    }();
    return pinned;
}

// The completion object owns its event and activation parameters. Windows may
// complete after our timeout; closing a borrowed event earlier is a handle UAF.
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
    explicit ActivationHandler(DWORD pid)
    {
        event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        CoCreateFreeThreadedMarshaler(static_cast<IUnknown*>(this), &marshaler_);
        params_.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params_.ProcessLoopbackParams.TargetProcessId = pid;
        params_.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PropVariantInit(&prop_);
        prop_.vt = VT_BLOB;
        prop_.blob.cbSize = sizeof(params_);
        prop_.blob.pBlobData = reinterpret_cast<BYTE*>(&params_);
    }
    ~ActivationHandler()
    {
        if (marshaler_) marshaler_->Release();
        if (event_) CloseHandle(event_);
    }
    HANDLE Event() const { return event_; }
    PROPVARIANT* Parameters() { return &prop_; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        // IAgileObject is a marker interface (IUnknown only).
        const GUID agile = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler) || riid == agile) {
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        if (riid == __uuidof(IMarshal) && marshaler_)
            return marshaler_->QueryInterface(riid, ppv);
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG count = (ULONG)InterlockedDecrement(&ref_);
        if (count == 0) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override
    {
        SetEvent(event_);
        return S_OK;
    }
private:
    LONG ref_ = 1;
    HANDLE event_ = nullptr;
    IUnknown* marshaler_ = nullptr;
    AUDIOCLIENT_ACTIVATION_PARAMS params_{};
    PROPVARIANT prop_{};
};

/* 一个进程环回采集会话 */
class ProcessLoopbackSession {
public:
    ~ProcessLoopbackSession() { Close(); }

    bool Open(DWORD pid, std::wstring* error, int rate = 48000, int bits = 16);
    void Close();
    bool Pump(SpscRing& ring);
    void WaitForSamples() const { if (sampleReady_) WaitForSingleObject(sampleReady_, 20); }

private:
    void Feed(const BYTE* data, UINT32 frames, SpscRing& ring);

    IAudioClient* client_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    HANDLE sampleReady_ = nullptr;
    int rate_=48000, bits_=16;
    double phase_=0, previousSample_=0;
    std::vector<int16_t> pcm_;
};

bool ProcessLoopbackSession::Open(DWORD pid, std::wstring* error, int rate, int bits)
{
    Close(); rate_=rate; bits_=bits; phase_=0; previousSample_=0;

    if (!PinActivationModule()) {
        if (error) *error = L"无法保护异步音频模块的生命周期";
        return false;
    }
    auto* handler = new ActivationHandler(pid);
    if (!handler->Event()) {
        handler->Release();
        if (error) *error = L"创建同步事件失败";
        return false;
    }
    IActivateAudioInterfaceAsyncOperation* operation = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), handler->Parameters(), handler, &operation);
    if (FAILED(hr) || !operation) {
        if (operation) operation->Release();
        handler->Release();
        if (error) *error = L"激活进程环回失败（此系统可能不支持按进程采集） " + HresultText(hr);
        return false;
    }
    DWORD wait = WaitForSingleObject(handler->Event(), 5000);
    handler->Release();
    if (wait != WAIT_OBJECT_0) {
        operation->Release();
        if (error) *error = wait == WAIT_TIMEOUT ? L"激活音频接口超时" : L"等待音频接口失败";
        return false;
    }

    HRESULT activateResult = E_FAIL;
    IUnknown* unknown = nullptr;
    hr = operation->GetActivateResult(&activateResult, &unknown);
    operation->Release();

    if (FAILED(hr) || FAILED(activateResult) || !unknown) {
        if (unknown)
            unknown->Release();
        if (error)
            *error = L"无法打开目标进程的音频 " + HresultText(FAILED(activateResult) ? activateResult : hr);
        return false;
    }

    hr = unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&client_));
    unknown->Release();
    if (FAILED(hr) || !client_) {
        if (error)
            *error = L"获取 IAudioClient 失败";
        Close();
        return false;
    }

    // Process-loopback virtual devices can return E_NOTIMPL for GetMixFormat.
    // Request a known format as in Microsoft's ApplicationLoopback sample;
    // the audio engine performs format conversion before delivering packets.
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = rate_;
    format.wBitsPerSample = bits_;
    format.nBlockAlign = 2 * bits_ / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        0, 0, &format, nullptr);
    if (FAILED(hr)) {
        if (error) *error = L"初始化采集失败 " + HresultText(hr);
        Close();
        return false;
    }
    sampleReady_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    hr = sampleReady_ ? client_->SetEventHandle(sampleReady_) : HRESULT_FROM_WIN32(GetLastError());
    if (FAILED(hr)) {
        if (error) *error = L"设置音频通知失败 " + HresultText(hr);
        Close();
        return false;
    }

    hr = client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture_));
    if (FAILED(hr) || !capture_) {
        if (error)
            *error = L"获取采集接口失败";
        Close();
        return false;
    }

    hr = client_->Start();
    if (FAILED(hr)) {
        if (error)
            *error = L"启动采集失败 " + HresultText(hr);
        Close();
        return false;
    }
    return true;
}

void ProcessLoopbackSession::Close()
{
    if (client_)
        client_->Stop();
    if (capture_) {
        capture_->Release();
        capture_ = nullptr;
    }
    if (client_) {
        client_->Release();
        client_ = nullptr;
    }
    if (sampleReady_) {
        CloseHandle(sampleReady_);
        sampleReady_ = nullptr;
    }
    pcm_.clear();
}

void ProcessLoopbackSession::Feed(const BYTE* data, UINT32 frames, SpscRing& ring)
{
    // Convert requested signed integer capture PCM to TS3's 48 kHz/s16 mono.
    const int bytes=bits_/8;
    auto sample=[&](UINT32 frame,int channel) -> double {
        if(!data)return 0;
        const BYTE* s=data+(frame*2+channel)*bytes;
        int32_t value=0;
        if(bits_==16) { int16_t v; memcpy(&v,s,2); value=v; }
        else if(bits_==24) { value=s[0]|(s[1]<<8)|(s[2]<<16); if(value&0x800000)value-=0x1000000; value/=256; }
        else { memcpy(&value,s,4); value/=65536; }
        return value;
    };
    pcm_.clear();
    auto mono=[&](int frame){return frame<0?previousSample_:(sample(frame,0)+sample(frame,1))/2;};
    while(phase_<static_cast<double>(frames)-1) {
        int f=static_cast<int>(std::floor(phase_)); double fraction=phase_-f;
        pcm_.push_back(static_cast<int16_t>(mono(f)*(1-fraction)+mono(f+1)*fraction));
        phase_+=static_cast<double>(rate_)/48000;
    }
    if(frames)previousSample_=mono(frames-1);
    phase_-=frames;
    if(!pcm_.empty())ring.write(pcm_.data(),pcm_.size());
}

bool ProcessLoopbackSession::Pump(SpscRing& ring)
{
    if (!capture_)
        return false;

    for (;;) {
        UINT32 packetFrames = 0;
        if (FAILED(capture_->GetNextPacketSize(&packetFrames)))
            return false;
        if (packetFrames == 0)
            break;

        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
            return false;

        if (frames > 0) {
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                Feed(nullptr, frames, ring);
            } else {
                Feed(data, frames, ring);
            }
        }
        if (FAILED(capture_->ReleaseBuffer(frames)))
            return false;
    }
    return true;
}

} // namespace

/* ============================ CaptureController ============================ */

CaptureController& CaptureController::Get()
{
    static CaptureController instance;
    return instance;
}

CaptureController::~CaptureController()
{
    Stop();
    if (wake_)
        CloseHandle(wake_);
}

void CaptureController::ApplySettings(const Settings& settings)
{
    int rate=settings.sampleRate?settings.sampleRate:48000, bits=settings.bitDepth?settings.bitDepth:16;
    if(sampleRate_.exchange(rate)!=rate || bitDepth_.load()!=bits)retarget_.store(true);
    bitDepth_.store(bits);
    float gain = (float)settings.volume / 100.0f;
    State().gain.store(gain);
    State().forceSend.store(settings.forceSend);
    State().keepMicActive.store(settings.keepMicActive);
    SetTarget(settings.targetExe, settings.targetLabel);
}

void CaptureController::SetTarget(const std::wstring& exe, const std::wstring& label)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (exe_ == exe)
            return;
        exe_ = exe;
        label_ = label;
    }
    retarget_.store(true);
    std::lock_guard<std::mutex> lifecycleGuard(lifecycleMutex_);
    if (wake_)
        SetEvent(wake_);
}

bool CaptureController::Start()
{
    std::lock_guard<std::mutex> guard(lifecycleMutex_);
    if (running_.load())
        return true;

    if (!wake_)
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake_)
        return false;

    if (thread_.joinable()) thread_.join();
    stop_.store(false);
    running_.store(true);
    try {
        thread_ = std::thread(&CaptureController::ThreadMain, this);
    } catch (const std::exception&) {
        running_.store(false);
        State().SetStatus(L"无法创建音频采集线程");
        return false;
    }
    return true;
}

void CaptureController::Stop()
{
    std::lock_guard<std::mutex> guard(lifecycleMutex_);
    if (!running_.load() && !thread_.joinable())
        return;

    stop_.store(true);
    if (wake_)
        SetEvent(wake_);
    if (thread_.joinable())
        thread_.join();

    running_.store(false);
    State().mixing.store(false);
    State().ring.clear();
    Ts3TransmitHoldEnd();
    State().SetStatus(L"已停止");
}

bool CaptureController::WaitWake(DWORD milliseconds)
{
    if (!wake_)
        return false;
    return WaitForSingleObject(wake_, milliseconds) == WAIT_OBJECT_0;
}

void CaptureController::ThreadMain()
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult)) {
        State().SetStatus(L"初始化音频 COM 线程失败 " + HresultText(comResult));
        running_.store(false);
        return;
    }

    ProcessLoopbackSession session;

    while (!stop_.load()) {
        retarget_.store(false);

        std::wstring exe, label;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            exe = exe_;
            label = label_;
        }

        if (exe.empty()) {
            State().SetStatus(L"尚未选择要转播的应用");
            WaitWake(400);
            continue;
        }

        DWORD pid = 0;
        if (!FindPidByExe(exe, &pid)) {
            State().SetStatus(L"等待应用启动：" + label);
            WaitWake(800);
            continue;
        }

        std::wstring error;
        if (!session.Open(pid, &error, sampleRate_.load(), bitDepth_.load())) {
            Log("open process loopback failed: %ls", error.c_str());
            State().SetStatus(L"采集失败：" + error);
            WaitWake(1500);
            continue;
        }

        const std::wstring targetStatus = label + L"（PID " + std::to_wstring(pid) + L"）";
        State().lastCallbackTick.store(0, std::memory_order_relaxed);
        State().lastMixedTick.store(0, std::memory_order_relaxed);
        State().SetStatus(L"正在采集 " + targetStatus + L"\r\nTS3 尚未取音频；请连接频道，检查麦克风静音及发送设置。");
        Ts3TransmitHoldBegin();
        State().ring.clear();
        State().mixing.store(true);

        DWORD lastTick = GetTickCount();
        DWORD lastPrune = lastTick;
        bool lost = false;

        while (!stop_.load() && !retarget_.load()) {
            if (!session.Pump(State().ring)) {
                lost = true;
                break;
            }
            DWORD now = GetTickCount();
            if (now - lastTick >= 400) {
                lastTick = now;
                Ts3TransmitHoldTick();
            }
            if (now - lastPrune >= 500) {
                lastPrune = now;
                /* 客户端没有在取数据（未连接 / 未发送）时别让旧音频越积越多 */
                uint32_t callback = State().lastCallbackTick.load(std::memory_order_relaxed);
                const uint32_t mixed = State().lastMixedTick.load(std::memory_order_relaxed);
                if (callback == 0 || (uint32_t)now - callback > 1000) {
                    State().ring.clear();
                    State().SetStatus(L"正在采集 " + targetStatus + L"\r\nTS3 尚未取音频；请连接频道，检查麦克风静音及发送设置。");
                } else if (mixed != 0 && (uint32_t)now - mixed <= 1000) {
                    State().SetStatus(L"正在向 TS3 混入 " + targetStatus + L"\r\n已写入本客户端音频流；频道接收情况请以接收端为准。");
                } else {
                    State().SetStatus(L"正在采集 " + targetStatus + L"\r\nTS3 音频回调活跃，正在等待应用音频数据。");
                }
            }
            if (!PidAlive(pid)) {
                lost = false;
                break;
            }
            session.WaitForSamples();
        }

        State().mixing.store(false);
        State().ring.clear();
        session.Close();
        Ts3TransmitHoldEnd();

        if (stop_.load())
            break;
        if (retarget_.load())
            continue;

        State().SetStatus(lost ? L"音频流中断，正在重连…" : L"目标应用已退出，等待重连…");
        WaitWake(600);
    }

    State().mixing.store(false);
    running_.store(false);
    CoUninitialize();
}

} // namespace la
