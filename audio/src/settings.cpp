#include "settings.h"
#include "common.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <mutex>

namespace la {

namespace {
std::recursive_mutex settingsMutex;

std::string Trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::wstring SingleLineValue(const std::wstring& value)
{
    // Application descriptions and local UI messages are not INI syntax. Drop
    // C0/C1 controls (including CR, LF and NUL) before writing or consuming a
    // value, so every caller uses the same single-line persistence boundary.
    std::wstring clean;
    clean.reserve(value.size());
    for (wchar_t ch : value) {
        if (ch < 0x20 || (ch >= 0x7f && ch <= 0x9f))
            continue;
        clean.push_back(ch);
    }
    return clean;
}

} // namespace

Settings LoadSettings(const std::wstring& path)
{
    std::lock_guard<std::recursive_mutex> lock(settingsMutex);
    Settings s;
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in)
        return s;

    std::string line;
    while (std::getline(in, line)) {
        if (line.size() > 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
            line = line.substr(3);
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));

        if (key == "targetExe")
            s.targetExe = SingleLineValue(ToLower(Utf8ToWide(val)));
        else if (key == "targetLabel")
            s.targetLabel = SingleLineValue(Utf8ToWide(val));
        else if (key == "sampleRate") s.sampleRate = atoi(val.c_str());
        else if (key == "bitDepth") s.bitDepth = atoi(val.c_str());
        else if (key == "volume")
            s.volume = atoi(val.c_str());
        else if (key == "forceSend")
            s.forceSend = (val == "1");
        else if (key == "keepMicActive")
            s.keepMicActive = (val == "1");
        else if (key == "autoStart")
            s.autoStart = (val == "1");
        else if (key == "denoiseModel")
            s.denoiseModel = atoi(val.c_str());
        else if (key == "denoiseStrength")
            s.denoiseStrength = atoi(val.c_str());
    }

    if(s.sampleRate!=44100 && s.sampleRate!=48000 && s.sampleRate!=96000) s.sampleRate=0;
    if(s.bitDepth!=16 && s.bitDepth!=24 && s.bitDepth!=32) s.bitDepth=0;
    if (s.volume < 0)
        s.volume = 0;
    if (s.volume > 200)
        s.volume = 200;
    if (s.denoiseModel < 0 || s.denoiseModel > 2)
        s.denoiseModel = 0;
    if (s.denoiseStrength < 0)
        s.denoiseStrength = 0;
    if (s.denoiseStrength > 100)
        s.denoiseStrength = 100;
    if (s.targetLabel.empty() && !s.targetExe.empty())
        s.targetLabel = s.targetExe;
    return s;
}

bool SaveSettings(const std::wstring& path, const Settings& s)
{
    std::lock_guard<std::recursive_mutex> lock(settingsMutex);
    std::ostringstream out;
    out << "# ts3-localaudio 配置，可手工编辑（UTF-8）\n";
    out << "targetExe=" << WideToUtf8(SingleLineValue(ToLower(s.targetExe))) << "\n";
    out << "targetLabel=" << WideToUtf8(SingleLineValue(s.targetLabel)) << "\n";
    out << "# sampleRate/bitDepth=0: follow TS microphone callback (48000 Hz / 16-bit), not Windows hardware format\n";
    out << "sampleRate=" << s.sampleRate << "\n";
    out << "bitDepth=" << s.bitDepth << "\n";
    out << "volume=" << s.volume << "\n";
    out << "forceSend=" << (s.forceSend ? 1 : 0) << "\n";
    out << "keepMicActive=" << (s.keepMicActive ? 1 : 0) << "\n";
    out << "autoStart=" << (s.autoStart ? 1 : 0) << "\n";
    out << "# denoiseModel: 0=off 1=GTCRN 2=RNNoise; denoiseStrength: 0-100 dry/wet mix\n";
    out << "denoiseModel=" << s.denoiseModel << "\n";
    out << "denoiseStrength=" << s.denoiseStrength << "\n";

    const std::wstring temporary = path + L".tmp";
    const std::string contents = out.str();
    std::ofstream file(temporary.c_str(), std::ios::binary | std::ios::trunc);
    if (!file)
        return false;
    file.write(contents.data(), (std::streamsize)contents.size());
    file.close();
    if (!file || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

bool SaveAudioSettings(const std::wstring& path,const Settings& audio)
{
    std::lock_guard<std::recursive_mutex> lock(settingsMutex);
    const auto latest=LoadSettings(path);auto merged=audio;
    merged.denoiseModel=latest.denoiseModel;merged.denoiseStrength=latest.denoiseStrength;
    return SaveSettings(path,merged);
}
bool SaveDenoiseSettings(const std::wstring& path,int model,int strength)
{
    if(model<0||model>2||strength<0||strength>100)return false;
    std::lock_guard<std::recursive_mutex> lock(settingsMutex);
    auto merged=LoadSettings(path);merged.denoiseModel=model;merged.denoiseStrength=strength;
    return SaveSettings(path,merged);
}
} // namespace la
