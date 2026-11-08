#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace la {
enum class NoiseModel : int { Off=0, Gtcrn=1, Rnnoise=2 };
struct DenoiseConfig { int model=0; int strength=70; };
// Processing implementation used by both the live worker and offline tests.
class DenoiseStream {
public:
    DenoiseStream(const std::wstring& directory, int model, int channels);
    ~DenoiseStream();
    void Reset();
    void Feed(const float* interleaved, int frames, float strength);
    size_t Read(float* interleaved, size_t frames);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
bool InitializeDenoise(const std::wstring& runtime, int model, int strength);
void ShutdownDenoise();
DenoiseConfig GetDenoiseConfig();
/* 只改运行期状态，不落盘：持久化走 Settings，避免覆盖 ts3-localaudio.ini 里的其它键。 */
bool SetDenoiseConfig(int model, int strength);
std::wstring DenoiseStatus();
/* 便宜的预检查：当前是否真的需要为这一帧付出降噪处理 */
bool DenoiseActive();
// No file access, inference, allocation or blocking locks on the TS audio thread.
bool ProcessDenoise(uint64_t handler, short* samples, int frames, int channels);
}
