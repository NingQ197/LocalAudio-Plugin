#pragma once
#include <stdint.h>
#include <wchar.h>
#pragma pack(push,8)
struct NingqNoiseUiCallbacks {
    uint32_t size;
    void (*read)(int* model,int* strength);
    int (*write)(int model,int strength);
    void (*status)(wchar_t* text,int capacity);
};
#pragma pack(pop)
typedef int (*NingqNoiseUiStartFn)(const NingqNoiseUiCallbacks*);
typedef void (*NingqNoiseUiStopFn)();
