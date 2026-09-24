#include "ui_main.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <climits>
#include <cstdio>
#include <cstdlib>

#include "autostart.h"
#include "config.h"
#include "hotkey.h"
#include "icon.h"
#include "sdr_api.h"
#include "tray.h"
#include "ui_settings.h"

namespace ui {
namespace {

constexpr wchar_t kClassName[] = L"SdrLuminanceMainWnd";
constexpr wchar_t kTitle[] = L"SDR 内容亮度";

// 布局常量，单位为 96 DPI 下的逻辑像素，实际使用时按窗口 DPI 缩放
constexpr int kClientW = 340;
constexpr int kClientH = 114;
constexpr int kMargin = 16;
constexpr int kLabelY = 10;
constexpr int kLabelH = 22;
constexpr int kValueW = 56;
constexpr int kTrackY = 42;
constexpr int kTrackH = 32;
constexpr int kStatusY = 84;
constexpr int kStatusH = 20;

constexpr int kIdTrack = 1001;
constexpr int kIdSettings = 1002;

constexpr int kMenuToggle = 40001;
constexpr int kMenuSettings = 40002;
constexpr int kMenuAutostart = 40003;
constexpr int kMenuExit = 40004;

constexpr int kBtnW = 56;
constexpr int kBtnH = 24;
constexpr int kBtnY = 80;

// 创建完成后由 CreateMainWindow 发送，按窗口实际 DPI 定尺寸。
// 不能在 WM_CREATE 里做——那时的 SetWindowPos 会被窗口创建流程覆盖。
constexpr UINT WM_FIT_CONTENT = WM_APP + 1;
constexpr UINT WM_TRAY_EVENT = WM_APP + 2;
// 快捷键冲突提示。必须 Post 而非在注册处直接弹窗 ——
// 在 WM_CREATE 里弹模态框会把窗口创建流程整个卡住，主窗口永远显示不出来。
constexpr UINT WM_HOTKEY_WARNING = WM_APP + 3;

constexpr UINT_PTR kTimerThrottle = 1;
constexpr UINT_PTR kTimerCommit = 2;
// 快捷键自动弹出的窗口何时收回。三个定时器均为一次性，触发即 KillTimer，
// 不构成常驻轮询。
constexpr UINT_PTR kTimerAutoHide = 3;
// 拖动时的写入节流间隔。约合 60Hz，肉眼已完全跟手，
// 同时把 DisplayConfigSetDeviceInfo 的调用频率压到最低。
constexpr ULONGLONG kThrottleMs = 16;
// 快捷键调节停止后多久落盘。长按连发期间只做实时生效不持久化，
// 松开手才写一次，避免几十次/秒的系统配置写入。
constexpr UINT kCommitDelayMs = 400;

constexpr wchar_t kKeyWindowX[] = L"WindowX";
constexpr wchar_t kKeyWindowY[] = L"WindowY";
constexpr int kNoPosition = INT_MIN;

struct State {
    HWND track = nullptr;
    HWND btnSettings = nullptr;
    HFONT fontUi = nullptr;
    HFONT fontValue = nullptr;
    HICON iconOn = nullptr;
    HICON iconOff = nullptr;
    UINT dpi = 96;
    bool trayAdded = false;

    int slider = 0;
    bool hdr = false;
    sdr::Status status = sdr::Status::Ok;

    // 拖动节流
    bool dragging = false;
    bool pending = false;
    int pendingValue = 0;
    ULONGLONG lastWriteTick = 0;

    // 快捷键
    hotkey::Combo combos[hotkey::kActionCount];
    int step = hotkey::kStepDefault;
    hotkey::RegisterOutcome hotkeyOutcome;
    bool commitPending = false;

    // 当前可见的窗口是快捷键自动弹出的，且尚未被用户接管。
    // 只有该标志为真时才会到点自动收回 —— 用户自己开的窗口绝不自动消失。
    bool autoShown = false;
    int popupMs = hotkey::kPopupMsDefault;
};

State* Get(HWND hwnd) {
    return reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

int Scale(const State* st, int v) {
    return MulDiv(v, static_cast<int>(st->dpi), 96);
}

// ---------------------------------------------------------------------------
// 字体与图标
// ---------------------------------------------------------------------------
HFONT MakeFont(UINT dpi, int heightPercent) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) {
        return reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    LOGFONTW lf = ncm.lfMessageFont;
    if (heightPercent != 100) lf.lfHeight = MulDiv(lf.lfHeight, heightPercent, 100);
    return CreateFontIndirectW(&lf);
}

void RebuildFonts(State* st) {
    if (st->fontUi) DeleteObject(st->fontUi);
    if (st->fontValue) DeleteObject(st->fontValue);
    st->fontUi = MakeFont(st->dpi, 100);
    st->fontValue = MakeFont(st->dpi, 150);
    if (st->btnSettings) {
        SendMessageW(st->btnSettings, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontUi), TRUE);
    }
}

void RebuildIcons(HWND hwnd, State* st) {
    if (st->iconOn) DestroyIcon(st->iconOn);
    if (st->iconOff) DestroyIcon(st->iconOff);
    const int size = GetSystemMetricsForDpi(SM_CXSMICON, st->dpi);
    st->iconOn = icon::Create(size, true);
    st->iconOff = icon::Create(size, false);
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(st->iconOn));
}

// ---------------------------------------------------------------------------
// 布局
// ---------------------------------------------------------------------------

// 按当前窗口 DPI 把窗口调整到内容所需的大小。
// 窗口尺寸与内容布局必须使用同一个 DPI 来源，否则在非 100% 缩放下内容会溢出：
// 创建阶段无法预知窗口会落在哪块显示器上，故统一以 GetDpiForWindow 的结果为准。
void ResizeToContent(HWND hwnd, State* st, const RECT* moveTo) {
    RECT rc{0, 0, Scale(st, kClientW), Scale(st, kClientH)};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    AdjustWindowRectExForDpi(&rc, style, FALSE, exStyle, st->dpi);

    UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
    int x = 0, y = 0;
    if (moveTo) {
        x = moveTo->left;
        y = moveTo->top;
    } else {
        flags |= SWP_NOMOVE;
    }
    SetWindowPos(hwnd, nullptr, x, y, rc.right - rc.left, rc.bottom - rc.top, flags);
}

void Layout(HWND hwnd, State* st) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right - rc.left;
    const int m = Scale(st, kMargin);
    // 滑杆自身留有内部边距，左右各外扩一点使其视觉上与文字对齐
    const int pad = Scale(st, 4);
    SetWindowPos(st->track, nullptr, m - pad, Scale(st, kTrackY), w - 2 * m + 2 * pad,
                 Scale(st, kTrackH), SWP_NOZORDER | SWP_NOACTIVATE);

    SetWindowPos(st->btnSettings, nullptr, w - m - Scale(st, kBtnW), Scale(st, kBtnY),
                 Scale(st, kBtnW), Scale(st, kBtnH), SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
// 窗口位置记忆
// ---------------------------------------------------------------------------
void SavePosition(HWND hwnd) {
    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return;
    config::SetInt(kKeyWindowX, rc.left);
    config::SetInt(kKeyWindowY, rc.top);
}

void RestorePosition(HWND hwnd) {
    const int x = config::GetInt(kKeyWindowX, kNoPosition);
    const int y = config::GetInt(kKeyWindowY, kNoPosition);
    if (x == kNoPosition || y == kNoPosition) return;

    RECT rc;
    if (!GetWindowRect(hwnd, &rc)) return;
    RECT target{x, y, x + (rc.right - rc.left), y + (rc.bottom - rc.top)};

    // 显示器数量或排布可能已变化，位置落到屏幕外时忽略记忆值
    if (!MonitorFromRect(&target, MONITOR_DEFAULTTONULL)) return;
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
// 托盘
// ---------------------------------------------------------------------------
void BuildTip(const State* st, wchar_t* buf, size_t cch) {
    if (st->status != sdr::Status::Ok) {
        swprintf_s(buf, cch, L"%s\n%s", kTitle, sdr::StatusText(st->status));
    } else if (!st->hdr) {
        swprintf_s(buf, cch, L"%s\n未开启 HDR", kTitle);
    } else {
        swprintf_s(buf, cch, L"%s：%d", kTitle, st->slider);
    }
}

void RefreshTray(HWND hwnd, State* st) {
    if (!st->trayAdded) return;
    wchar_t tip[128];
    BuildTip(st, tip, _countof(tip));
    const bool usable = st->hdr && st->status == sdr::Status::Ok;
    tray::Update(hwnd, usable ? st->iconOn : st->iconOff, tip);
}

void SyncFromSystem(HWND hwnd, State* st);

// 只负责显示，不碰状态。调用方自行决定是否需要先与系统对齐 ——
// 快捷键路径刚写完新值，再 GetSlider 回读一次反而可能读到滞后的旧值。
void ShowNoActivate(HWND hwnd) {
    // SHOWNOACTIVATE：唤起窗口时不抢走前台程序（例如全屏游戏）的焦点
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------
// 快捷键自动弹窗的收回计时
//
// 用户一旦主动介入（点托盘、拖滑杆、开设置、按唤起键），窗口即转为常驻，
// 只能由用户自己关闭 —— 否则会出现「正拖着滑杆窗口突然消失」。
// ---------------------------------------------------------------------------
void ScheduleAutoHide(HWND hwnd, State* st) {
    // 重复调用即重置倒计时：长按连发期间每次 WM_HOTKEY 都会刷新，停手后才开始计时
    SetTimer(hwnd, kTimerAutoHide, static_cast<UINT>(st->popupMs), nullptr);
}

void CancelAutoHide(HWND hwnd, State* st) {
    if (!st->autoShown) return;
    KillTimer(hwnd, kTimerAutoHide);
    st->autoShown = false;
}

void ToggleWindow(HWND hwnd) {
    State* st = Get(hwnd);
    if (st) CancelAutoHide(hwnd, st);

    if (IsWindowVisible(hwnd)) {
        ShowWindow(hwnd, SW_HIDE);
    } else if (st) {
        // 隐藏期间亮度可能已被系统设置页等外部途径改动，显示前重新对齐一次
        SyncFromSystem(hwnd, st);
        ShowNoActivate(hwnd);
    }
}

void OpenSettings(HWND hwnd) {
    if (State* st = Get(hwnd)) CancelAutoHide(hwnd, st);
    // 录制期间必须先解除全部热键，否则按下当前绑定的组合会触发动作而录不进去
    hotkey::UnregisterAll(hwnd);
    ui::OpenSettings(hwnd, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd,
                                                                        GWLP_HINSTANCE)));
}

void ShowTrayMenu(HWND hwnd, int x, int y) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    // 右键托盘同样算用户介入。菜单期间本窗口会被临时置前台（见下），
    // 此时若倒计时到点，SW_HIDE 掉一个前台窗口会连带把菜单也弄没。
    if (State* st = Get(hwnd)) CancelAutoHide(hwnd, st);

    AppendMenuW(menu, MF_STRING, kMenuToggle, IsWindowVisible(hwnd) ? L"隐藏窗口" : L"显示窗口");
    AppendMenuW(menu, MF_STRING, kMenuSettings, L"设置…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    // 每次弹出都实时查注册表，与设置页共享同一状态
    AppendMenuW(menu, MF_STRING | (autostart::IsEnabled() ? MF_CHECKED : MF_UNCHECKED),
                kMenuAutostart, L"开机时自动启动");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");

    // 托盘菜单的惯例做法：先置前台，菜单才会在点击别处时正常消失。
    // 但 WS_EX_NOACTIVATE 窗口无法被 SetForegroundWindow 置前台，
    // 故在菜单期间临时摘掉该样式，结束后立即还原。
    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_NOACTIVATE);
    SetForegroundWindow(hwnd);

    const int cmd = static_cast<int>(TrackPopupMenu(
        menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, x, y, 0, hwnd, nullptr));

    PostMessageW(hwnd, WM_NULL, 0, 0);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
    DestroyMenu(menu);

    if (cmd == kMenuToggle) {
        ToggleWindow(hwnd);
    } else if (cmd == kMenuSettings) {
        OpenSettings(hwnd);
    } else if (cmd == kMenuAutostart) {
        if (!autostart::SetEnabled(!autostart::IsEnabled())) {
            MessageBoxW(hwnd, L"开机自启动设置写入失败。", kTitle, MB_ICONWARNING | MB_OK);
        }
    } else if (cmd == kMenuExit) {
        DestroyWindow(hwnd);
    }
}

// ---------------------------------------------------------------------------
// 快捷键
// ---------------------------------------------------------------------------
void ReloadHotkeys(HWND hwnd, State* st) {
    hotkey::UnregisterAll(hwnd);
    hotkey::Load(st->combos);
    st->step = hotkey::LoadStep();
    st->popupMs = hotkey::LoadPopupMs();

    st->hotkeyOutcome = hotkey::RegisterAll(hwnd, st->combos);
    if (!st->hotkeyOutcome.ok) {
        // 静默失败会让用户完全摸不着头脑，必须提示（FR-2）。
        // 但只能 Post：ReloadHotkeys 会在 WM_CREATE 中被调用，
        // 此处直接弹模态框会阻塞窗口创建。
        PostMessageW(hwnd, WM_HOTKEY_WARNING, 0, 0);
    }
}

void ShowHotkeyWarning(HWND hwnd, State* st) {
    wchar_t body[512] = L"以下快捷键已被其他程序占用，未能注册：\n";
    for (int i = 0; i < hotkey::kActionCount; ++i) {
        if (!st->hotkeyOutcome.failed[i]) continue;
        wchar_t combo[64];
        hotkey::ToString(st->combos[i], combo, _countof(combo));
        wchar_t line[160];
        swprintf_s(line, L"\n　· %s：%s（错误码 %lu）",
                   hotkey::ActionName(static_cast<hotkey::Action>(i)), combo,
                   st->hotkeyOutcome.error[i]);
        wcscat_s(body, line);
    }
    wcscat_s(body, L"\n\n其余快捷键不受影响，仍可正常使用。\n请在「设置」中为上述项改用其他组合键。");
    MessageBoxW(hwnd, body, kTitle, MB_ICONWARNING | MB_OK);
}

// ---------------------------------------------------------------------------
// 与系统状态同步
// ---------------------------------------------------------------------------
void SyncFromSystem(HWND hwnd, State* st) {
    st->hdr = sdr::IsHdrActive();

    int v = st->slider;
    st->status = sdr::GetSlider(v);
    if (st->status == sdr::Status::Ok) st->slider = v;

    SendMessageW(st->track, TBM_SETPOS, TRUE, st->slider);
    // 非 HDR 或读取失败时置灰，滑杆不可拖动（FR-3）
    EnableWindow(st->track, st->hdr && st->status == sdr::Status::Ok);
    RefreshTray(hwnd, st);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// 亮度写入
// ---------------------------------------------------------------------------
void FlushPending(HWND hwnd, State* st) {
    KillTimer(hwnd, kTimerThrottle);
    st->pending = false;
}

// 拖动过程中的写入：节流 + 不持久化
void OnThumbTrack(HWND hwnd, State* st, int value) {
    st->dragging = true;
    st->slider = value;
    InvalidateRect(hwnd, nullptr, FALSE);

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG elapsed = now - st->lastWriteTick;
    if (elapsed >= kThrottleMs) {
        FlushPending(hwnd, st);
        sdr::SetSlider(value, /*persist=*/false);
        st->lastWriteTick = now;
    } else if (!st->pending) {
        // 距上次写入不足 16ms，暂存并起一次性定时器合并
        st->pendingValue = value;
        st->pending = true;
        SetTimer(hwnd, kTimerThrottle, static_cast<UINT>(kThrottleMs - elapsed), nullptr);
    } else {
        st->pendingValue = value;
    }
}

// 离散操作（方向键、翻页、点击轨道）与拖动结束：立即写入并持久化
void ApplyFinal(HWND hwnd, State* st, int value) {
    FlushPending(hwnd, st);
    st->slider = value;
    st->status = sdr::SetSlider(value, /*persist=*/true);
    st->lastWriteTick = GetTickCount64();
    if (st->status != sdr::Status::Ok) {
        // 写入失败通常意味着 HDR 刚被关闭，重新同步状态
        st->hdr = sdr::IsHdrActive();
        EnableWindow(st->track, st->hdr && st->status == sdr::Status::Ok);
    }
    RefreshTray(hwnd, st);
    InvalidateRect(hwnd, nullptr, FALSE);
}

// 快捷键调节：与拖动同理，连发期间只实时生效不落盘，停手 400ms 后统一持久化
void ScheduleCommit(HWND hwnd, State* st) {
    st->commitPending = true;
    SetTimer(hwnd, kTimerCommit, kCommitDelayMs, nullptr);
}

void CommitNow(HWND hwnd, State* st) {
    KillTimer(hwnd, kTimerCommit);
    if (!st->commitPending) return;
    st->commitPending = false;
    sdr::SetSlider(st->slider, /*persist=*/true);
}

// 窗口收在托盘时把它弹出来，让用户看得到数值；停手后自动收回。
// 窗口已可见时不介入 —— 无论是用户自己开的还是上一次自动弹出的。
void PopupForHotkey(HWND hwnd, State* st) {
    if (!IsWindowVisible(hwnd)) {
        ShowNoActivate(hwnd);
        st->autoShown = true;
    }
    if (st->autoShown) ScheduleAutoHide(hwnd, st);
}

void OnHotkeyAdjust(HWND hwnd, State* st, int delta) {
    // 亮度可能已被系统设置页或其他途径改动，以系统当前值为准再增减
    int current = st->slider;
    if (sdr::GetSlider(current) == sdr::Status::Ok) st->slider = current;

    int next = st->slider + delta;
    if (next < sdr::kSliderMin) next = sdr::kSliderMin;
    if (next > sdr::kSliderMax) next = sdr::kSliderMax;
    // 已在边界，无需重复写入。但仍要刷新弹窗计时：用户还按着键，
    // 窗口不该在他手底下消失 —— 反馈以「按了键」为准，而非「值变了」。
    if (next == st->slider && st->commitPending) {
        PopupForHotkey(hwnd, st);
        return;
    }

    st->slider = next;
    st->status = sdr::SetSlider(next, /*persist=*/false);
    if (st->status != sdr::Status::Ok) {
        st->hdr = sdr::IsHdrActive();
        SyncFromSystem(hwnd, st);
        return;
    }

    ScheduleCommit(hwnd, st);
    SendMessageW(st->track, TBM_SETPOS, TRUE, st->slider);
    RefreshTray(hwnd, st);
    InvalidateRect(hwnd, nullptr, FALSE);
    PopupForHotkey(hwnd, st);
}

void OnHotkey(HWND hwnd, State* st, int id) {
    hotkey::Action action;
    if (!hotkey::ActionFromId(id, action)) return;

    if (action == hotkey::Action::Show) {
        // 纯开关：可见则隐藏，隐藏则显示。不区分窗口是自动弹出还是用户打开的 ——
        // 想留住自动弹出的窗口，点它一下即可（见 WM_MOUSEACTIVATE）
        ToggleWindow(hwnd);
        return;
    }

    // 非 HDR 时不执行任何亮度写入（FR-3）
    if (!sdr::IsHdrActive()) return;

    OnHotkeyAdjust(hwnd, st, action == hotkey::Action::Up ? st->step : -st->step);
}

void OnHScroll(HWND hwnd, State* st, WPARAM wp, LPARAM lp) {
    if (reinterpret_cast<HWND>(lp) != st->track) return;
    // 用户动了滑杆，窗口转为常驻 —— 不能在拖动途中把窗口收走
    CancelAutoHide(hwnd, st);
    const int code = LOWORD(wp);
    const int pos = static_cast<int>(SendMessageW(st->track, TBM_GETPOS, 0, 0));

    switch (code) {
        case TB_THUMBTRACK:
            OnThumbTrack(hwnd, st, pos);
            break;

        case TB_ENDTRACK:
            // 仅在真正拖动过之后才补一次持久化写入；
            // 键盘/翻页操作也会触发 ENDTRACK，但它们已在 default 分支写过了
            if (st->dragging) {
                st->dragging = false;
                ApplyFinal(hwnd, st, pos);
            }
            break;

        default:
            ApplyFinal(hwnd, st, pos);
            break;
    }
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------
void Paint(HWND hwnd, State* st, HDC dc) {
    RECT client;
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));

    const int w = client.right - client.left;
    const int m = Scale(st, kMargin);
    const bool usable = st->hdr && st->status == sdr::Status::Ok;

    SetBkMode(dc, TRANSPARENT);

    // 标题
    HGDIOBJ old = SelectObject(dc, st->fontUi);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    RECT r{m, Scale(st, kLabelY), w - m - Scale(st, kValueW), Scale(st, kLabelY + kLabelH)};
    DrawTextW(dc, kTitle, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 数值
    SelectObject(dc, st->fontValue);
    SetTextColor(dc, usable ? GetSysColor(COLOR_WINDOWTEXT) : GetSysColor(COLOR_GRAYTEXT));
    wchar_t value[16];
    swprintf_s(value, L"%d", st->slider);
    RECT rv{w - m - Scale(st, kValueW), Scale(st, kLabelY - 4), w - m,
            Scale(st, kLabelY + kLabelH + 4)};
    DrawTextW(dc, value, -1, &rv, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // 状态行
    SelectObject(dc, st->fontUi);
    const wchar_t* text;
    wchar_t buf[192];
    if (st->status != sdr::Status::Ok) {
        swprintf_s(buf, L"%s（系统返回码 %ld）", sdr::StatusText(st->status),
                   sdr::LastSystemError());
        text = buf;
        SetTextColor(dc, RGB(197, 48, 48));
    } else if (!st->hdr) {
        text = sdr::StatusText(sdr::Status::NotHdr);
        SetTextColor(dc, RGB(197, 108, 20));
    } else {
        text = L"HDR 已开启";
        SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
    }
    // 右侧要给设置按钮留位置
    RECT rs{m, Scale(st, kStatusY), w - m - Scale(st, kBtnW + 8),
            Scale(st, kStatusY + kStatusH)};
    DrawTextW(dc, text, -1, &rs, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(dc, old);
}

// ---------------------------------------------------------------------------
// 窗口过程
// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* st = new State();
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    State* st = Get(hwnd);
    if (!st) return DefWindowProcW(hwnd, msg, wp, lp);

    // -----------------------------------------------------------------------
    // 点窗口任意位置即接管为常驻（FR-7）
    //
    // 不能用 WM_MOUSEACTIVATE 来做：它仅在窗口「非活动」时发送。而 ShowTrayMenu
    // 会临时摘掉 WS_EX_NOACTIVATE 并 SetForegroundWindow，设置窗口销毁时系统也会
    // 把激活权交还给属主 —— 这两条路径之后本窗口就是活动窗口，点击不再产生该消息，
    // 表现为「偶尔点空白处/标题栏无法转常驻」。鼠标按下消息则与活动状态无关。
    // -----------------------------------------------------------------------
    switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_NCLBUTTONDOWN:
        case WM_NCRBUTTONDOWN:
        case WM_NCMBUTTONDOWN:
            CancelAutoHide(hwnd, st);
            break;

        // 子控件（滑杆、设置按钮）上的点击由属主收到本消息，同样不看活动状态
        case WM_PARENTNOTIFY:
            switch (LOWORD(wp)) {
                case WM_LBUTTONDOWN:
                case WM_RBUTTONDOWN:
                case WM_MBUTTONDOWN:
                    CancelAutoHide(hwnd, st);
                    break;
                default:
                    break;
            }
            break;

        default:
            break;
    }

    // explorer.exe 重启后重建托盘图标
    if (msg == tray::TaskbarCreatedMessage()) {
        st->trayAdded = tray::Add(hwnd, WM_TRAY_EVENT, st->iconOn, kTitle);
        RefreshTray(hwnd, st);
        return 0;
    }

    if (msg == ActivateMessage()) {
        if (IsWindowVisible(hwnd)) {
            CancelAutoHide(hwnd, st); // 已可见，仅接管为常驻
        } else {
            ToggleWindow(hwnd);
        }
        return 0;
    }

    // 设置窗口点了「确定」：重新加载配置并重注册热键
    if (msg == ui::SettingsAppliedMessage()) {
        ReloadHotkeys(hwnd, st);
        return 0;
    }

    switch (msg) {
        case WM_CREATE: {
            st->dpi = GetDpiForWindow(hwnd);
            RebuildFonts(st);
            RebuildIcons(hwnd, st);

            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            st->track = CreateWindowExW(
                0, TRACKBAR_CLASSW, nullptr,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_BOTH, 0, 0,
                0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdTrack)),
                cs->hInstance, nullptr);
            if (!st->track) return -1;

            SendMessageW(st->track, TBM_SETRANGE, TRUE,
                         MAKELPARAM(sdr::kSliderMin, sdr::kSliderMax));
            SendMessageW(st->track, TBM_SETLINESIZE, 0, 1);
            SendMessageW(st->track, TBM_SETPAGESIZE, 0, 10);

            st->btnSettings = CreateWindowExW(
                0, L"BUTTON", L"设置…", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0,
                hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdSettings)),
                cs->hInstance, nullptr);
            SendMessageW(st->btnSettings, WM_SETFONT, reinterpret_cast<WPARAM>(st->fontUi),
                         TRUE);

            st->trayAdded = tray::Add(hwnd, WM_TRAY_EVENT, st->iconOn, kTitle);

            ReloadHotkeys(hwnd, st);
            Layout(hwnd, st);
            SyncFromSystem(hwnd, st);
            return 0;
        }

        case WM_FIT_CONTENT:
            st->dpi = GetDpiForWindow(hwnd);
            RebuildFonts(st);
            RebuildIcons(hwnd, st);
            ResizeToContent(hwnd, st, nullptr); // 触发 WM_SIZE → Layout
            RestorePosition(hwnd);
            RefreshTray(hwnd, st);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;

        case WM_TRAY_EVENT:
            switch (LOWORD(lp)) {
                case NIN_SELECT:
                case NIN_KEYSELECT:
                    ToggleWindow(hwnd);
                    break;
                case WM_CONTEXTMENU:
                    ShowTrayMenu(hwnd, GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
                    break;
            }
            return 0;

        // 与 WS_EX_NOACTIVATE 双保险：托盘菜单期间会临时摘掉该扩展样式，
        // 那段时间里由本分支继续挡住鼠标点击导致的激活
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        // 防止全屏程序把本窗口压到下层。
        // 设置窗口打开时不再强制抬升：它是本窗口的属主窗口且同为置顶，
        // 继续抬升会把自己的对话框盖住。
        case WM_WINDOWPOSCHANGING: {
            if (!ui::SettingsWindow()) {
                auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
                pos->hwndInsertAfter = HWND_TOPMOST;
            }
            return 0;
        }

        case WM_SIZE:
            Layout(hwnd, st);
            return 0;

        case WM_EXITSIZEMOVE:
            SavePosition(hwnd);
            return 0;

        case WM_HSCROLL:
            OnHScroll(hwnd, st, wp, lp);
            return 0;

        case WM_HOTKEY:
            OnHotkey(hwnd, st, static_cast<int>(wp));
            return 0;

        case WM_HOTKEY_WARNING:
            ShowHotkeyWarning(hwnd, st);
            return 0;

        case WM_COMMAND:
            if (LOWORD(wp) == kIdSettings) {
                OpenSettings(hwnd);
                return 0;
            }
            break;

        case WM_TIMER:
            if (wp == kTimerThrottle) {
                KillTimer(hwnd, kTimerThrottle);
                if (st->pending) {
                    st->pending = false;
                    sdr::SetSlider(st->pendingValue, /*persist=*/false);
                    st->lastWriteTick = GetTickCount64();
                }
                return 0;
            }
            if (wp == kTimerCommit) {
                CommitNow(hwnd, st);
                return 0;
            }
            if (wp == kTimerAutoHide) {
                KillTimer(hwnd, kTimerAutoHide);
                st->autoShown = false;
                // 不调 SavePosition：自动弹出全程未移动窗口，位置无变化
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            }
            break;

        // 显示配置变化（含 HDR 开关切换）。事件驱动，全程无轮询。
        case WM_DISPLAYCHANGE:
            sdr::Invalidate();
            SyncFromSystem(hwnd, st);
            return 0;

        case WM_DPICHANGED: {
            st->dpi = HIWORD(wp);
            RebuildFonts(st);
            RebuildIcons(hwnd, st);
            RefreshTray(hwnd, st);
            // 采用系统建议的位置，但尺寸按内容重新计算
            ResizeToContent(hwnd, st, reinterpret_cast<const RECT*>(lp));
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        // 让滑杆背景与窗口背景一致
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));

        case WM_ERASEBKGND:
            return 1; // 背景在 WM_PAINT 中一次性绘制，避免闪烁

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            Paint(hwnd, st, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        // 关闭按钮不退出进程，收进托盘 —— 否则快捷键会跟着失效
        case WM_CLOSE:
            CancelAutoHide(hwnd, st);
            SavePosition(hwnd);
            ShowWindow(hwnd, SW_HIDE);
            return 0;

        case WM_DESTROY:
            SavePosition(hwnd);
            CancelAutoHide(hwnd, st);
            FlushPending(hwnd, st);
            CommitNow(hwnd, st); // 退出前把未落盘的快捷键调节写掉
            hotkey::UnregisterAll(hwnd);
            if (st->trayAdded) tray::Remove(hwnd);
            if (st->fontUi) DeleteObject(st->fontUi);
            if (st->fontValue) DeleteObject(st->fontValue);
            if (st->iconOn) DestroyIcon(st->iconOn);
            if (st->iconOff) DestroyIcon(st->iconOff);
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

const wchar_t* WindowClassName() {
    return kClassName;
}

UINT ActivateMessage() {
    static const UINT msg = RegisterWindowMessageW(L"SdrLuminance.Activate");
    return msg;
}

HWND CreateMainWindow(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // 自行绘制
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return nullptr;

    // TOPMOST   ：浮于全屏程序之上
    // TOOLWINDOW：不出现在任务栏与 Alt+Tab
    // NOACTIVATE：点击窗口不夺取前台焦点 —— FR-6 的关键。
    //   必须用扩展样式，不能只靠 WM_MOUSEACTIVATE 返回 MA_NOACTIVATE：
    //   后者只能否决「本窗口被激活」，系统在此之前已经把原前台窗口停用了，
    //   实测结果是前台变为 NULL，游戏同样会失焦。
    const DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    // 只保留标题栏与关闭按钮：不可缩放，关闭即收进托盘
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;

    // 尺寸先给占位值，创建完成后再按窗口实际所处显示器的 DPI 调整
    HWND hwnd = CreateWindowExW(exStyle, kClassName, kTitle, style, CW_USEDEFAULT,
                                CW_USEDEFAULT, kClientW, kClientH, nullptr, nullptr, inst,
                                nullptr);
    if (!hwnd) return nullptr;

    SendMessageW(hwnd, WM_FIT_CONTENT, 0, 0);
    return hwnd;
}

} // namespace ui
