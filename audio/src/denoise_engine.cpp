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
    const SherpaOnnxOnlineSpeechDenoiser* gt=nullptr;
    const SherpaOnnxLinearResampler* up=nullptr;
    std::deque<float> wet,dry,strength;
    float amount=-1;
};
struct Packet {
    uint64_t epoch=0;
    int model=0, channels=1, strength=70, slot=0;
    std::array<float,kFrame*kMaxChannels> audio{};
};
template<size_t Capacity> struct Queue {
    std::array<Packet,Capacity> data;
    alignas(64) std::atomic<size_t> head{0};
    alignas(64) std::atomic<size_t> tail{0};
    bool push(const Packet& p){const auto h=head.load(std::memory_order_relaxed);if(h-tail.load(std::memory_order_acquire)>=Capacity)return false;data[h%Capacity]=p;head.store(h+1,std::memory_order_release);return true;}
    bool pop(Packet& p){const auto t=tail.load(std::memory_order_relaxed);if(t==head.load(std::memory_order_acquire))return false;p=data[t%Capacity];tail.store(t+1,std::memory_order_release);return true;}
    size_t size()const{return head.load(std::memory_order_acquire)-tail.load(std::memory_order_acquire);}
};
struct CapturePort {
    Queue<16> output;
    std::atomic<uint64_t> handler{0},epoch{0};
    std::atomic<int> requestedChannels{1},loadedModel{0},loadedChannels{0};
    uint64_t lastRevision=0;
    DWORD lastUsed=0;
    int inPosition=0,outPosition=kFrame;
    unsigned misses=0;
    bool bypassed=true,primed=false;
    Packet pending,current;
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
        if(c<1||c>2||(m!=1&&m!=2))throw std::runtime_error("unsupported denoise stream");
        try{
            if(model==1)sherpa=std::make_unique<Sherpa>(directory);
            for(int i=0;i<channels;++i){auto& ch=state[i];
                if(model==2){ch.rn=rnnoise_create(nullptr);if(!ch.rn)throw std::bad_alloc();}
                else {
                    SherpaOnnxOnlineSpeechDenoiserConfig cfg{};
                    const auto file=WideToUtf8(directory+L"\\gtcrn_simple.onnx");
                    cfg.model.gtcrn.model=file.c_str();cfg.model.num_threads=1;cfg.model.provider="cpu";
                    ch.gt=sherpa->SherpaOnnxCreateOnlineSpeechDenoiserFn(&cfg);
                    ch.up=sherpa->SherpaOnnxCreateLinearResamplerFn(16000,48000,7200,6);
                    if(!ch.gt||!ch.up)throw std::runtime_error("GTCRN initialization failed");
                }
            }
            reset();
        }catch(...){destroy();throw;}
    }
    void destroy(){for(auto& ch:state){if(ch.rn){rnnoise_destroy(ch.rn);ch.rn=nullptr;}if(ch.gt){sherpa->SherpaOnnxDestroyOnlineSpeechDenoiserFn(ch.gt);ch.gt=nullptr;}if(ch.up){sherpa->SherpaOnnxDestroyLinearResamplerFn(ch.up);ch.up=nullptr;}}}
    ~Impl(){destroy();}
    void reset(){for(int i=0;i<channels;++i){auto& ch=state[i];ch.wet.clear();ch.dry.clear();ch.strength.clear();ch.amount=-1;
        if(model==2){rnnoise_init(ch.rn,nullptr);ch.dry.insert(ch.dry.end(),960,0.0f);ch.strength.insert(ch.strength.end(),960,1.0f);}
        else {sherpa->SherpaOnnxOnlineSpeechDenoiserResetFn(ch.gt);sherpa->SherpaOnnxLinearResamplerResetFn(ch.up);}
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
            const auto* enhanced=impl_->sherpa->SherpaOnnxOnlineSpeechDenoiserRunFn(ch.gt,input.data(),frames,48000);
            if(enhanced){
                if(enhanced->sample_rate!=16000){impl_->sherpa->SherpaOnnxDestroyDenoisedAudioFn(enhanced);throw std::runtime_error("unexpected GTCRN sample rate");}
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
    c->model=std::clamp(model,0,2);
    c->strength=std::clamp(strength,0,100);
    control=std::move(c);auto* ptr=control.get();
    try{ptr->worker=std::thread([ptr]{
        struct Lane {std::unique_ptr<DenoiseStream> stream;uint64_t epoch=0;int model=0,channels=0;std::array<float,kFrame*kMaxChannels> assembled{};size_t count=0;};
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
                if(in.epoch!=lane.epoch){lane.stream->Reset();lane.epoch=in.epoch;lane.count=0;}
                lane.stream->Feed(in.audio.data(),kFrame,in.strength/100.0f);
                Packet out;out.epoch=in.epoch;out.model=in.model;out.channels=in.channels;out.slot=in.slot;
                // Partial reads are retained until a complete TS capture block exists.
                for(;;){
                    lane.count+=lane.stream->Read(lane.assembled.data()+lane.count*lane.channels,kFrame-lane.count);
                    if(lane.count<kFrame)break;
                    std::copy_n(lane.assembled.data(),kFrame*lane.channels,out.audio.data());lane.count=0;
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
    if(!control||model<0||model>2||strength<0||strength>100)return false;
    const int old=control->model.exchange(model);
    control->strength=strength;control->failure=false;control->overloads=0;
    if(old!=model)control->revision.fetch_add(1);
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
bool ProcessDenoise(uint64_t handler,short* samples,int frames,int channels){
    auto* c=control.get();if(!c||!samples||frames<1||frames>8192||channels<1||channels>2)return false;
    const int model=c->model.load(std::memory_order_acquire),strength=c->strength.load(std::memory_order_relaxed);
    if(!handler)return false;
    int slot=-1;
    for(int i=0;i<4;++i)if(c->ports[i].handler.load()==handler){slot=i;break;}
    if(slot<0){
        for(int i=0;i<4;++i)if(c->ports[i].handler.load()==0){slot=i;break;}
        if(slot<0){
            const DWORD now=GetTickCount();
            for(int i=0;i<4;++i)if(now-c->ports[i].lastUsed>2000){slot=i;break;}
            // Additional simultaneous capture streams keep their original audio.
            if(slot<0)return false;
        }
        auto& port=c->ports[slot];port.epoch.fetch_add(1);port.handler=handler;port.bypassed=true;
    }
    auto& p=c->ports[slot];p.lastUsed=GetTickCount();p.requestedChannels=channels;
    if(model==0||strength==0||p.loadedModel!=model||p.loadedChannels!=channels||c->failure){p.bypassed=true;return false;}
    const auto revision=c->revision.load(std::memory_order_acquire);
    if(p.bypassed||revision!=p.lastRevision){
        p.epoch.fetch_add(1);p.lastRevision=revision;p.inPosition=0;p.outPosition=kFrame;p.bypassed=false;p.primed=false;p.misses=0;
        Packet stale;while(p.output.pop(stale)){}
    }
    for(int i=0;i<frames;++i){
        for(int ch=0;ch<channels;++ch)p.pending.audio[p.inPosition*channels+ch]=samples[i*channels+ch]/32768.0f;
        ++p.inPosition;
        if(p.inPosition==kFrame){p.pending.epoch=p.epoch.load();p.pending.model=model;p.pending.channels=channels;p.pending.strength=strength;p.pending.slot=slot;if(!c->input.push(p.pending)){c->overloads++;c->failure=true;p.bypassed=true;return i>0;}p.inPosition=0;SetEvent(c->wake);}
        if(!p.primed&&p.output.size()>=4)p.primed=true;
        if(p.primed&&p.outPosition==kFrame){
            while(p.output.pop(p.current)){if(p.current.epoch==p.epoch.load()&&p.current.channels==channels){p.outPosition=0;p.misses=0;break;}}
            // Once streaming has started, a missed inference deadline must not
            // turn microphone speech into silent blocks. Preserve the original
            // remainder; repeated missed deadlines disable inference safely.
            if(p.outPosition==kFrame){c->overloads++;if(++p.misses>=3){c->failure=true;p.bypassed=true;}return i>0;}
        }
        for(int ch=0;ch<channels;++ch){float value=p.outPosition<kFrame?p.current.audio[p.outPosition*channels+ch]:0.0f;samples[i*channels+ch]=static_cast<short>(std::clamp(static_cast<int>(std::lrint(value*32768.0f)),-32768,32767));}
        if(p.outPosition<kFrame)++p.outPosition;
    }
    return true;
}
}
