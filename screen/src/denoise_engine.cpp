#include "denoise_engine.h"
#include "common.h"
#include "../../third_party/rnnoise/include/rnnoise.h"
#include "../../third_party/sherpa/c-api.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace la {
namespace {
constexpr int kFrame=480, kMaxChannels=2;
constexpr int kDpdfnetDelayBlocks = 4;
constexpr int kRnnoiseDelayBlocks = 2;
constexpr int kHistoryFrames = 8192;
static_assert(kHistoryFrames > 12*kFrame+1);
// Adjust the playback rate by at most 1/16 while moving to or from live audio.
constexpr int kSamplePhase = 16;
constexpr int kFadeFrames = kFrame;
constexpr DWORD kCaptureGapMs = 200;

int ModelDelayBlocks(int model) {
    if (model == static_cast<int>(NoiseModel::Dpdfnet)) return kDpdfnetDelayBlocks;
    if (model == static_cast<int>(NoiseModel::Rnnoise)) return kRnnoiseDelayBlocks;
    return 0;
}

int PlaybackDelayFrames(int model) {
    const int bufferBlocks = model == static_cast<int>(NoiseModel::Rnnoise) ? 5 : 8;
    return (ModelDelayBlocks(model) + bufferBlocks) * kFrame;
}
struct Sherpa {
    HMODULE dll=nullptr;
#define API(name) decltype(&name) name##Fn=nullptr
    API(SherpaOnnxCreateOnlineSpeechDenoiser);
    API(SherpaOnnxDestroyOnlineSpeechDenoiser);
    API(SherpaOnnxOnlineSpeechDenoiserRun);
    API(SherpaOnnxOnlineSpeechDenoiserReset);
    API(SherpaOnnxDestroyDenoisedAudio);
    API(SherpaOnnxCreateLinearResampler);
    API(SherpaOnnxDestroyLinearResampler);
    API(SherpaOnnxLinearResamplerResample);
    API(SherpaOnnxLinearResamplerResampleFree);
    API(SherpaOnnxLinearResamplerReset);
#undef API
    explicit Sherpa(const std::wstring& root) {
        dll=LoadLibraryExW((root+L"\\sherpa-onnx-c-api.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!dll)throw std::runtime_error("cannot load verified speech runtime");
        try {
#define LOAD(name) name##Fn=reinterpret_cast<decltype(name##Fn)>(GetProcAddress(dll,#name));if(!name##Fn)throw std::runtime_error("speech runtime API mismatch")
            LOAD(SherpaOnnxCreateOnlineSpeechDenoiser);LOAD(SherpaOnnxDestroyOnlineSpeechDenoiser);
            LOAD(SherpaOnnxOnlineSpeechDenoiserRun);LOAD(SherpaOnnxOnlineSpeechDenoiserReset);LOAD(SherpaOnnxDestroyDenoisedAudio);
            LOAD(SherpaOnnxCreateLinearResampler);LOAD(SherpaOnnxDestroyLinearResampler);
            LOAD(SherpaOnnxLinearResamplerResample);LOAD(SherpaOnnxLinearResamplerResampleFree);LOAD(SherpaOnnxLinearResamplerReset);
#undef LOAD
        }catch(...){FreeLibrary(dll);dll=nullptr;throw;}
    }
    ~Sherpa(){if(dll)FreeLibrary(dll);}
};
struct Channel {
    DenoiseState* rn=nullptr;
    const SherpaOnnxOnlineSpeechDenoiser* sd=nullptr;
    const SherpaOnnxLinearResampler* up=nullptr;
    std::deque<float> wet,dry,strength;
    float amount=-1;
};
struct Packet {
    uint64_t epoch=0, firstFrame=0;
    int model=0, channels=1, strength=70, slot=0;
    std::array<float,kFrame*kMaxChannels> audio{};
};
template<size_t Capacity> struct Queue {
    std::array<Packet,Capacity> data;
    alignas(64) std::atomic<size_t> head{0};
    alignas(64) std::atomic<size_t> tail{0};
    bool push(const Packet& p){const auto h=head.load(std::memory_order_relaxed);if(h-tail.load(std::memory_order_acquire)>=Capacity)return false;data[h%Capacity]=p;head.store(h+1,std::memory_order_release);return true;}
    bool pop(Packet& p){const auto t=tail.load(std::memory_order_relaxed);if(t==head.load(std::memory_order_acquire))return false;p=data[t%Capacity];tail.store(t+1,std::memory_order_release);return true;}
};
struct WetFrame {
    uint64_t frame=0, epoch=0;
    std::array<float,kMaxChannels> samples{};
};

struct CapturePort {
    Queue<16> output;
    std::atomic<uint64_t> handler{0},epoch{0};
    std::atomic<int> requestedChannels{1},loadedModel{0},loadedChannels{0};
    std::array<std::array<short,kMaxChannels>,kHistoryFrames> dryHistory{};
    std::array<WetFrame,kHistoryFrames> wetHistory{};
    uint64_t captured=0, playback=0, wetEnd=0, lastRevision=0;
    DWORD lastUsed=0, lastBlockMs=0;
    int channels=0, inPosition=0, phase=0;
    int wetMix=0, missingFrames=0;
    bool feeding=false, renderedWet=false;
    Packet pending;

    void InvalidateInference() {
        epoch.fetch_add(1);
        inPosition=0;
        wetEnd=0;
        feeding=false;
        renderedWet=false;
        missingFrames=0;
    }

    void ResetCapture(int newChannels) {
        InvalidateInference();
        playback=captured;
        phase=0;
        wetMix=0;
        channels=newChannels;
    }

    void CollectWet() {
        Packet packet;
        const auto currentEpoch=epoch.load();
        while (output.pop(packet)) {
            if (packet.epoch!=currentEpoch || packet.channels!=channels) continue;
            for (int i=0; i<kFrame; ++i) {
                const uint64_t frame=packet.firstFrame+i;
                if (frame<playback) continue;
                auto& entry=wetHistory[frame%kHistoryFrames];
                entry.frame=frame;
                entry.epoch=packet.epoch;
                std::copy_n(packet.audio.data()+i*channels, channels, entry.samples.begin());
            }
            wetEnd=packet.firstFrame+kFrame;
        }
    }

    bool Render(short* samples, int targetDelay) {
        const uint64_t frame=playback;
        const auto phaseWeight=static_cast<float>(phase)/kSamplePhase;
        const auto& dry=dryHistory[frame%kHistoryFrames];
        const auto& dryNext=dryHistory[std::min(frame+1,captured)%kHistoryFrames];
        const auto& wet=wetHistory[frame%kHistoryFrames];
        const auto& wetNext=wetHistory[(frame+1)%kHistoryFrames];
        const bool hasWet=wet.epoch!=0 && wet.frame==frame &&
            (phase==0 || (wetNext.frame==frame+1 && wetNext.epoch==wet.epoch));
        const bool currentWet=feeding && hasWet && wet.epoch==epoch.load();
        // Fade toward aligned dry audio before a depleted queue can run out.
        const bool wetReady=currentWet && wetEnd>frame+kFadeFrames;
        if (!hasWet) wetMix=0;
        else if (wetReady) wetMix=std::min(kFadeFrames,wetMix+1);
        else wetMix=std::max(0,wetMix-1);
        if (currentWet && wetMix>0) renderedWet=true;
        if (currentWet) missingFrames=0;
        else if (feeding && renderedWet) ++missingFrames;

        const bool edited=frame!=captured || phase!=0 || wetMix>0;
        if (edited) {
            const auto wetWeight=static_cast<float>(wetMix)/kFadeFrames;
            for (int channel=0; channel<channels; ++channel) {
                const float raw=dry[channel]+phaseWeight*(dryNext[channel]-dry[channel]);
                float value=raw;
                if (wetMix>0) {
                    const float processed=phase==0 ? wet.samples[channel] :
                        wet.samples[channel]+phaseWeight*(wetNext.samples[channel]-wet.samples[channel]);
                    value+=wetWeight*(processed*32768.0f-raw);
                }
                samples[channel]=static_cast<short>(std::clamp(static_cast<int>(std::lrint(value)),-32768,32767));
            }
        }

        // Both sources are read at the same position. Move it forward even
        // when the worker is late, or the user disables or changes the model.
        const int delay=static_cast<int>(captured-frame)*kSamplePhase-phase;
        const int target=targetDelay*kSamplePhase;
        const int step=kSamplePhase+std::clamp(delay-target,-1,1);
        const int nextPhase=phase+step;
        playback+=nextPhase/kSamplePhase;
        phase=nextPhase%kSamplePhase;
        return edited;
    }
};
struct Controller {
    HANDLE wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    ~Controller(){if(wake)CloseHandle(wake);}
    std::wstring root;
    std::atomic<int> model{0},strength{70};
    std::atomic<uint64_t> revision{1};
    std::atomic<bool> stopping{false},failure{false};
    std::thread worker;
    Queue<32> input;
    std::array<CapturePort,4> ports;
    std::atomic<unsigned> overloads{0};
};
std::unique_ptr<Controller> control;
}

struct DenoiseStream::Impl {
    int model,channels;
    std::unique_ptr<Sherpa> sherpa;
    std::array<Channel,kMaxChannels> state;
    Impl(const std::wstring& directory,int m,int c):model(m),channels(c){
        if(c<1||c>2||(m!=1&&m!=2&&m!=3))throw std::runtime_error("unsupported denoise stream");
        try{
            if(model==1||model==3)sherpa=std::make_unique<Sherpa>(directory);
            for(int i=0;i<channels;++i){auto& ch=state[i];
                if(model==2){ch.rn=rnnoise_create(nullptr);if(!ch.rn)throw std::bad_alloc();}
                else if(model==3){
                    SherpaOnnxOnlineSpeechDenoiserConfig cfg{};
                    const auto file=WideToUtf8(directory+L"\\dpdfnet_baseline.onnx");
                    cfg.model.dpdfnet.model=file.c_str();cfg.model.num_threads=1;cfg.model.provider="cpu";
                    ch.sd=sherpa->SherpaOnnxCreateOnlineSpeechDenoiserFn(&cfg);
                    ch.up=sherpa->SherpaOnnxCreateLinearResamplerFn(16000,48000,7200,6);
                    if(!ch.sd||!ch.up)throw std::runtime_error("DPDFNet initialization failed");
                }else {
                    SherpaOnnxOnlineSpeechDenoiserConfig cfg{};
                    const auto file=WideToUtf8(directory+L"\\gtcrn_simple.onnx");
                    cfg.model.gtcrn.model=file.c_str();cfg.model.num_threads=1;cfg.model.provider="cpu";
                    ch.sd=sherpa->SherpaOnnxCreateOnlineSpeechDenoiserFn(&cfg);
                    ch.up=sherpa->SherpaOnnxCreateLinearResamplerFn(16000,48000,7200,6);
                    if(!ch.sd||!ch.up)throw std::runtime_error("GTCRN initialization failed");
                }
            }
            // Warm up ONNX Runtime sessions off the audio deadline so first live frames do not stall.
            if(model==1||model==3){
                std::array<float,kFrame> zero{};
                for(int i=0;i<channels;++i){
                    for(int w=0;w<4;++w){
                        const auto* warmed=sherpa->SherpaOnnxOnlineSpeechDenoiserRunFn(state[i].sd,zero.data(),kFrame,48000);
                        if(warmed)sherpa->SherpaOnnxDestroyDenoisedAudioFn(warmed);
                    }
                }
            }
            reset();
        }catch(...){destroy();throw;}
    }
    void destroy(){for(auto& ch:state){if(ch.rn){rnnoise_destroy(ch.rn);ch.rn=nullptr;}if(ch.sd){sherpa->SherpaOnnxDestroyOnlineSpeechDenoiserFn(ch.sd);ch.sd=nullptr;}if(ch.up){sherpa->SherpaOnnxDestroyLinearResamplerFn(ch.up);ch.up=nullptr;}}}
    ~Impl(){destroy();}
    void reset(){for(int i=0;i<channels;++i){auto& ch=state[i];ch.wet.clear();ch.dry.clear();ch.strength.clear();ch.amount=-1;
        if(model==2){rnnoise_init(ch.rn,nullptr);ch.dry.insert(ch.dry.end(),kRnnoiseDelayBlocks*kFrame,0.0f);ch.strength.insert(ch.strength.end(),kRnnoiseDelayBlocks*kFrame,1.0f);}
        // DPDFNet adds 30 ms internal causal lookahead on top of the 10 ms STFT startup hop
        // (40 ms = 1920 samples at 48 kHz; GetFrameShiftInSamples only reports the STFT hop).
        else if(model==3){sherpa->SherpaOnnxOnlineSpeechDenoiserResetFn(ch.sd);sherpa->SherpaOnnxLinearResamplerResetFn(ch.up);ch.dry.insert(ch.dry.end(),kDpdfnetDelayBlocks*kFrame,0.0f);ch.strength.insert(ch.strength.end(),kDpdfnetDelayBlocks*kFrame,1.0f);}
        else {sherpa->SherpaOnnxOnlineSpeechDenoiserResetFn(ch.sd);sherpa->SherpaOnnxLinearResamplerResetFn(ch.up);}
    }}
};
DenoiseStream::DenoiseStream(const std::wstring& root,int model,int channels):impl_(new Impl(root,model,channels)){}
DenoiseStream::~DenoiseStream()=default;
void DenoiseStream::Reset(){impl_->reset();}
void DenoiseStream::Feed(const float* audio,int frames,float strength){
    if(!audio||frames!=kFrame)throw std::runtime_error("expected 480 sample capture frame");
    strength=std::isfinite(strength)?std::clamp(strength,0.0f,1.0f):1.0f;
    for(int c=0;c<impl_->channels;++c){auto& ch=impl_->state[c];std::array<float,kFrame> input{},out{};
        for(int i=0;i<frames;++i){const float value=audio[i*impl_->channels+c];input[i]=std::isfinite(value)?std::clamp(value,-1.0f,1.0f):0.0f;ch.dry.push_back(input[i]);if(ch.amount<0)ch.amount=strength;else ch.amount+=(strength-ch.amount)*.004f;ch.strength.push_back(ch.amount);}
        if(impl_->model==2){for(auto& x:input)x*=32768.0f;rnnoise_process_frame(ch.rn,out.data(),input.data());for(float x:out)ch.wet.push_back(x/32768.0f);}
        else {
            const auto* enhanced=impl_->sherpa->SherpaOnnxOnlineSpeechDenoiserRunFn(ch.sd,input.data(),frames,48000);
            if(enhanced){
                if(enhanced->sample_rate!=16000){impl_->sherpa->SherpaOnnxDestroyDenoisedAudioFn(enhanced);throw std::runtime_error(impl_->model==3?"unexpected DPDFNet sample rate":"unexpected GTCRN sample rate");}
                const auto* resampled=impl_->sherpa->SherpaOnnxLinearResamplerResampleFn(ch.up,enhanced->samples,enhanced->n,0);
                if(resampled){for(int i=0;i<resampled->n;++i)ch.wet.push_back(resampled->samples[i]);impl_->sherpa->SherpaOnnxLinearResamplerResampleFreeFn(resampled);}
                impl_->sherpa->SherpaOnnxDestroyDenoisedAudioFn(enhanced);
            }
        }
        if(ch.dry.size()>48000||ch.wet.size()>48000)throw std::runtime_error("speech runtime output stalled");
    }
}
size_t DenoiseStream::Read(float* audio,size_t frames){
    size_t count=frames;
    for(int c=0;c<impl_->channels;++c)count=std::min(count,std::min(impl_->state[c].wet.size(),impl_->state[c].dry.size()));
    for(size_t i=0;i<count;++i)for(int c=0;c<impl_->channels;++c){auto& ch=impl_->state[c];const float dry=ch.dry.front(),wet=ch.wet.front(),amount=ch.strength.front();ch.dry.pop_front();ch.wet.pop_front();ch.strength.pop_front();const float value=dry*(1-amount)+wet*amount;audio[i*impl_->channels+c]=std::isfinite(value)?std::clamp(value,-1.0f,1.0f):dry;}
    return count;
}

bool InitializeDenoise(const std::wstring& runtime,int model,int strength){
    if(control)return false;
    auto c=std::make_unique<Controller>();if(!c->wake)return false;c->root=runtime;
    c->model=std::clamp(model,0,3);
    c->strength=std::clamp(strength,0,100);
    control=std::move(c);auto* ptr=control.get();
    try{ptr->worker=std::thread([ptr]{
        struct Lane {
            std::unique_ptr<DenoiseStream> stream;
            uint64_t epoch=0, nextFrame=0, expectedInput=0;
            int model=0,channels=0;
            std::array<float,kFrame*kMaxChannels> assembled{};
            size_t count=0;
            int startupBlocks=0;
        };
        std::array<Lane,4> lanes;
        while(!ptr->stopping.load(std::memory_order_acquire)){
            const int desired=ptr->model.load(std::memory_order_acquire);
            for(size_t i=0;i<lanes.size();++i){auto& lane=lanes[i];auto& port=ptr->ports[i];
                if(i!=0&&port.handler.load()==0)continue;
                const int channels=port.requestedChannels.load();
                if(desired!=0&&(desired!=lane.model||channels!=lane.channels)&&!ptr->failure){
                    port.loadedModel=0;
                    try{lane.stream=std::make_unique<DenoiseStream>(ptr->root,desired,channels);lane.model=desired;lane.channels=channels;lane.epoch=0;lane.count=0;port.loadedChannels=channels;port.loadedModel=desired;}
                    catch(...){ptr->failure=true;lane.model=0;lane.stream.reset();}
                }
            }
            Packet in;if(!ptr->input.pop(in)){WaitForSingleObject(ptr->wake,10);continue;}
            if(in.slot<0||in.slot>=4)continue;
            auto& port=ptr->ports[in.slot];auto& lane=lanes[in.slot];
            if(in.epoch!=port.epoch.load()||in.model!=desired||in.channels!=lane.channels||in.model!=lane.model)continue;
            try{
                if(in.epoch!=lane.epoch || in.firstFrame!=lane.expectedInput){
                    lane.stream->Reset();
                    lane.epoch=in.epoch;
                    lane.count=0;
                    lane.nextFrame=in.firstFrame;
                    lane.startupBlocks=ModelDelayBlocks(in.model);
                }
                lane.expectedInput=in.firstFrame+kFrame;
                lane.stream->Feed(in.audio.data(),kFrame,in.strength/100.0f);
                Packet out;out.epoch=in.epoch;out.model=in.model;out.channels=in.channels;out.slot=in.slot;
                // Partial reads are retained until a complete TS capture block exists.
                for(;;){
                    lane.count+=lane.stream->Read(lane.assembled.data()+lane.count*lane.channels,kFrame-lane.count);
                    if(lane.count<kFrame)break;
                    lane.count=0;
                    // Model alignment prepends padding. Only emit blocks
                    // that correspond to real captured frames.
                    if(lane.startupBlocks>0){
                        --lane.startupBlocks;
                        continue;
                    }
                    out.firstFrame=lane.nextFrame;
                    lane.nextFrame+=kFrame;
                    std::copy_n(lane.assembled.data(),kFrame*lane.channels,out.audio.data());
                    // The callback may have reset this lane during inference.
                    if(in.epoch!=port.epoch.load()) break;
                    if(!port.output.push(out)){ptr->overloads++;ptr->failure=true;break;}
                }
            }catch(...){ptr->failure=true;lane.stream.reset();lane.model=0;port.loadedModel=0;}
        }
    });}catch(...){control.reset();return false;}
    return true;
}
void ShutdownDenoise(){if(!control)return;control->stopping=true;SetEvent(control->wake);if(control->worker.joinable())control->worker.join();control.reset();}
DenoiseConfig GetDenoiseConfig(){return control?DenoiseConfig{control->model.load(),control->strength.load()}:DenoiseConfig{};}
bool SetDenoiseConfig(int model,int strength){
    if(!control||model<0||model>3||strength<0||strength>100)return false;
    const int old=control->model.exchange(model);
    const int oldStrength=control->strength.exchange(strength);
    control->failure=false;control->overloads=0;
    if(old!=model||(oldStrength==0)!=(strength==0))control->revision.fetch_add(1);
    SetEvent(control->wake);
    return true;
}
std::wstring DenoiseStatus(){
    if(!control)return L"降噪组件未加载";
    if(control->failure)return control->overloads.load()?L"模型处理跟不上，已恢复原始麦克风声音；请重新选择模型":L"模型处理失败，已保留原始麦克风声音；请重新选择模型";
    if(control->overloads.load())return L"检测到处理跟不上，请切换模型或降低系统负载";
    if(control->model==0||control->strength==0)return L"AI 降噪已关闭";
    if(control->ports[0].loadedModel!=control->model)return L"正在加载模型，暂时保留原始麦克风声音";
    return L"仅处理麦克风，音乐共享声音保持原样";
}
bool DenoiseActive(){
    auto* c=control.get();
    return c&&!c->failure&&c->model.load(std::memory_order_acquire)!=0&&c->strength.load(std::memory_order_relaxed)!=0;
}
bool ProcessDenoise(uint64_t handler, short* samples, int frames, int channels) {
    auto* c=control.get();
    if (!c || !handler || !samples || frames<1 || frames>8192 || channels<1 || channels>kMaxChannels) return false;
    const int model=c->model.load(std::memory_order_acquire);
    const int strength=c->strength.load(std::memory_order_relaxed);
    const bool enabled=model!=0 && strength!=0 && !c->failure;
    const DWORD now=GetTickCount();
    int slot=-1;
    for (int i=0; i<4; ++i) {
        if (c->ports[i].handler.load()==handler) { slot=i; break; }
    }
    if (slot<0) {
        if (!enabled) return false;
        for (int i=0; i<4; ++i) {
            if (c->ports[i].handler.load()==0) { slot=i; break; }
        }
        if (slot<0) {
            for (int i=0; i<4; ++i) {
                if (now-c->ports[i].lastUsed>2000) { slot=i; break; }
            }
        }
        // Additional simultaneous capture streams keep their original audio.
        if (slot<0) return false;
        auto& port=c->ports[slot];
        port.ResetCapture(channels);
        port.lastUsed=now;
        port.handler=handler;
    }

    auto& p=c->ports[slot];
    if (p.channels!=channels || now-p.lastUsed>kCaptureGapMs+p.lastBlockMs) p.ResetCapture(channels);
    p.lastUsed=now;
    p.lastBlockMs=static_cast<DWORD>(frames/48);
    p.requestedChannels=channels;

    const auto revision=c->revision.load(std::memory_order_acquire);
    const bool ready=enabled && p.loadedModel==model && p.loadedChannels==channels;
    if (ready!=p.feeding || revision!=p.lastRevision) {
        p.InvalidateInference();
        p.feeding=ready;
        p.lastRevision=revision;
    }
    p.CollectWet();
    if (!enabled && p.playback==p.captured && p.phase==0 && p.wetMix==0) {
        p.playback+=frames;
        p.captured+=frames;
        return false;
    }

    int targetDelay=enabled ? PlaybackDelayFrames(model) : 0;
    bool edited=false;
    for (int i=0; i<frames; ++i) {
        auto* input=samples+i*channels;
        auto& saved=p.dryHistory[p.captured%kHistoryFrames];
        std::copy_n(input,channels,saved.begin());
        if (p.feeding) {
            if (p.inPosition==0) p.pending.firstFrame=p.captured;
            for (int channel=0; channel<channels; ++channel) {
                p.pending.audio[p.inPosition*channels+channel]=saved[channel]/32768.0f;
            }
            if (++p.inPosition==kFrame) {
                p.inPosition=0;
                p.pending.epoch=p.epoch.load();
                p.pending.model=model;
                p.pending.channels=channels;
                p.pending.strength=strength;
                p.pending.slot=slot;
                if (c->input.push(p.pending)) SetEvent(c->wake);
                else {
                    c->overloads++;
                    c->failure=true;
                    p.InvalidateInference();
                    targetDelay=0;
                }
                p.CollectWet();
            }
        }

        edited=p.Render(input,targetDelay) || edited;
        ++p.captured;
        if (p.missingFrames>=3*kFrame) {
            c->overloads++;
            c->failure=true;
            p.InvalidateInference();
            targetDelay=0;
        }
    }
    return edited;
}
}
