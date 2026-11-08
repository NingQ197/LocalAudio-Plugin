/*
 * “按进程环回采集”API 的兼容层（运行时能力由 Windows 音频服务决定）。
 * Microsoft 文档列出的最低版本为 build 20348；不能据 SDK 版本假定支持。
 * 老版本 SDK / MinGW 里没有 audioclientactivationparams.h，这里按官方 ABI 补齐。
 */
#ifndef LA_WASAPI_COMPAT_H
#define LA_WASAPI_COMPAT_H

#include <windows.h>
#include <mmdeviceapi.h>

#if defined(__has_include)
#if __has_include(<audioclientactivationparams.h>)
#include <audioclientactivationparams.h>
#define LA_HAVE_ACTIVATION_PARAMS 1
#endif
#endif

#ifndef LA_HAVE_ACTIVATION_PARAMS

typedef enum AUDIOCLIENT_ACTIVATION_TYPE {
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
} AUDIOCLIENT_ACTIVATION_TYPE;

typedef enum PROCESS_LOOPBACK_MODE {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;

typedef struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;

typedef struct AUDIOCLIENT_ACTIVATION_PARAMS {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union {
        AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    };
} AUDIOCLIENT_ACTIVATION_PARAMS;

#endif /* LA_HAVE_ACTIVATION_PARAMS */

#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

#endif /* LA_WASAPI_COMPAT_H */
