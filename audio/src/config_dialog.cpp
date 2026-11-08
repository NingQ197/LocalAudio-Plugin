#include "config_dialog.h"

#include <atomic>
#include <string>
#include <thread>
#include <mutex>
#include <vector>

#include <windows.h>

#include <commctrl.h>

#include "app_scanner.h"
#include "common.h"
#include "process_loopback.h"
#include "settings.h"

namespace la {
namespace {

const wchar_t* kWindowClass = L"TS3LocalAudioConfigWindow";
const wchar_t* kWindowTitle = L"本地应用声音转播 - TeamSpeak 3 插件";

enum ControlId {
    IDC_APP_LIST = 1001,
    IDC_REFRESH = 1002,
    IDC_VOLUME = 1003,
    IDC_VOLUME_LABEL = 1004,
    IDC_FORCE_SEND = 1005,
    IDC_KEEP_ACTIVE = 1006,
    IDC_AUTOSTART = 1007,
    IDC_START = 1008,
    IDC_STOP = 1009,
    IDC_CLOSE = 1010,
    IDC_STATUS = 1011,
    IDC_SELECTED_LABEL = 1012,
    IDC_PRESET_BASE = 1100,
};

const int kClientWidth = 660;
const int kClientHeight = 612;

struct DialogContext {
    HWND window = nullptr;
    HWND list = nullptr;
    HWND volume = nullptr;
    HWND volumeLabel = nullptr;
    HWND status = nullptr;
    HWND selectedLabel = nullptr;
    HWND startButton = nullptr;
    HWND stopButton = nullptr;
    HWND forceSend = nullptr;
    HWND keepActive = nullptr;
    HWND autoStart = nullptr;
    HFONT font = nullptr;
    HFONT boldFont = nullptr;

    Settings settings;
    std::vector<AppEntry> apps;
    std::wstring pendingExe;
    std::wstring pendingLabel;
};

std::atomic<bool> g_dialogOpen{false};
std::atomic<bool> g_dialogClosing{false};
std::mutex g_dialogMutex;
std::thread g_dialogThread;
std::atomic<HWND> g_dialogWindow{nullptr};
std::wstring g_configPath;

HINSTANCE PluginModule()
{
    HMODULE instance = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&g_dialogOpen), &instance);
    return instance;
}

DialogContext* ContextOf(HWND hwnd)
{
    return reinterpret_cast<DialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void ApplyFont(HWND parent, HFONT font)
{
    EnumChildWindows(
        parent,
        [](HWND child, LPARAM param) -> BOOL {
            SendMessageW(child, WM_SETFONT, (WPARAM)param, TRUE);
            return TRUE;
        },
        (LPARAM)font);
}

std::wstring SelectedSummary(const DialogContext* ctx)
{
    if (ctx->pendingExe.empty())
        return L"当前未选择应用";
    std::wstring text = L"当前选择：" + (ctx->pendingLabel.empty() ? ctx->pendingExe : ctx->pendingLabel);
    if (!ctx->pendingExe.empty())
        text += L"  (" + ctx->pendingExe + L")";
    return text;
}

void RefreshList(DialogContext* ctx)
{
    ctx->apps = ScanApps();

    SendMessageW(ctx->list, LB_RESETCONTENT, 0, 0);
    int selectIndex = -1;
    for (size_t i = 0; i < ctx->apps.size(); ++i) {
        const AppEntry& app = ctx->apps[i];
        std::wstring line;
        line += app.playing ? L"[音频] " : L"[程序] ";
        line += app.display;
        line += L"   (" + app.exe + L")";
        SendMessageW(ctx->list, LB_ADDSTRING, 0, (LPARAM)line.c_str());
        if (app.exe == ctx->pendingExe)
            selectIndex = (int)i;
    }
    if (selectIndex >= 0)
        SendMessageW(ctx->list, LB_SETCURSEL, (WPARAM)selectIndex, 0);

    SetWindowTextW(ctx->selectedLabel, SelectedSummary(ctx).c_str());

    if (ctx->apps.empty())
        SetWindowTextW(ctx->status, L"没有扫描到正在运行的应用。可以先打开音乐软件并播放一首歌，然后点“扫描程序”。");
}

void UpdateVolumeLabel(DialogContext* ctx)
{
    int value = (int)SendMessageW(ctx->volume, TBM_GETPOS, 0, 0);
    wchar_t text[64];
    swprintf(text, 64, L"音量：%d%%", value);
    SetWindowTextW(ctx->volumeLabel, text);
}

void ReadUiToSettings(DialogContext* ctx)
{
    ctx->settings.targetExe = ctx->pendingExe;
    ctx->settings.targetLabel = ctx->pendingLabel;
    ctx->settings.volume = (int)SendMessageW(ctx->volume, TBM_GETPOS, 0, 0);
    ctx->settings.forceSend = SendMessageW(ctx->forceSend, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ctx->settings.keepMicActive = SendMessageW(ctx->keepActive, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ctx->settings.autoStart = SendMessageW(ctx->autoStart, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void StartCapture(DialogContext* ctx)
{
    if (ctx->pendingExe.empty()) {
        MessageBoxW(ctx->window, L"请先在列表里选择一个要转播的应用。", L"提示", MB_OK | MB_ICONINFORMATION);
        return;
    }
    ReadUiToSettings(ctx);
    if (!SaveAudioSettings(g_configPath, ctx->settings)) {
        MessageBoxW(ctx->window, L"无法保存配置，请检查 TeamSpeak 配置目录的写入权限。", L"保存失败", MB_OK | MB_ICONERROR);
        return;
    }

    CaptureController& controller = CaptureController::Get();
    controller.ApplySettings(ctx->settings);
    if (!controller.Start())
        MessageBoxW(ctx->window, L"无法启动采集线程，请查看 TeamSpeak 客户端日志。", L"启动失败", MB_OK | MB_ICONERROR);
}

void StopCapture(DialogContext* /*ctx*/)
{
    CaptureController::Get().Stop();
}

void CreateControls(DialogContext* ctx)
{
    HINSTANCE instance = PluginModule();

    auto create = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) -> HWND {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, ctx->window, (HMENU)(INT_PTR)id, instance, nullptr);
    };

    create(L"STATIC", L"① 先打开软件播放音乐，再选预设或点“扫描程序”选择应用。", SS_LEFT, 16, 14, 628, 20, 0);

    ctx->list = create(L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP, 16, 38, 628, 196, IDC_APP_LIST);

    create(L"BUTTON", L"扫描程序", BS_PUSHBUTTON | WS_TABSTOP, 16, 242, 100, 28, IDC_REFRESH);
    create(L"STATIC", L"[音频] 表示存在活动音频会话；仅采集所选应用及子进程。", SS_LEFT, 126, 248, 400, 18, 0);

    create(L"STATIC", L"快捷选择：", SS_LEFT, 16, 282, 70, 20, 0);

    const std::vector<Preset>& presets = GetPresets();
    const int presetCount = presets.size() < 5 ? (int)presets.size() : 5;
    int x = 90;
    for (int i = 0; i < presetCount; ++i) {
        create(L"BUTTON", presets[i].label, BS_PUSHBUTTON | WS_TABSTOP, x, 278, 104, 28, IDC_PRESET_BASE + i);
        x += 110;
    }

    create(L"BUTTON", L"② 选项", BS_GROUPBOX, 16, 320, 628, 138, 0);

    create(L"STATIC", L"音量：", SS_LEFT, 32, 348, 50, 20, 0);
    ctx->volume = create(L"msctls_trackbar32", L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 84, 344, 400, 30, IDC_VOLUME);
    SendMessageW(ctx->volume, TBM_SETRANGE, TRUE, MAKELPARAM(0, 200));
    SendMessageW(ctx->volume, TBM_SETTICFREQ, 10, 0);
    ctx->volumeLabel = create(L"STATIC", L"音量：100%", SS_LEFT, 494, 348, 120, 20, IDC_VOLUME_LABEL);

    ctx->keepActive = create(L"BUTTON", L"保持连续发送（无需按说话键；麦克风声音也会一起发送）", BS_AUTOCHECKBOX | WS_TABSTOP, 32, 378, 590, 22, IDC_KEEP_ACTIVE);
    ctx->forceSend = create(L"BUTTON", L"音乐帧请求发送（客户端/服务器静音仍然有效）", BS_AUTOCHECKBOX | WS_TABSTOP, 32, 404, 590, 22, IDC_FORCE_SEND);
    ctx->autoStart = create(L"BUTTON", L"下次启动 TeamSpeak 时自动开始转播（默认关闭）", BS_AUTOCHECKBOX | WS_TABSTOP, 32, 430, 590, 22, IDC_AUTOSTART);

    create(L"BUTTON", L"③ 状态", BS_GROUPBOX, 16, 466, 628, 82, 0);
    ctx->status = create(L"STATIC", L"未开始", SS_LEFT | SS_NOPREFIX, 32, 486, 596, 36, IDC_STATUS);
    ctx->selectedLabel = create(L"STATIC", L"当前未选择应用", SS_LEFT | SS_NOPREFIX, 32, 526, 596, 20, IDC_SELECTED_LABEL);

    ctx->startButton = create(L"BUTTON", L"应用并开始", BS_DEFPUSHBUTTON | WS_TABSTOP, 356, 562, 110, 32, IDC_START);
    ctx->stopButton = create(L"BUTTON", L"停止", BS_PUSHBUTTON | WS_TABSTOP, 476, 562, 80, 32, IDC_STOP);
    create(L"BUTTON", L"关闭", BS_PUSHBUTTON | WS_TABSTOP, 566, 562, 78, 32, IDC_CLOSE);

    ApplyFont(ctx->window, ctx->font);
    SendMessageW(ctx->status, WM_SETFONT, (WPARAM)ctx->font, TRUE);
}

void SelectPreset(DialogContext* ctx, int index)
{
    const std::vector<Preset>& presets = GetPresets();
    if (index < 0 || index >= (int)presets.size())
        return;

    ctx->pendingExe = ToLower(presets[index].exe);
    ctx->pendingLabel = presets[index].label;

    int select = -1;
    for (size_t i = 0; i < ctx->apps.size(); ++i) {
        if (ctx->apps[i].exe == ctx->pendingExe) {
            select = (int)i;
            break;
        }
    }
    if (select >= 0) {
        SendMessageW(ctx->list, LB_SETCURSEL, (WPARAM)select, 0);
        SetWindowTextW(ctx->selectedLabel, SelectedSummary(ctx).c_str());
    } else {
        SendMessageW(ctx->list, LB_SETCURSEL, (WPARAM)-1, 0);
        std::wstring text = SelectedSummary(ctx) + L"  —— 未运行；点击开始后会等待它启动";
        SetWindowTextW(ctx->selectedLabel, text.c_str());
    }
}

void OnCommand(DialogContext* ctx, int id, int notification)
{
    if (id == IDC_APP_LIST && notification == LBN_SELCHANGE) {
        int index = (int)SendMessageW(ctx->list, LB_GETCURSEL, 0, 0);
        if (index >= 0 && index < (int)ctx->apps.size()) {
            ctx->pendingExe = ctx->apps[index].exe;
            ctx->pendingLabel = ctx->apps[index].display;
            SetWindowTextW(ctx->selectedLabel, SelectedSummary(ctx).c_str());
        }
        return;
    }

    if (id == IDC_REFRESH) {
        RefreshList(ctx);
        return;
    }

    if (id >= IDC_PRESET_BASE && id < IDC_PRESET_BASE + 8) {
        SelectPreset(ctx, id - IDC_PRESET_BASE);
        return;
    }

    switch (id) {
    case IDC_START:
        StartCapture(ctx);
        break;
    case IDC_STOP:
        StopCapture(ctx);
        break;
    case IDC_CLOSE:
        DestroyWindow(ctx->window);
        break;
    case IDC_KEEP_ACTIVE:
    case IDC_FORCE_SEND:
    case IDC_AUTOSTART:
        ReadUiToSettings(ctx);
        SaveAudioSettings(g_configPath, ctx->settings);
        CaptureController::Get().ApplySettings(ctx->settings);
        break;
    default:
        break;
    }
}

void OnTimer(DialogContext* ctx)
{
    std::wstring status = State().GetStatus();
    SetWindowTextW(ctx->status, status.c_str());

    bool running = CaptureController::Get().Running();
    EnableWindow(ctx->startButton, !ctx->pendingExe.empty());
    EnableWindow(ctx->stopButton, running);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    DialogContext* ctx = ContextOf(hwnd);

    switch (message) {
    case WM_CREATE: {
        ctx = new DialogContext();
        ctx->window = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)ctx);

        NONCLIENTMETRICSW metrics = {};
        metrics.cbSize = sizeof(metrics);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
        ctx->font = CreateFontIndirectW(&metrics.lfMessageFont);
        LOGFONTW bold = metrics.lfMessageFont;
        bold.lfWeight = FW_BOLD;
        ctx->boldFont = CreateFontIndirectW(&bold);

        ctx->settings = LoadSettings(g_configPath);
        ctx->pendingExe = ctx->settings.targetExe;
        ctx->pendingLabel = ctx->settings.targetLabel;

        CreateControls(ctx);

        SendMessageW(ctx->volume, TBM_SETPOS, TRUE, ctx->settings.volume);
        SendMessageW(ctx->keepActive, BM_SETCHECK, ctx->settings.keepMicActive ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(ctx->forceSend, BM_SETCHECK, ctx->settings.forceSend ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(ctx->autoStart, BM_SETCHECK, ctx->settings.autoStart ? BST_CHECKED : BST_UNCHECKED, 0);
        UpdateVolumeLabel(ctx);

        RefreshList(ctx);
        SetTimer(hwnd, 1, 400, nullptr);
        return 0;
    }
    case WM_COMMAND:
        if (ctx)
            OnCommand(ctx, LOWORD(wParam), HIWORD(wParam));
        return 0;
    case WM_HSCROLL:
        if (ctx && (HWND)lParam == ctx->volume) {
            UpdateVolumeLabel(ctx);
            ctx->settings.volume = (int)SendMessageW(ctx->volume, TBM_GETPOS, 0, 0);
            State().gain.store((float)ctx->settings.volume / 100.0f);
            if (LOWORD(wParam) == TB_ENDTRACK)
                SaveAudioSettings(g_configPath, ctx->settings);
        }
        return 0;
    case WM_TIMER:
        if (ctx)
            OnTimer(ctx);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (ctx) {
            if (ctx->font)
                DeleteObject(ctx->font);
            if (ctx->boldFont)
                DeleteObject(ctx->boldFont);
            delete ctx;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

void RunDialogThread()
{
    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&controls);

    HINSTANCE instance = PluginModule();

    {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kWindowClass;
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = wc.hIcon;
        if (!RegisterClassExW(&wc)) {
            g_dialogOpen.store(false);
            return;
        }
    }

    RECT rect = {0, 0, kClientWidth, kClientHeight};
    AdjustWindowRectEx(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, kWindowTitle, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        UnregisterClassW(kWindowClass, instance);
        g_dialogOpen.store(false);
        return;
    }

    g_dialogWindow.store(hwnd);
    if (g_dialogClosing.load())
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    else
        ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_dialogWindow.store(nullptr);
    UnregisterClassW(kWindowClass, instance);
    g_dialogOpen.store(false);
}

} // namespace

void ShowConfigDialog()
{
    std::lock_guard<std::mutex> lock(g_dialogMutex);
    if (g_dialogOpen.load()) {
        HWND window = g_dialogWindow.load();
        if (window) {
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
        }
        return;
    }
    if (g_dialogThread.joinable())
        g_dialogThread.join();
    g_dialogClosing.store(false);
    g_dialogOpen.store(true);
    g_dialogThread = std::thread(RunDialogThread);
}

void SetConfigPathForDialog(const std::wstring& path)
{
    g_configPath = path;
}

void CloseConfigDialog()
{
    std::lock_guard<std::mutex> lock(g_dialogMutex);
    g_dialogClosing.store(true);
    HWND window = g_dialogWindow.load();
    if (window)
        PostMessageW(window, WM_CLOSE, 0, 0);
    if (g_dialogThread.joinable())
        g_dialogThread.join();
}

bool IsConfigDialogOpen()
{
    return g_dialogOpen.load();
}

} // namespace la

