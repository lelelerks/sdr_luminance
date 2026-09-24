// main.cpp — 程序入口、单实例控制与消息循环

#include <windows.h>

#include <commctrl.h>
#include <shlwapi.h>

#include "autostart.h"
#include "config.h"
#include "ui_main.h"
#include "ui_settings.h"

// 启用 comctl32 v6（视觉样式）。省去单独的 .rc 清单文件。
#pragma comment(linker,                                                   \
                "\"/manifestdependency:type='win32' "                     \
                "name='Microsoft.Windows.Common-Controls' "               \
                "version='6.0.0.0' processorArchitecture='*' "            \
                "publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr wchar_t kMutexName[] = L"Local\\SdrLuminance.SingleInstance";

// 已有实例在运行时唤起它并返回 false，表示本实例应当退出
bool ClaimSingleInstance(HANDLE& outMutex) {
    outMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!outMutex) return true; // 互斥体创建失败不应阻止程序运行
    if (GetLastError() != ERROR_ALREADY_EXISTS) return true;

    HWND existing = FindWindowW(ui::WindowClassName(), nullptr);
    if (existing) PostMessageW(existing, ui::ActivateMessage(), 0, 0);
    CloseHandle(outMutex);
    outMutex = nullptr;
    return false;
}

// 自启动时带 --tray，表示直接收进托盘不弹窗口
bool WantsTrayStart(LPWSTR cmdLine) {
    return cmdLine && StrStrIW(cmdLine, L"--tray") != nullptr;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int) {
    HANDLE mutex = nullptr;
    if (!ClaimSingleInstance(mutex)) return 0;

    const bool trayStart = WantsTrayStart(cmdLine);

    // 每显示器 DPI 感知 V2：跨不同缩放的显示器移动时窗口自动重排
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    config::Init();
    // 程序被移动过时静默修正自启动项里记录的路径
    autostart::RefreshPath();

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    if (!InitCommonControlsEx(&icc)) return 1;

    HWND hwnd = ui::CreateMainWindow(inst);
    if (!hwnd) return 1;

    if (!trayStart) {
        // SHOWNOACTIVATE：启动时不抢走当前前台程序的焦点
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd);
    }

    // 阻塞式消息循环：无消息时线程完全挂起，CPU 占用为 0%
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // 设置窗口需要 Tab 切换焦点、回车确定等对话框式键盘导航。
        // 主窗口是 WS_EX_NOACTIVATE，永远拿不到键盘输入，无需参与。
        HWND settings = ui::SettingsWindow();
        if (settings && IsDialogMessageW(settings, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mutex) CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
