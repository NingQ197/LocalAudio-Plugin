#ifndef LA_SETTINGS_H
#define LA_SETTINGS_H

#include <string>

namespace la {

struct Settings {
    std::wstring targetExe;   /* 小写 exe 名 */
    std::wstring targetLabel; /* 显示名 */
    int sampleRate = 0; // 0 follows the TS microphone callback (48000 Hz)
    int bitDepth = 0;   // 0 follows the TS microphone callback (16-bit)
    int volume = 100;         /* 0 - 200 (%) */
    bool forceSend = true;
    bool keepMicActive = true;
    bool autoStart = false;

    /* AI 降噪：0=关闭 1=GTCRN 2=RNNoise；strength 为 0-100 的干湿混合比例 */
    int denoiseModel = 0;
    int denoiseStrength = 70;

    bool HasTarget() const { return !targetExe.empty(); }
};

/* 配置文件放在 TeamSpeak 的配置目录下，UTF-8 纯文本 key=value */
Settings LoadSettings(const std::wstring& path);
bool SaveSettings(const std::wstring& path, const Settings& s);
bool SaveAudioSettings(const std::wstring& path,const Settings& s);
bool SaveDenoiseSettings(const std::wstring& path,int model,int strength);

} // namespace la

#endif /* LA_SETTINGS_H */
