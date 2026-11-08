#include "app_scanner.h"
#include "common.h"

#include <algorithm>
#include <map>
#include <set>

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <gdiplus.h>
#include <wincrypt.h>

namespace la {

namespace {

class ComScope {
public:
    ComScope()
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        owned_ = SUCCEEDED(hr);
    }
    ~ComScope()
    {
        if (owned_)
            CoUninitialize();
    }

private:
    bool owned_ = false;
};

std::wstring BaseName(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

std::wstring QueryProcessPath(DWORD pid)
{
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc)
        return std::wstring();
    wchar_t buf[MAX_PATH * 2] = {0};
    DWORD size = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    std::wstring result;
    if (QueryFullProcessImageNameW(proc, 0, buf, &size))
        result.assign(buf, size);
    CloseHandle(proc);
    return result;
}

void AddEntry(std::map<std::wstring, AppEntry>& out, const std::wstring& exePath, DWORD pid, bool playing, bool hasWindow)
{
    if (exePath.empty())
        return;
    std::wstring exe = ToLower(BaseName(exePath));
    if (exe.empty())
        return;

    auto it = out.find(exe);
    if (it == out.end()) {
        AppEntry e;
        e.exe = exe;
        e.display = QueryAppDisplayName(exePath);
        if (e.display.empty())
            e.display = exe.substr(0, exe.size() > 4 ? exe.size() - 4 : exe.size());
        e.pid = pid;
        e.playing = playing;
        e.hasWindow = hasWindow;
        out.emplace(exe, e);
        return;
    }

    AppEntry& e = it->second;
    if (playing) {
        e.playing = true;
        e.pid = pid; /* 正在出声的进程最有代表性 */
    } else if (e.pid == 0) {
        e.pid = pid;
    }
    if (hasWindow)
        e.hasWindow = true;
}

void CollectAudioSessions(std::map<std::wstring, AppEntry>& out)
{
    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr) || !enumerator)
        return;

    IMMDeviceCollection* devices = nullptr;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)) && devices) {
        UINT deviceCount = 0;
        devices->GetCount(&deviceCount);
        for (UINT deviceIndex = 0; deviceIndex < deviceCount; ++deviceIndex) {
            IMMDevice* device = nullptr;
            if (FAILED(devices->Item(deviceIndex, &device)) || !device)
                continue;
            IAudioSessionManager2* manager = nullptr;
            if (SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&manager)) && manager) {
                IAudioSessionEnumerator* sessions = nullptr;
                if (SUCCEEDED(manager->GetSessionEnumerator(&sessions)) && sessions) {
                    int count = 0;
                    if (SUCCEEDED(sessions->GetCount(&count))) {
                        for (int i = 0; i < count; ++i) {
                            IAudioSessionControl* control = nullptr;
                            if (FAILED(sessions->GetSession(i, &control)) || !control)
                                continue;
                            IAudioSessionControl2* control2 = nullptr;
                            if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&control2)) && control2) {
                                DWORD pid = 0;
                                AudioSessionState state = AudioSessionStateInactive;
                                if (SUCCEEDED(control2->GetProcessId(&pid)) && pid > 4 && pid != GetCurrentProcessId()) {
                                    control->GetState(&state);
                                    AddEntry(out, QueryProcessPath(pid), pid, state == AudioSessionStateActive, false);
                                }
                                control2->Release();
                            }
                            control->Release();
                        }
                    }
                    sessions->Release();
                }
                manager->Release();
            }
            device->Release();
        }
        devices->Release();
    }
    enumerator->Release();
}

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM param)
{
    auto* pids = reinterpret_cast<std::vector<DWORD>*>(param);
    if (!IsWindowVisible(hwnd))
        return TRUE;
    if (GetWindowTextLengthW(hwnd) == 0)
        return TRUE;
    if (GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid > 4)
        pids->push_back(pid);
    return TRUE;
}

void CollectWindowedProcesses(std::map<std::wstring, AppEntry>& out)
{
    std::vector<DWORD> pids;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&pids));
    if (pids.empty())
        return;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;

    std::map<DWORD, std::wstring> exeByPid;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            exeByPid[entry.th32ProcessID] = entry.szExeFile;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    for (DWORD pid : pids) {
        std::wstring path = QueryProcessPath(pid);
        if (path.empty()) {
            auto it = exeByPid.find(pid);
            if (it == exeByPid.end())
                continue;
            path = it->second;
        }
        AddEntry(out, path, pid, false, true);
    }
}

} // namespace

const std::vector<Preset>& GetPresets()
{
    static const std::vector<Preset> presets = {
        {L"QQ音乐", L"QQMusic.exe"},
        {L"网易云音乐", L"cloudmusic.exe"},
        {L"Edge", L"msedge.exe"},
        {L"Chrome", L"chrome.exe"},
        {L"微信", L"WeChat.exe"},
        {L"酷狗音乐", L"KuGou.exe"},
        {L"PotPlayer", L"PotPlayerMini64.exe"},
        {L"VLC", L"vlc.exe"},
        {L"Spotify", L"Spotify.exe"},
    };
    return presets;
}

std::vector<AppEntry> ScanApps()
{
    ComScope com;
    std::map<std::wstring, AppEntry> found;
    CollectAudioSessions(found);
    CollectWindowedProcesses(found);

    const DWORD self = GetCurrentProcessId();
    std::vector<AppEntry> result;
    result.reserve(found.size());
    for (auto& kv : found) {
        if (kv.second.pid == self || kv.second.exe == L"ts3client_win64.exe" || kv.second.exe == L"ts3client_win32.exe")
            continue;
        // Extract once per deduplicated application, not once per window/session.
        kv.second.icon = QueryAppIcon(QueryProcessPath(kv.second.pid));
        result.push_back(kv.second);
    }

    std::stable_sort(result.begin(), result.end(), [](const AppEntry& a, const AppEntry& b) {
        if (a.playing != b.playing)
            return a.playing; /* 正在出声的排前面 */
        if (a.hasWindow != b.hasWindow)
            return a.hasWindow;
        return a.display < b.display;
    });
    return result;
}

bool FindPidByExe(const std::wstring& exeLower, DWORD* pidOut)
{
    if (!pidOut) return false;
    *pidOut = 0;
    const std::wstring target = ToLower(exeLower);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    std::map<DWORD, DWORD> parents;
    std::set<DWORD> candidates;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            parents[entry.th32ProcessID] = entry.th32ParentProcessID;
            if (ToLower(entry.szExeFile) == target && entry.th32ProcessID > 4 &&
                entry.th32ProcessID != GetCurrentProcessId())
                candidates.insert(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (candidates.empty()) return false;

    /* The audio session may belong to a browser helper. Walk to the highest
     * matching ancestor so process loopback includes every child audio stream. */
    auto rootOf = [&](DWORD pid) {
        DWORD root = pid;
        std::set<DWORD> visited;
        while (pid && visited.insert(pid).second) {
            auto parent = parents.find(pid);
            if (parent == parents.end() || parent->second == pid) break;
            pid = parent->second;
            if (candidates.count(pid)) root = pid;
        }
        return root;
    };

    DWORD foregroundPid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
    if (candidates.count(foregroundPid)) {
        *pidOut = rootOf(foregroundPid);
        return true;
    }

    ComScope com;
    std::map<std::wstring, AppEntry> sessions;
    CollectAudioSessions(sessions);
    auto active = sessions.find(target);
    if (active != sessions.end() && active->second.playing && candidates.count(active->second.pid)) {
        *pidOut = rootOf(active->second.pid);
        return true;
    }

    std::vector<DWORD> windowPids;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&windowPids));
    for (DWORD pid : windowPids) {
        if (candidates.count(pid)) {
            *pidOut = rootOf(pid);
            return true;
        }
    }
    *pidOut = rootOf(*candidates.begin());
    return true;
}

std::wstring QueryAppDisplayName(const std::wstring& exePath)
{
    DWORD dummy = 0;
    DWORD size = GetFileVersionInfoSizeW(exePath.c_str(), &dummy);
    if (size == 0)
        return std::wstring();

    std::vector<BYTE> buffer(size);
    if (!GetFileVersionInfoW(exePath.c_str(), 0, size, buffer.data()))
        return std::wstring();

    struct Translation {
        WORD language;
        WORD codePage;
    };

    Translation* translations = nullptr;
    UINT length = 0;
    std::wstring result;
    if (VerQueryValueW(buffer.data(), L"\\VarFileInfo\\Translation", (LPVOID*)&translations, &length) && translations && length >= sizeof(Translation)) {
        for (UINT i = 0; i < length / sizeof(Translation) && result.empty(); ++i) {
            wchar_t subBlock[128] = {0};
            swprintf(subBlock, 128, L"\\StringFileInfo\\%04x%04x\\FileDescription", translations[i].language, translations[i].codePage);
            wchar_t* description = nullptr;
            UINT descLen = 0;
            if (VerQueryValueW(buffer.data(), subBlock, (LPVOID*)&description, &descLen) && description && descLen > 1)
                result.assign(description);
        }
    }
    return result;
}

std::string QueryAppIcon(const std::wstring& exePath)
{
    // Never ask the shell for a generic association icon or a network resource.
    // A missing/inaccessible executable is represented honestly by an empty icon.
    if (exePath.size() < 3 || exePath[1] != L':' ||
        GetFileAttributesW(exePath.c_str()) == INVALID_FILE_ATTRIBUTES)
        return {};
    HICON icon = nullptr;
    if (!ExtractIconExW(exePath.c_str(), 0, &icon, nullptr, 1) || !icon)
        return {};
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    std::string result;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok) {
        {
            Gdiplus::Bitmap bitmap(icon);
            IStream* stream = nullptr;
            if (bitmap.GetLastStatus() == Gdiplus::Ok &&
                SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) {
                const CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a,0x73,0x00,0x00,0xf8,0x1e,0xf3,0x2e}};
                if (bitmap.Save(stream, &png, nullptr) == Gdiplus::Ok) {
                    STATSTG stat = {};
                    HGLOBAL memory = nullptr;
                    if (SUCCEEDED(stream->Stat(&stat, STATFLAG_NONAME)) &&
                        stat.cbSize.QuadPart > 0 && stat.cbSize.QuadPart <= 65536 &&
                        SUCCEEDED(GetHGlobalFromStream(stream, &memory))) {
                        const auto* bytes = static_cast<const BYTE*>(GlobalLock(memory));
                        if (bytes) {
                            const DWORD size = static_cast<DWORD>(stat.cbSize.QuadPart);
                            DWORD length = 0;
                            const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
                            if (CryptBinaryToStringA(bytes, size, flags, nullptr, &length)) {
                                std::string encoded(length, '\0');
                                if (CryptBinaryToStringA(bytes, size, flags, encoded.data(), &length)) {
                                    encoded.resize(length);
                                    if (!encoded.empty() && encoded.back() == '\0') encoded.pop_back();
                                    result = "data:image/png;base64," + encoded;
                                }
                            }
                            GlobalUnlock(memory);
                        }
                    }
                }
                stream->Release();
            }
        }
        Gdiplus::GdiplusShutdown(token);
    }
    DestroyIcon(icon);
    return result;
}

bool PidAlive(DWORD pid)
{
    HANDLE proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc)
        return false;
    bool alive = WaitForSingleObject(proc, 0) == WAIT_TIMEOUT;
    CloseHandle(proc);
    return alive;
}

std::wstring PidExeName(DWORD pid)
{
    return ToLower(BaseName(QueryProcessPath(pid)));
}

} // namespace la
