#pragma once
#include <functional>
#include <string>
#include "ts3_functions.h"
namespace la {
using ShareJsonEmitter = std::function<void(std::string)>;
void TS3ScreenInitialize(const TS3Functions& api, const char* pluginID, const std::wstring& configDirectory);
void TS3ScreenSetPluginID(const char* pluginID);
void TS3ScreenShutdown();
void TS3ScreenCurrentHandlerChange(uint64 handler);
void TS3ScreenOnPluginCommand(uint64 handler, const char* pluginName, const char* command,
                             anyID invoker, const char* invokerName, const char* invokerUid);
void ShareBridgeSetEmitter(ShareJsonEmitter emitter);
void ShareBridgeHandleMessage(const std::string& json);
void ShareBridgeTick();
void ShareBridgeWindowClosed();
bool ShareBridgeMayPlayRemoteAudio();
std::string ShareBridgeContextJson();
}
