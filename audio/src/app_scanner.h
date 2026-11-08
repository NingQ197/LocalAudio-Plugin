#ifndef LA_APP_SCANNER_H
#define LA_APP_SCANNER_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace la {

struct AppEntry {
    std::wstring exe;     /* 小写 exe 名，例如 "qqmusic.exe" */
    std::wstring display; /* 友好名称，例如 "QQ音乐" */
    DWORD pid = 0;        /* 扫描到的代表进程 id */
    bool playing = false; /* 当前是否有活动音频会话（不等于实际音量非零） */
    bool hasWindow = false;
    std::string icon;     /* 程序原始图标的 PNG data URL；读取失败则为空 */
};

struct Preset {
    const wchar_t* label;
    const wchar_t* exe;
};

/* 常见应用的快捷选项 */
const std::vector<Preset>& GetPresets();

/* 扫描正在运行的、可能出声的应用（所有活动输出设备的音频会话 + 有窗口的进程） */
std::vector<AppEntry> ScanApps();

/* 按 exe 名找进程：选择同名根进程，包含其音频子进程 */
bool FindPidByExe(const std::wstring& exeLower, DWORD* pidOut);

/* exe 的友好名称（读版本信息里的 FileDescription） */
std::wstring QueryAppDisplayName(const std::wstring& exePath);

/* 只读取可执行文件的图标资源，不运行目标程序，不向前端暴露文件路径。 */
std::string QueryAppIcon(const std::wstring& exePath);

bool PidAlive(DWORD pid);
std::wstring PidExeName(DWORD pid);

} // namespace la

#endif /* LA_APP_SCANNER_H */
