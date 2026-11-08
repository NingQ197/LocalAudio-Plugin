#include "share_bridge.h"
#include "common.h"
#include "app_scanner.h"
#include "denoise_engine.h"
#include "process_loopback.h"
#include "screen_audio.h"
#include "settings.h"
#include "default_turn_credentials.h"
#include "teamspeak/public_errors.h"
#include "teamspeak/public_rare_definitions.h"
#include <objbase.h>
#include <cctype>
#include "../../third_party/json/json.hpp"
#include <algorithm>
#include <chrono>
#include <deque>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>
#include <wincrypt.h>
#include <bcrypt.h>
#include <windns.h>

namespace la {
namespace {
using Json = nlohmann::json;
constexpr size_t kMaxSignal = 128 * 1024;
constexpr size_t kMaxEncoded = ((kMaxSignal + 2) / 3) * 4;
constexpr size_t kChunk = 4800;
constexpr const char* kPrefix = "TS3LA2:";
std::mutex bridgeMutex;
TS3Functions functions{};
bool initialized = false;
bool autoplayPermitted = false;
std::string pluginID;
std::wstring directory;
ShareJsonEmitter emitter;
std::string previousContext, previousAudio, previousScreen, previousDenoise;
std::string activeSession;
uint64 activeHandler = 0, activeChannel = 0;
uint64 lastHandler = 0, lastChannel = 0;
struct Context { TS3Functions api{}; uint64 handler=0, channel=0; anyID self=0; bool connected=false; };
struct Assembly { size_t count=0, bytes=0, have=0; uint64 created=0; std::vector<std::string> fragments; };
struct Budget { uint64 since=0; size_t packets=0, bytes=0; };
std::map<std::string,Assembly> assemblies;
std::map<std::string,uint64> completed;
std::map<anyID,Budget> budgets;
Budget sendBudget;
struct SrvRecord { WORD priority=0, weight=0, port=0; std::string target; };
using SrvQuery = std::function<std::vector<SrvRecord>(const std::wstring&)>;
std::vector<SrvRecord> QuerySrv(const std::wstring& name) {
    PDNS_RECORDW records=nullptr;
    const DNS_STATUS status=DnsQuery_W(name.c_str(),DNS_TYPE_SRV,DNS_QUERY_STANDARD,nullptr,&records,nullptr);
    std::vector<SrvRecord> result;
    if(status==0) {
        for(PDNS_RECORDW record=records;record;record=record->pNext) {
            if(record->wType!=DNS_TYPE_SRV)continue;
            const auto& srv=record->Data.SRV;
            if(!srv.pNameTarget || !*srv.pNameTarget || wcscmp(srv.pNameTarget,L".")==0)continue;
            std::wstring target=srv.pNameTarget;
            while(!target.empty()&&target.back()==L'.')target.pop_back();
            std::string utf8=WideToUtf8(target);
            if(utf8.empty()||utf8.find_first_of(" /\\\r\n\t")!=std::string::npos)continue;
            result.push_back({srv.wPriority,srv.wWeight,srv.wPort,std::move(utf8)});
        }
    }
    if(records)DnsRecordListFree(records,DnsFreeRecordList);
    return result;
}
SrvQuery srvQuery=QuerySrv;
std::string srvCacheInput,srvCacheOutput;
uint64 srvCacheTick=0;
std::mutex srvCacheMutex;

uint64 Now() { return GetTickCount64(); }
int64_t UnixSeconds() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
void Emit(const Json& message) {
    ShareJsonEmitter target;
    { std::lock_guard<std::mutex> lock(bridgeMutex); target=emitter; }
    if (target) target(message.dump(-1, ' ', false, Json::error_handler_t::replace));
}
void Error(const std::string& message) { Emit({{"type","error"},{"message",message}}); }
Json Parse(const std::string& text, size_t limit) {
    if (text.size()>limit) return {};
    int depth=0; bool quoted=false,escape=false;
    for (char c:text) {
        if (quoted) { if (escape) escape=false; else if(c=='\\') escape=true; else if(c=='"') quoted=false; }
        else if(c=='"') quoted=true;
        else if(c=='{' || c=='[') { if(++depth>32) return {}; }
        else if(c=='}' || c==']') { if(--depth<0) return {}; }
    }
    auto result=Json::parse(text,nullptr,false);
    return result.is_object() ? result : Json{};
}
std::string Text(const Json& object, const char* field, size_t max=4096) {
    auto at=object.find(field); if(at==object.end() || !at->is_string()) return {};
    std::string value=at->get<std::string>(); return value.size()<=max ? value : std::string{};
}
bool Bool(const Json& j,const char* key,bool fallback) { auto it=j.find(key); return it!=j.end() && it->is_boolean() ? it->get<bool>() : fallback; }
int Int(const Json& j,const char* key,int fallback,int low,int high) {
    auto it=j.find(key); if(it==j.end() || !it->is_number_integer()) return fallback;
    int64_t value=it->get<int64_t>(); return static_cast<int>(std::max<int64_t>(low,std::min<int64_t>(high,value)));
}
Context Current() {
    Context c;
    { std::lock_guard<std::mutex> lock(bridgeMutex); if(!initialized)return c; c.api=functions; }
    if(!c.api.getCurrentServerConnectionHandlerID || !c.api.getConnectionStatus || !c.api.getClientID || !c.api.getChannelOfClient) return c;
    c.handler=c.api.getCurrentServerConnectionHandlerID(); int status=0;
    if(c.handler && c.api.getConnectionStatus(c.handler,&status)==ERROR_ok && status>=STATUS_CONNECTION_ESTABLISHED &&
       c.api.getClientID(c.handler,&c.self)==ERROR_ok && c.api.getChannelOfClient(c.handler,c.self,&c.channel)==ERROR_ok)
        c.connected=c.self && c.channel;
    return c;
}
std::string ClientUid(const Context& c,anyID client) {
    if(!c.api.getClientVariableAsString||!c.api.freeMemory||!client)return {};
    char* value=nullptr;
    const auto status=c.api.getClientVariableAsString(c.handler,client,CLIENT_UNIQUE_IDENTIFIER,&value);
    std::string uid;
    if(status==ERROR_ok&&value){const size_t length=strnlen(value,257);if(length&&length<=256)uid.assign(value,length);}
    if(value)c.api.freeMemory(value);
    if(std::any_of(uid.begin(),uid.end(),[](unsigned char ch){return ch<=32||ch==127;}))return {};
    return uid;
}
std::wstring ConfigPath(const wchar_t* file) { std::lock_guard<std::mutex> lock(bridgeMutex); return directory+L"\\"+file; }
std::string Base64(const BYTE* bytes,size_t n) {
    static const char* chars="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; out.reserve(((n+2)/3)*4);
    for(size_t i=0;i<n;i+=3) {
        uint32_t value=(uint32_t)bytes[i]<<16;
        if(i+1<n)value|=(uint32_t)bytes[i+1]<<8;
        if(i+2<n)value|=bytes[i+2];
        out+=chars[(value>>18)&63];out+=chars[(value>>12)&63];out+=i+1<n?chars[(value>>6)&63]:'=';out+=i+2<n?chars[value&63]:'=';
    }
    return out;
}
bool Unbase64(const std::string& text,std::string& out) {
    if(text.size()%4 || text.size()>kMaxEncoded+4096)return false;
    auto value=[](char c)->int { if(c>='A'&&c<='Z')return c-'A';if(c>='a'&&c<='z')return c-'a'+26;if(c>='0'&&c<='9')return c-'0'+52;if(c=='+')return 62;if(c=='/')return 63;return -1; };
    out.clear();out.reserve(text.size()/4*3);
    for(size_t i=0;i<text.size();i+=4) {
        int a=value(text[i]),b=value(text[i+1]),c=text[i+2]=='='?0:value(text[i+2]),d=text[i+3]=='='?0:value(text[i+3]);
        if(a<0||b<0||c<0||d<0)return false;
        bool pad2=text[i+2]=='=',pad1=text[i+3]=='=';
        if((pad2&&!pad1)||((pad1||pad2)&&i+4!=text.size()))return false;
        uint32_t v=(a<<18)|(b<<12)|(c<<6)|d;
        out+=(char)(v>>16);if(!pad2)out+=(char)(v>>8);if(!pad1)out+=(char)v;
    }
    return true;
}
std::string SelectSrvTarget(const std::vector<SrvRecord>& records,uint64 entropy) {
    if(records.empty())return {};
    WORD priority=std::numeric_limits<WORD>::max();
    for(const auto& record:records)if(!record.target.empty())priority=std::min(priority,record.priority);
    std::vector<const SrvRecord*> eligible;
    uint64 totalWeight=0;
    for(const auto& record:records)if(!record.target.empty()&&record.priority==priority) {
        eligible.push_back(&record);totalWeight+=record.weight;
    }
    if(eligible.empty())return {};
    if(totalWeight) {
        std::stable_sort(eligible.begin(),eligible.end(),[](const SrvRecord* a,const SrvRecord* b){return a->weight<b->weight;});
        uint64 pick=entropy%(totalWeight+1);
        uint64 cumulative=0;
        for(const auto* record:eligible) {
            cumulative+=record->weight;
            if(cumulative>=pick)return record->target;
        }
    }
    return eligible[static_cast<size_t>(entropy%eligible.size())]->target;
}
bool IsIpLiteral(const std::string& host) {
    if(host.empty())return false;
    if(host.front()=='['&&host.back()==']')return true;
    return host.find(':')!=std::string::npos || std::all_of(host.begin(),host.end(),[](unsigned char c){return (c>='0'&&c<='9')||c=='.';});
}
std::string ResolveSrvHost(const std::string& input) {
    if(input.empty()||IsIpLiteral(input)||!srvQuery)return input;
    std::lock_guard<std::mutex> lock(srvCacheMutex);
    const uint64 now=Now();
    if(input==srvCacheInput&&now-srvCacheTick<300000)return srvCacheOutput;
    std::string resolved=input;
    std::wstring query=L"_ts3._udp."+Utf8ToWide(input);
    const auto candidates=srvQuery(query);
    const auto chosen=SelectSrvTarget(candidates,now);
    if(!chosen.empty())resolved=chosen;
    srvCacheInput=input;srvCacheOutput=resolved;srvCacheTick=now;
    return resolved;
}
// CONNECTION_SERVER_IP is the server address supplied to TeamSpeak; it can
// still be the SRV alias, so resolve TeamSpeak's _ts3._udp record before using
// it for the relay default. The TURN service port remains the requested 21113.
std::string RelayHost() {
    auto c=Current(); char* address=nullptr; std::string host;
    if(c.connected && c.api.getConnectionVariableAsString &&
       c.api.getConnectionVariableAsString(c.handler,c.self,CONNECTION_SERVER_IP,&address)==ERROR_ok && address) {
        host=address; if(c.api.freeMemory)c.api.freeMemory(address);
    }
    if(host.find_first_of(" /\\\r\n\t")!=std::string::npos)return {};
    host=ResolveSrvHost(host);
    if(host.find(':')!=std::string::npos && host.front()!='[')host="["+host+"]";
    return host;
}
Json DefaultNetwork() { return {{"stunUrls",Json::array()},{"turnUrls",Json::array()},{"turnUsername",LOCAL_AUDIO_DEFAULT_TURN_USERNAME},{"turnCredential",LOCAL_AUDIO_DEFAULT_TURN_CREDENTIAL},{"iceTransportPolicy","all"},{"maxViewers",4}}; }
bool Network(const Json& input,Json& result) {
    if(!input.is_object())return false;result=DefaultNetwork();
    for(const char* name:{"stunUrls","turnUrls"}) {
        auto at=input.find(name);if(at==input.end())continue;
        if(!at->is_array()||at->size()>8)return false;
        for(const auto& item:*at) {
            if(!item.is_string())return false;auto url=item.get<std::string>();
            if(url.empty()||url.size()>1024||url.find_first_of(" \t\r\n")!=std::string::npos)return false;
            bool stun=std::string(name)=="stunUrls";
            if(stun ? url.rfind("stun:",0)!=0&&url.rfind("stuns:",0)!=0 : url.rfind("turn:",0)!=0&&url.rfind("turns:",0)!=0)return false;
            result[name].push_back(url);
        }
    }
    auto username=Text(input,"turnUsername",1024),credential=Text(input,"turnCredential",2048);
    if(!username.empty()||!credential.empty()) {
        // Keep user-supplied REST credentials (including their expiry) intact.
        // Never combine a partial custom pair with the bundled default pair.
        result["turnUsername"]=std::move(username);result["turnCredential"]=std::move(credential);
    } else if(!result["turnUrls"].empty()) {
        // A manually entered TURN endpoint must not receive this build's
        // bundled credential. Leave it unauthenticated until configured.
        result["turnUsername"]="";result["turnCredential"]="";
    }
    auto policy=Text(input,"iceTransportPolicy",16);if(!policy.empty()&&policy!="all"&&policy!="relay")return false;
    if(!policy.empty())result["iceTransportPolicy"]=policy;
    result["maxViewers"]=Int(input,"maxViewers",4,1,6);return true;
}
// The scope is computed here, never accepted from a WebView request. Include
// the complete configured TURN endpoint set; only automatic mode follows TS.
std::string CredentialScope(const Json& network,const std::string& automaticHost) {
    std::set<std::string> endpoints;
    auto at=network.find("turnUrls");
    if(at!=network.end()&&at->is_array())for(const auto& item:*at)if(item.is_string())endpoints.insert(item.get<std::string>());
    const bool automatic=endpoints.empty();
    if(automatic){if(automaticHost.empty())return {};endpoints.insert("turn:"+automaticHost+":21113?transport=udp");}
    const std::string input=Json({{"v",1},{"mode",automatic?"automatic":"fixed"},{"endpoints",endpoints}}).dump();
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return {};
    unsigned char digest[32]={};
    const auto result=BCryptHash(algorithm,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),static_cast<ULONG>(input.size()),digest,sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm,0);if(result<0)return {};
    static const char* hex="0123456789abcdef";std::string scope="turn-scope-v1:";
    for(unsigned char value:digest){scope+=hex[value>>4];scope+=hex[value&15];}return scope;
}
Json ReadNetworkFile() {
    std::ifstream file(ConfigPath(L"ts3-share-network.json").c_str(),std::ios::binary);
    if(!file)return Json::object();file.seekg(0,std::ios::end);auto size=file.tellg();if(size<0||size>32768)return Json::object();file.seekg(0);
    std::string text((std::istreambuf_iterator<char>(file)),{});return Parse(text,32768);
}
struct StoredCredential {bool present=false,scoped=false;std::string username,credential,scope;};
StoredCredential ReadStoredCredential(const Json& stored) {
    StoredCredential result;
    // Recognize legacy plaintext/DPAPI-password formats only to invalidate them
    // and to prevent an unchanged form value being rebound without user input.
    result.username=Text(stored,"turnUsername",1024);result.credential=Text(stored,"turnCredential",2048);
    result.present=!result.credential.empty();
    const auto encoded=Text(stored,"protectedCredential",32768);
    if(encoded.empty())return result;result.present=true;
    std::string encrypted;if(!Unbase64(encoded,encrypted))return result;
    DATA_BLOB in{static_cast<DWORD>(encrypted.size()),reinterpret_cast<BYTE*>(encrypted.data())},out{};
    if(!CryptUnprotectData(&in,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out))return result;
    const std::string plain(reinterpret_cast<char*>(out.pbData),out.cbData);
    SecureZeroMemory(out.pbData,out.cbData);LocalFree(out.pbData);
    const Json envelope=Parse(plain,32768);
    if(Text(envelope,"format",64)=="LocalAudio/TURN-v2") {
        result.username=Text(envelope,"username",1024);result.credential=Text(envelope,"credential",2048);result.scope=Text(envelope,"scope",128);
        result.scoped=!result.username.empty()&&!result.credential.empty()&&!result.scope.empty();
    } else if(plain.size()<=2048)result.credential=plain;
    return result;
}
Json LoadNetwork(const std::string& relayHost) {
    const Json stored=ReadNetworkFile();Json cleaned;if(!Network(stored,cleaned))cleaned=DefaultNetwork();
    const auto credential=ReadStoredCredential(stored);const auto expected=CredentialScope(cleaned,relayHost);
    const bool valid=credential.scoped&&!expected.empty()&&credential.scope==expected&&credential.username==Text(cleaned,"turnUsername",1024);
    cleaned["turnCredential"]="";
    if(valid)cleaned["turnCredential"]=credential.credential;
    else if(credential.present)cleaned["turnUsername"]="";
    const bool reset=credential.present&&!valid;
    cleaned["credentialScopeInvalid"]=reset||Bool(stored,"credentialResetRequired",false);
    cleaned["credentialScopeReason"]=reset?(credential.scoped?"destination-changed":"legacy-unscoped"):(Bool(stored,"credentialResetRequired",false)?"reenter-required":"");
    return cleaned;
}
Json LoadNetwork(){const auto relayHost=RelayHost();return LoadNetwork(relayHost);}
bool SaveNetwork(Json network,bool credentialEdited=false,const std::string& observedTargetScope={},bool* targetChanged=nullptr) {
    if(targetChanged)*targetChanged=false;
    Json clean;if(!Network(network,clean))return false;network=std::move(clean);
    std::string credential=Text(network,"turnCredential",2048),username=Text(network,"turnUsername",1024);
    const auto previousStored=ReadNetworkFile();const auto previous=ReadStoredCredential(previousStored);
    const bool automatic=network["turnUrls"].empty();
    const auto relayHost=automatic?RelayHost():std::string{};
    const auto scope=CredentialScope(network,relayHost);
    // This marker only detects a stale UI observation. It cannot select the
    // protected scope: the native destination above is independently computed.
    // Reject before writing so legacy/previous credentials remain recoverable.
    if(automatic&&!credential.empty()&&(scope.empty()||observedTargetScope!=scope)){
        if(targetChanged)*targetChanged=true;return false;
    }
    // A visible old password carried across a URL edit is not fresh consent to
    // use it at a different service. The UI marks an actual password input; a
    // missing marker fails closed and asks for one explicit re-entry.
    const bool rebind=previous.present&&!previous.credential.empty()&&credential==previous.credential&&previous.scope!=scope&&!credentialEdited;
    const bool reset=!credential.empty()&&(scope.empty()||rebind);
    if(reset){credential.clear();username.clear();network["turnUsername"]="";}
    network.erase("turnCredential");network["credentialResetRequired"]=reset;
    // A fail-closed Bootstrap deliberately shows empty credential fields. An
    // unrelated settings save must not erase that recoverable old credential.
    // Only an actual password edit clears/replaces it; preserving an old blob
    // never grants it the newly computed scope.
    const bool preserve=previous.present&&!credentialEdited&&(credential.empty()||reset);
    const std::string savedScope=preserve?previous.scope:scope;
    if(preserve){
        credential=previous.credential;username=previous.username;network["turnUsername"]=username;
        network["credentialResetRequired"]=!previous.scoped;
        const auto encoded=Text(previousStored,"protectedCredential",32768);
        if(!encoded.empty())network["protectedCredential"]=encoded;
    }
    if(!network.contains("protectedCredential")&&!credential.empty()&&(preserve||!username.empty())) {
        const std::string payload=Json({{"format","LocalAudio/TURN-v2"},{"username",username},{"credential",credential},{"scope",savedScope}}).dump();
        DATA_BLOB in{static_cast<DWORD>(payload.size()),reinterpret_cast<BYTE*>(const_cast<char*>(payload.data()))},out{};
        if(!CryptProtectData(&in,L"LocalAudio scoped TURN credential",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&out))return false;
        network["protectedCredential"]=Base64(out.pbData,out.cbData);LocalFree(out.pbData);
    }
    auto path=ConfigPath(L"ts3-share-network.json"),tmp=path+L".tmp";
    const std::string text=network.dump(2);if(text.size()>32768)return false;
    std::ofstream file(tmp.c_str(),std::ios::binary|std::ios::trunc);if(!file)return false;
    file.write(text.data(),text.size());file.close();
    if(!file||!MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(tmp.c_str());return false;}return true;
}
Json EffectiveNetwork() {
    // One destination snapshot must govern both authentication and URL expansion.
    const auto host=RelayHost();auto n=LoadNetwork(host);
    n["automaticStun"]=n["stunUrls"].empty(); n["automaticTurn"]=n["turnUrls"].empty();
    if(!host.empty()) {
        if(n["stunUrls"].empty()) n["stunUrls"].push_back("stun:"+host+":21113");
        if(n["turnUrls"].empty()) n["turnUrls"].push_back("turn:"+host+":21113?transport=udp");
    }
    n["automaticHost"]=host;
    n["observedTargetScope"]=CredentialScope(Json{{"turnUrls",Json::array()}},host);
    return n;
}
Json AudioSettings() {
    auto s=LoadSettings(ConfigPath(L"ts3-localaudio.ini"));
    return {{"exe",WideToUtf8(s.targetExe)},{"label",WideToUtf8(s.targetLabel)},{"targetExe",WideToUtf8(s.targetExe)},{"targetLabel",WideToUtf8(s.targetLabel)},
      {"sampleRate",s.sampleRate},{"bitDepth",s.bitDepth},{"volume",s.volume},{"forceSend",s.forceSend},{"keepMicActive",s.keepMicActive},{"autoStart",s.autoStart},
      {"denoiseModel",s.denoiseModel},{"denoiseStrength",s.denoiseStrength}};
}
Json Bootstrap() {
    Context c=Current();Json peers=Json::array();
    if(c.connected&&c.api.getChannelClientList) {
        anyID* ids=nullptr;
        if(c.api.getChannelClientList(c.handler,c.channel,&ids)==ERROR_ok&&ids) {
            for(size_t i=0;ids[i]&&i<1024;++i) {
                if(ids[i]==c.self)continue;const auto uid=ClientUid(c,ids[i]);if(uid.empty())continue;
                char* name=nullptr;std::string label="Client "+std::to_string(ids[i]);
                if(c.api.getClientVariableAsString&&c.api.getClientVariableAsString(c.handler,ids[i],CLIENT_NICKNAME,&name)==ERROR_ok&&name) { label=std::string(name).substr(0,512);if(c.api.freeMemory)c.api.freeMemory(name); }
                peers.push_back({{"id",ids[i]},{"name",label},{"uid",uid}});
            }
            if(c.api.freeMemory)c.api.freeMemory(ids);
        }
    }
    return {{"type","bootstrap"},{"version","2.5.4"},{"protocolMajor",2},{"connected",c.connected},{"selfID",c.self},{"handler",c.handler},{"channelID",c.channel},
      {"peers",peers},{"audioSettings",AudioSettings()},{"networkConfig",EffectiveNetwork()}};
}
bool TakeBudget(Budget& budget,size_t bytes,size_t packets,size_t maxBytes) {
    auto now=Now();if(!budget.since||now-budget.since>10000)budget={now,0,0};
    if(++budget.packets>packets||budget.bytes+bytes>maxBytes)return false;budget.bytes+=bytes;return true;
}
void Prune() {
    uint64 now=Now();
    for(auto i=assemblies.begin();i!=assemblies.end();)if(now-i->second.created>15000)i=assemblies.erase(i);else ++i;
    for(auto i=completed.begin();i!=completed.end();)if(now-i->second>300000)i=completed.erase(i);else ++i;
    if(completed.size()>4096)completed.clear();
}
void SendSignal(const Json& payload,anyID to,const Context* overrideContext=nullptr) {
    Context c=overrideContext?*overrideContext:Current();if(!c.connected||!c.api.sendPluginCommand)return;
    std::string id;
    { std::lock_guard<std::mutex> lock(bridgeMutex);id=pluginID; }
    if(id.empty())return;
    if(to) { uint64 channel=0;if(!c.api.getChannelOfClient||c.api.getChannelOfClient(c.handler,to,&channel)!=ERROR_ok||channel!=c.channel||to==c.self)return; }
    std::string raw=payload.dump(-1,' ',false,Json::error_handler_t::replace);if(raw.size()>kMaxSignal){Error("屏幕信令超出 128 KiB 限制");return;}
    auto encoded=Base64(reinterpret_cast<const BYTE*>(raw.data()),raw.size());GUID guid{};if(FAILED(CoCreateGuid(&guid)))return;
    std::string nonce;const BYTE* guidBytes=reinterpret_cast<const BYTE*>(&guid);static const char* hex="0123456789abcdef";
    for(size_t i=0;i<sizeof(guid);++i){nonce+=hex[guidBytes[i]>>4];nonce+=hex[guidBytes[i]&15];}
    size_t count=(encoded.size()+kChunk-1)/kChunk;
    for(size_t i=0;i<count;++i) {
        Json packet={{"v",2},{"id",nonce},{"n",count},{"i",i},{"c",c.channel},{"to",to},{"ts",UnixSeconds()},{"d",encoded.substr(i*kChunk,kChunk)}};
        std::string command=kPrefix+packet.dump();if(command.size()>7000)return;
        { std::lock_guard<std::mutex> lock(bridgeMutex);if(!TakeBudget(sendBudget,command.size(),1000,4*1024*1024))return; }
        anyID targets[]={to,0};
        c.api.sendPluginCommand(c.handler,id.c_str(),command.c_str(),to?PluginCommandTarget_CLIENT:PluginCommandTarget_CURRENT_CHANNEL,to?targets:nullptr,nullptr);
    }
}
void StopShare(const char* reason) {
    Context old;std::string session;
    {std::lock_guard<std::mutex> lock(bridgeMutex);session=activeSession;old.api=functions;old.handler=activeHandler?activeHandler:lastHandler;old.channel=activeChannel?activeChannel:lastChannel;old.connected=old.handler&&old.channel;activeSession.clear();activeHandler=activeChannel=0;autoplayPermitted=false;}
    if(old.connected)SendSignal({{"v",1},{"kind","peer-leave"},{"channelID",old.channel},{"sessionID",session},{"reason",reason}},0,&old);
    ScreenAudioStop();Emit({{"type","stopScreenShare"},{"reason",reason}});
}
}
void TS3ScreenInitialize(const TS3Functions& api,const char* id,const std::wstring& configDirectory) {
    std::lock_guard<std::mutex> lock(bridgeMutex);functions=api;pluginID=id?id:"";directory=configDirectory;initialized=true;
    previousContext.clear();previousAudio.clear();previousScreen.clear();previousDenoise.clear();assemblies.clear();completed.clear();budgets.clear();lastHandler=lastChannel=0;
}
void TS3ScreenSetPluginID(const char* id) {std::lock_guard<std::mutex> lock(bridgeMutex);pluginID=id?id:"";}
void TS3ScreenShutdown() {StopShare("plugin-shutdown");std::lock_guard<std::mutex> lock(bridgeMutex);initialized=false;emitter={};assemblies.clear();completed.clear();budgets.clear();pluginID.clear();}
void ShareBridgeSetEmitter(ShareJsonEmitter sink) {std::lock_guard<std::mutex> lock(bridgeMutex);emitter=std::move(sink);previousContext.clear();previousAudio.clear();previousScreen.clear();previousDenoise.clear();}
std::string ShareBridgeContextJson() {return Bootstrap().dump();}
void TS3ScreenCurrentHandlerChange(uint64) {StopShare("connection-changed");ShareBridgeTick();}
void TS3ScreenPeerMembershipChanged(uint64 handler,anyID client) {
    const Context c=Current();if(!c.connected||handler!=c.handler||!client||client==c.self)return;
    const std::string prefix=std::to_string(handler)+":"+std::to_string(c.channel)+":"+std::to_string(client)+":";
    {std::lock_guard<std::mutex> lock(bridgeMutex);for(auto i=assemblies.begin();i!=assemblies.end();)if(i->first.rfind(prefix,0)==0)i=assemblies.erase(i);else ++i;}
    Emit({{"type","signal"},{"nativePeerRevoked",true},{"handler",handler},{"channelID",c.channel},{"from",client}});
    ShareBridgeTick();
}
void ShareBridgeWindowClosed() {StopShare("window-closed");}
bool ShareBridgeMayPlayRemoteAudio() {std::lock_guard<std::mutex> lock(bridgeMutex);return autoplayPermitted;}
void ShareBridgeTick() {
    Context c=Current();bool changed=false;
    {std::lock_guard<std::mutex> lock(bridgeMutex);changed=lastHandler!=c.handler||lastChannel!=c.channel;if(changed)assemblies.clear();Prune();}
    if(changed)StopShare("channel-changed");
    {std::lock_guard<std::mutex> lock(bridgeMutex);lastHandler=c.handler;lastChannel=c.channel;}
    Json context=Bootstrap();std::string contextText=context.dump();bool emitContext;
    {std::lock_guard<std::mutex> lock(bridgeMutex);emitContext=contextText!=previousContext;previousContext=contextText;}
    if(emitContext)Emit(context);
    Json audio={{"type","audioStatus"},{"running",CaptureController::Get().Running()},{"status",WideToUtf8(State().GetStatus())}};
    Json screen={{"type","screenAudioStatus"},{"running",ScreenAudioRunning()},{"status",WideToUtf8(ScreenAudioStatus())}};
    const auto denoiseConfig=GetDenoiseConfig();
    Json denoise={{"type","denoiseStatus"},{"model",denoiseConfig.model},{"strength",denoiseConfig.strength},{"status",WideToUtf8(DenoiseStatus())}};
    bool emitAudio,emitScreen,emitDenoise;
    {std::lock_guard<std::mutex> lock(bridgeMutex);emitAudio=audio.dump()!=previousAudio;previousAudio=audio.dump();emitScreen=screen.dump()!=previousScreen;previousScreen=screen.dump();emitDenoise=denoise.dump()!=previousDenoise;previousDenoise=denoise.dump();}
    if(emitAudio)Emit(audio);if(emitScreen)Emit(screen);if(emitDenoise)Emit(denoise);
}
void ShareBridgeHandleMessage(const std::string& text) {
    try {
        Json m=Parse(text,256*1024);std::string type=Text(m,"type",64);
        if(type=="ready"||type=="getContext"){Emit(Bootstrap());ShareBridgeTick();}
        else if(type=="scanApps") {Json apps=Json::array();for(const auto& app:ScanApps())apps.push_back({{"exe",WideToUtf8(app.exe)},{"display",WideToUtf8(app.display)},{"pid",app.pid},{"playing",app.playing},{"icon",app.icon}});Emit({{"type","apps"},{"apps",apps}});}
        else if(type=="audioStart"||type=="audioSave") {
            auto s=LoadSettings(ConfigPath(L"ts3-localaudio.ini"));std::string exe=Text(m,"exe",260),label=Text(m,"label",512);
            if(exe.empty())exe=Text(m,"targetExe",260);if(label.empty())label=Text(m,"targetLabel",512);
            auto wideExe=ToLower(Utf8ToWide(exe));
            if(wideExe.size()<5||wideExe.substr(wideExe.size()-4)!=L".exe"||exe.find_first_of("\\/\r\n")!=std::string::npos||std::any_of(exe.begin(),exe.end(),[](unsigned char c){return c<32;})){Error("请选择正在运行的应用程序");return;}
            s.targetExe=std::move(wideExe);s.targetLabel=label.empty()?s.targetExe:Utf8ToWide(label);s.volume=Int(m,"volume",s.volume,0,200);
            s.sampleRate=Int(m,"sampleRate",s.sampleRate,0,96000); s.bitDepth=Int(m,"bitDepth",s.bitDepth,0,32);
            if((s.sampleRate!=0&&s.sampleRate!=44100&&s.sampleRate!=48000&&s.sampleRate!=96000)||(s.bitDepth!=0&&s.bitDepth!=16&&s.bitDepth!=24&&s.bitDepth!=32)){Error("不支持的音频采集格式");return;}
            s.keepMicActive=Bool(m,"keepMicActive",s.keepMicActive);s.forceSend=Bool(m,"forceSend",s.forceSend);s.autoStart=Bool(m,"autoStart",s.autoStart);
            if(!SaveAudioSettings(ConfigPath(L"ts3-localaudio.ini"),s)){Error("无法保存音频配置");return;}
            if(type=="audioStart")StopShare("audio-start");
            CaptureController::Get().ApplySettings(s);if(type=="audioStart"&&!CaptureController::Get().Start())Error("无法启动音频采集");ShareBridgeTick();
        } else if(type=="denoiseSave") {
            const int model=Int(m,"model",0,0,2),strength=Int(m,"strength",70,0,100);
            auto s=LoadSettings(ConfigPath(L"ts3-localaudio.ini"));
            if(!SaveDenoiseSettings(ConfigPath(L"ts3-localaudio.ini"),model,strength)){Error("无法保存降噪配置");return;}
            /* 运行库没加载也允许先存配置：下次启动照样生效，这里只做即时切换 */
            if(!SetDenoiseConfig(model,strength)&&model!=0)Error("降噪运行库未就绪，设置已保存，重启 TeamSpeak 后生效");
            ShareBridgeTick();
        } else if(type=="audioStop") {CaptureController::Get().Stop();ShareBridgeTick();}
        else if(type=="signal"||type=="broadcast") {
            auto payload=m.find("payload");if(payload==m.end()||!payload->is_object())return;
            std::string session=Text(*payload,"sessionID",128),kind=Text(*payload,"kind",64);
            if(kind=="viewrequest"||kind=="leave"){std::lock_guard<std::mutex> lock(bridgeMutex);autoplayPermitted=kind=="viewrequest";}
            if(!session.empty()&&(kind=="viewrequest"||kind=="offer"||kind=="hoststart"||kind=="announce"||kind=="share-started"||kind=="share"||kind=="hostannounce")) {
                Context c=Current();std::lock_guard<std::mutex> lock(bridgeMutex);activeSession=session;activeHandler=c.handler;activeChannel=c.channel;
            }
            if(kind=="hoststop"||kind=="stop"||kind=="share-stopped") {std::lock_guard<std::mutex> lock(bridgeMutex);if(activeSession==session)activeSession.clear();}
            anyID to=type=="signal"?(anyID)Int(m,"to",0,0,65535):0;SendSignal(*payload,to);
        } else if(type=="screenAudio") {
            if(!Bool(m,"enabled",false))ScreenAudioStop();
            else {
                std::string mode=Text(m,"mode",32),exe=Text(m,"exe",260);
                if(mode!="exclude-ts"&&mode!="app"){Error("未知屏幕音频采集模式");return;}
                if(mode=="app"&&(exe.empty()||exe.find_first_of("\\/\r\n")!=std::string::npos)){Error("请选择屏幕共享音频应用");return;}
                if(!ScreenAudioStart(mode=="exclude-ts",Utf8ToWide(exe),[](std::string base64){Emit({{"type","pcm"},{"base64",std::move(base64)},{"rate",48000},{"channels",1}});}))Error(WideToUtf8(ScreenAudioStatus()));
            }
            ShareBridgeTick();
        } else if(type=="saveNetworkConfig") {
            Json clean;auto at=m.find("config");if(at==m.end())at=m.find("networkConfig");
            if(at==m.end()||!Network(*at,clean)){Error("STUN/TURN 配置格式不正确");return;}
            bool targetChanged=false;
            if(!SaveNetwork(clean,Bool(m,"credentialEdited",false),Text(m,"observedTargetScope",128),&targetChanged)){
                if(targetChanged){Emit(Bootstrap());Error("中继目标在输入后已变化，请重新输入用户名和密码并保存一次。");}
                else Error("无法保存网络配置或保护 TURN 凭据");return;
            }Emit(Bootstrap());
        } else if(type=="shareStopped")StopShare("share-stopped");
    } catch(const std::exception&) {Error("无法处理本地界面请求");}
}
void TS3ScreenOnPluginCommand(uint64 handler,const char* name,const char* command,anyID invoker,const char* invokerName,const char* invokerUid) {
    try {
        if(!command||!name||!invoker)return;
        std::string plugin=name;std::transform(plugin.begin(),plugin.end(),plugin.begin(),[](unsigned char c){return (char)std::tolower(c);});
        if(plugin!="ts3localaudio"&&plugin!="ts3localaudio_win64")return;
        size_t length=strnlen(command,7001);if(length>7000||length<8||strncmp(command,kPrefix,strlen(kPrefix))!=0)return;
        Context c=Current();uint64 channel=0;
        if(!c.connected||handler!=c.handler||invoker==c.self||!c.api.getChannelOfClient||c.api.getChannelOfClient(handler,invoker,&channel)!=ERROR_ok||channel!=c.channel)return;
        const auto uid=ClientUid(c,invoker);
        if(uid.empty()||!invokerUid||strnlen(invokerUid,257)!=uid.size()||uid!=invokerUid)return;
        Json p=Parse(std::string(command+strlen(kPrefix),length-strlen(kPrefix)),7000);
        if(!p.is_object()||Int(p,"v",0,0,10)!=2||!p.contains("c")||!p["c"].is_number_unsigned()||p["c"].get<uint64>()!=c.channel)return;
        int to=Int(p,"to",0,0,65535);if(to&&to!=c.self)return;
        if(!p.contains("ts")||!p["ts"].is_number_integer())return;
        int64_t stamp=p["ts"].get<int64_t>(),now=UnixSeconds();if(stamp<now-300||stamp>now+60)return;
        int count=Int(p,"n",0,0,48),index=Int(p,"i",-1,-1,48);std::string nonce=Text(p,"id",64),data=Text(p,"d",kChunk);
        if(!count||index<0||index>=count||nonce.size()<16||data.empty())return;
        const std::string peerPrefix=std::to_string(handler)+":"+std::to_string(c.channel)+":"+std::to_string(invoker)+":"+std::to_string(uid.size())+":"+uid+":";
        std::string key=peerPrefix+nonce,encoded;
        {std::lock_guard<std::mutex> lock(bridgeMutex);Prune();if(!TakeBudget(budgets[invoker],length,256,1024*1024)||completed.count(key))return;
         auto at=assemblies.find(key);if(at==assemblies.end()) {size_t peerCount=0;for(const auto& pair:assemblies)if(pair.first.rfind(peerPrefix,0)==0)++peerCount;if(assemblies.size()>=32||peerCount>=4)return;Assembly a;a.count=count;a.created=Now();a.fragments.resize(count);at=assemblies.emplace(key,std::move(a)).first;}
         Assembly& a=at->second;if(a.count!=(size_t)count||!a.fragments[index].empty())return;
         a.bytes+=data.size();if(a.bytes>kMaxEncoded){assemblies.erase(at);return;}a.fragments[index]=data;++a.have;if(a.have!=a.count)return;
         for(const auto& part:a.fragments)encoded+=part;assemblies.erase(at);completed[key]=Now();}
        std::string raw;if(!Unbase64(encoded,raw)||raw.size()>kMaxSignal)return;Json payload=Parse(raw,kMaxSignal);if(!payload.is_object())return;
        Emit({{"type","signal"},{"handler",handler},{"channelID",c.channel},{"from",invoker},{"fromUid",uid},{"name",std::string(invokerName?invokerName:"").substr(0,512)},{"payload",payload}});
    } catch(const std::exception&) { /* Malformed/untrusted control traffic is ignored. */ }
}
}
