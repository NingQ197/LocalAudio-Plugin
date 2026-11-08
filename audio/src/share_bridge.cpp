#include "share_bridge.h"
#include "common.h"
#include "app_scanner.h"
#include "denoise_engine.h"
#include "process_loopback.h"
#include "settings.h"
#include "teamspeak/public_definitions.h"
#include "teamspeak/public_errors.h"
#include "../../third_party/json/json.hpp"
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace la {
namespace {
using Json=nlohmann::json;
std::mutex bridgeMutex;
TS3Functions functions{};
std::wstring directory;
bool initialized=false;
ShareJsonEmitter emitter;
std::string previousContext,previousAudio,previousDenoise;
std::wstring ConfigPath(){std::lock_guard<std::mutex> lock(bridgeMutex);return directory+L"\\ts3-localaudio.ini";}
void Emit(const Json& message){ShareJsonEmitter sink;{std::lock_guard<std::mutex> lock(bridgeMutex);sink=emitter;}if(sink)sink(message.dump());}
void Error(const char* message){Emit({{"type","error"},{"message",message}});}
int Integer(const Json& object,const char* key,int fallback,int low,int high){
    auto value=object.find(key);if(value==object.end())return fallback;
    if(!value->is_number_integer())throw std::runtime_error("invalid integer");
    const auto number=value->get<int64_t>();if(number<low||number>high)throw std::runtime_error("integer out of range");return static_cast<int>(number);
}
bool Boolean(const Json& object,const char* key,bool fallback){auto v=object.find(key);if(v==object.end())return fallback;if(!v->is_boolean())throw std::runtime_error("invalid boolean");return v->get<bool>();}
std::string Text(const Json& object,const char* key,size_t limit){auto v=object.find(key);if(v==object.end())return {};if(!v->is_string())throw std::runtime_error("invalid text");auto text=v->get<std::string>();if(text.size()>limit)throw std::runtime_error("text too long");return text;}
Json AudioSettings(){
    const auto s=LoadSettings(ConfigPath());
    return {{"exe",WideToUtf8(s.targetExe)},{"label",WideToUtf8(s.targetLabel)},{"targetExe",WideToUtf8(s.targetExe)},{"targetLabel",WideToUtf8(s.targetLabel)},
      {"sampleRate",s.sampleRate},{"bitDepth",s.bitDepth},{"volume",s.volume},{"forceSend",s.forceSend},{"keepMicActive",s.keepMicActive},{"autoStart",s.autoStart},
      {"denoiseModel",s.denoiseModel},{"denoiseStrength",s.denoiseStrength}};
}
Json Bootstrap(){
    TS3Functions api{};bool active=false;{std::lock_guard<std::mutex> lock(bridgeMutex);api=functions;active=initialized;}
    uint64 handler=0,channel=0;anyID self=0;int status=0;
    if(active&&api.getCurrentServerConnectionHandlerID)handler=api.getCurrentServerConnectionHandlerID();
    bool connected=handler&&api.getConnectionStatus&&api.getConnectionStatus(handler,&status)==ERROR_ok&&status==STATUS_CONNECTION_ESTABLISHED;
    if(connected&&api.getClientID)api.getClientID(handler,&self);
    if(connected&&self&&api.getChannelOfClient)api.getChannelOfClient(handler,self,&channel);
    connected=connected&&self&&channel;
    return {{"type","bootstrap"},{"version","2.5.4"},{"features",{"music","ai-denoise"}},{"connected",connected},{"selfID",self},{"handler",handler},{"channelID",channel},{"audioSettings",AudioSettings()}};
}
bool Changed(std::string& previous,const Json& value){std::lock_guard<std::mutex> lock(bridgeMutex);auto text=value.dump();if(text==previous)return false;previous=std::move(text);return true;}
}
void AudioBridgeInitialize(const TS3Functions& api,const std::wstring& configDirectory){std::lock_guard<std::mutex> lock(bridgeMutex);functions=api;directory=configDirectory;initialized=true;previousContext.clear();previousAudio.clear();previousDenoise.clear();}
void AudioBridgeShutdown(){std::lock_guard<std::mutex> lock(bridgeMutex);initialized=false;functions={};emitter={};previousContext.clear();previousAudio.clear();previousDenoise.clear();}
void ShareBridgeSetEmitter(ShareJsonEmitter next){std::lock_guard<std::mutex> lock(bridgeMutex);emitter=std::move(next);previousContext.clear();previousAudio.clear();previousDenoise.clear();}
void ShareBridgeWindowClosed(){ShareBridgeSetEmitter({});}
std::string ShareBridgeContextJson(){return Bootstrap().dump();}
void ShareBridgeTick(){
    const auto context=Bootstrap();if(Changed(previousContext,context))Emit(context);
    const Json audio={{"type","audioStatus"},{"running",CaptureController::Get().Running()},{"status",WideToUtf8(State().GetStatus())}};
    if(Changed(previousAudio,audio))Emit(audio);
    const auto config=GetDenoiseConfig();
    const Json denoise={{"type","denoiseStatus"},{"model",config.model},{"strength",config.strength},{"status",WideToUtf8(DenoiseStatus())}};
    if(Changed(previousDenoise,denoise))Emit(denoise);
}
void ShareBridgeHandleMessage(const std::string& text){
    try{
        if(text.empty()||text.size()>256*1024){Error("请求过大或为空");return;}
        auto message=Json::parse(text,[](int depth,Json::parse_event_t,Json&){if(depth>16)throw std::runtime_error("request too deep");return true;});
        if(!message.is_object())throw std::runtime_error("expected object");
        const auto type=Text(message,"type",64);
        if(type=="ready"||type=="getContext"){Emit(Bootstrap());ShareBridgeTick();}
        else if(type=="scanApps"){
            auto apps=Json::array();for(const auto& app:ScanApps())apps.push_back({{"exe",WideToUtf8(app.exe)},{"display",WideToUtf8(app.display)},{"pid",app.pid},{"playing",app.playing},{"icon",app.icon}});
            Emit({{"type","apps"},{"apps",apps}});
        }else if(type=="audioSave"||type=="audioStart"){
            const auto path=ConfigPath();auto settings=LoadSettings(path);
            auto exe=Text(message,"exe",1040),label=Text(message,"label",2048);
            if(exe.empty())exe=Text(message,"targetExe",1040);if(label.empty())label=Text(message,"targetLabel",2048);
            auto wide=ToLower(Utf8ToWide(exe));
            if(wide.size()<5||wide.size()>260||wide.substr(wide.size()-4)!=L".exe"||exe.find_first_of("\\/:*?\"<>|")!=std::string::npos||std::any_of(exe.begin(),exe.end(),[](unsigned char c){return c<32||c==127;})){Error("请选择应用程序或填写有效的 .exe 名称");return;}
            settings.targetExe=std::move(wide);settings.targetLabel=label.empty()?settings.targetExe:Utf8ToWide(label);
            settings.volume=Integer(message,"volume",settings.volume,0,200);
            settings.sampleRate=Integer(message,"sampleRate",settings.sampleRate,0,96000);settings.bitDepth=Integer(message,"bitDepth",settings.bitDepth,0,32);
            if((settings.sampleRate!=0&&settings.sampleRate!=44100&&settings.sampleRate!=48000&&settings.sampleRate!=96000)||(settings.bitDepth!=0&&settings.bitDepth!=16&&settings.bitDepth!=24&&settings.bitDepth!=32)){Error("不支持的音频格式");return;}
            settings.keepMicActive=Boolean(message,"keepMicActive",settings.keepMicActive);settings.forceSend=Boolean(message,"forceSend",settings.forceSend);
            if(!SaveAudioSettings(path,settings)){Error("无法保存音频设置");return;}
            CaptureController::Get().ApplySettings(settings);
            if(type=="audioStart"&&!CaptureController::Get().Start())Error("无法启动应用音频采集");
            ShareBridgeTick();
        }else if(type=="audioStop"){CaptureController::Get().Stop();ShareBridgeTick();}
        else if(type=="denoiseSave"){
            const int model=Integer(message,"model",0,0,2),strength=Integer(message,"strength",70,0,100);
            if(!SaveDenoiseSettings(ConfigPath(),model,strength)){Error("无法保存降噪设置");return;}
            if(!SetDenoiseConfig(model,strength))Error("降噪组件未就绪，设置已保存");ShareBridgeTick();
        }else Error("此版本不支持此操作");
    }catch(...){Error("无效请求，设置未修改");}
}
}
