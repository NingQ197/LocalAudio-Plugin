#pragma once
#include <functional>
#include <string>
#include "ts3_functions.h"
namespace la {
using ShareJsonEmitter = std::function<void(std::string)>;
void AudioBridgeInitialize(const TS3Functions& api,const std::wstring& configDirectory);
void AudioBridgeShutdown();
void ShareBridgeSetEmitter(ShareJsonEmitter emitter);
void ShareBridgeHandleMessage(const std::string& json);
void ShareBridgeTick();
void ShareBridgeWindowClosed();
std::string ShareBridgeContextJson();
}
