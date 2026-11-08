#ifndef LA_TS3_BRIDGE_H
#define LA_TS3_BRIDGE_H

#include <cstdint>

namespace la {

/* 这些函数由 plugin.cpp 实现，内部转发到 TeamSpeak 客户端 API */

/* 把采集线程的日志写进 TS3 日志（可在 客户端 设置-日志 里看到） */
void Ts3LogLine(const char* message);

/*
 * 转播期间让客户端保持“连续发送”：
 * 1) 按键说话(PTT)：把 CLIENT_INPUT_DEACTIVATED 强制成 INPUT_ACTIVE，否则不按键时根本不采集；
 * 2) 语音激活(VAD)：临时把预处理器的 vad 关掉，否则客户端在没听到人声时不会开始一次传输，
 *    插件可在用户选择此选项后临时保持连续发送。
 * 停止转播后会把两项都恢复成原样。
 */
bool Ts3ShouldMixOnHandler(uint64_t handler);
void Ts3TransmitHoldBegin();
void Ts3TransmitHoldTick();
void Ts3TransmitHoldEnd();

} // namespace la

#endif /* LA_TS3_BRIDGE_H */
