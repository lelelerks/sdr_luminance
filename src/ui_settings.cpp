#include "ui_settings.h"

#include <commctrl.h>

#include <cstdio>
#include <cwchar>
#include <stdlib.h>

#include "autostart.h"
#include "hotkey.h"

namespace ui {
namespace {

constexpr wchar_t kClassName[] = L"SdrLuminanceSettingsWnd";
constexpr wchar_t kTitle[] = L"设置";

// 布局常量，单位为 96 DPI 下的逻辑像素
constexpr int kClientW = 386;
constexpr int kClientH = 320;
constexpr int kMargin = 16;
constexpr int kLabelW = 92;
constexpr int kGap = 8;
constexpr int kRowH = 28;
constexpr int kRowStep = 36;
constexpr int kRow0Y = 18;
constexpr int kStepEditW = 64;
constexpr int kCheckY = 200;
constexpr int kCheckH = 24;
constexpr int kHintY = 234;
constexpr int kHintH = 36;
constexpr int kBtnY = 276;
constexpr int kBtnW = 78;
constexpr int kBtnH = 28;

constexpr int kIdRecordBase = 2001; // 2001..2003 对应三个动作
constexpr int kIdStep = 2010;
constexpr int kIdAutostart = 2011;
constexpr int kIdPopup = 2012;
constexpr int kIdOk = IDOK;
constexpr int kIdCancel = IDCANCEL;

const wchar_t* kRowLabels[hotkey::kActionCount] = {L"调亮快捷键", L"调暗快捷键",
                                                   L"唤起窗口"};

struct State {
    HWND owner = nullptr;
    HWND record[hotkey::kActionCount]{};
    HWND stepEdit = nullptr;
    HWND popupEdit = nullptr;
    HWND autostart = nullptr;
    // 三个快捷键行标签 + 步进与弹窗停留两行各自的「标签 + 范围提示」
    HWND labels[hotkey::kActionCount + 4]{};
    HWND btnOk = nullptr;
    HWND btnCancel = nullptr;
    HFONT font = nullptr;
    UINT dpi = 96;
};

HWND g_window = nullptr;

State* Get(HWND hwnd) {
    return reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

int Scale(UINT dpi, int v) {
    return MulDiv(v, static_cast<int>(dpi), 96);
}

// 组合键打包进控件的 GWLP_USERDATA，避免额外的并行数组
LONG_PTR Pack(const hotkey::Combo& c) {
    return static_cast<LONG_PTR>((static_cast<ULONG_PTR>(c.mods) << 16) | c.vk);
}

hotkey::Combo Unpack(LONG_PTR v) {
    hotkey::Combo c;
    c.mods = static_cast<UINT>((static_cast<ULONG_PTR>(v) >> 16) & 0xFFFF);
    c.vk = static_cast<UINT>(static_cast<ULONG_PTR>(v) & 0xFFFF);
    return c;
}

void SetCombo(HWND edit, const hotkey::Combo& c) {
    SetWindowLongPtrW(edit, GWLP_USERDATA, Pack(c));
    wchar_t text[64];
    hotkey::ToString(c, text, _countof(text));
    SetWindowTextW(edit, text);
}

hotkey::Combo GetCombo(HWND edit) {
    return Unpack(GetWindowLongPtrW(edit, GWLP_USERDATA));
}

UINT CurrentMods() {
    UINT m = 0;
    if (GetKeyState(VK_CONTROL) < 0) m |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) < 0) m |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) < 0) m |= MOD_SHIFT;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) m |= MOD_WIN;
    return m;
}

bool IsModifierKey(UINT vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_MENU ||
           vk == VK_LMENU || vk == VK_RMENU || vk == VK_SHIFT || vk == VK_LSHIFT ||
           vk == VK_RSHIFT || vk == VK_LWIN || vk == VK_RWIN;
}

// ---------------------------------------------------------------------------
// 录制控件：子类化的只读 EDIT，按下即录
// ---------------------------------------------------------------------------
LRESULT CALLBACK RecordProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
        // 要走键盘全部按键，否则方向键、回车等会被对话框管理器截走
        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const UINT vk = static_cast<UINT>(wp);
            if (IsModifierKey(vk)) return 0; // 单独按修饰键不构成组合

            const UINT mods = CurrentMods();

            // 无修饰键时保留这几个键的原有语义，否则窗口就没法用键盘操作了
            if (mods == 0) {
                if (vk == VK_TAB) {
                    HWND next = GetNextDlgTabItem(GetParent(hwnd), hwnd,
                                                  GetKeyState(VK_SHIFT) < 0);
                    if (next) SetFocus(next);
                    return 0;
                }
                if (vk == VK_ESCAPE) {
                    SendMessageW(GetParent(hwnd), WM_COMMAND, kIdCancel, 0);
                    return 0;
                }
                if (vk == VK_RETURN) {
                    SendMessageW(GetParent(hwnd), WM_COMMAND, kIdOk, 0);
                    return 0;
                }
                if (vk == VK_BACK || vk == VK_DELETE) {
                    SetCombo(hwnd, hotkey::Combo{}); // 清除绑定
                    return 0;
                }
            }

            SetCombo(hwnd, hotkey::Combo{mods, vk});
            return 0;
        }

        case WM_KEYUP:
        case WM_SYSKEYUP:
            return 0;

        // 抑制系统蜂鸣：只读 EDIT 收到字符输入会响
        case WM_CHAR:
        case WM_SYSCHAR:
            return 0;

        case WM_SETFOCUS: {
            LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
            SendMessageW(hwnd, EM_SETSEL, 0, 0); // 不显示选中高亮
            return r;
        }

        case WM_NCDESTROY:
            RemoveWindowSubclass(hwnd, RecordProc, 0);
            break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
HFONT MakeFont(UINT dpi) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) {
        return reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

void ApplyFont(HWND hwnd, HFONT font) {
    if (hwnd) SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

// 步进值与弹窗停留两行结构完全相同：[标签][窄数值框][范围提示]
void LayoutNumberRow(State* st, int w, int row, int labelIdx, HWND edit) {
    const UINT d = st->dpi;
    const int m = Scale(d, kMargin);
    const int labelW = Scale(d, kLabelW);
    const int editX = m + labelW + Scale(d, kGap);
    const int y = Scale(d, kRow0Y + kRowStep * row);
    const int hintX = editX + Scale(d, kStepEditW + kGap);

    SetWindowPos(st->labels[labelIdx], nullptr, m, y + Scale(d, 5), labelW, Scale(d, 20),
                 SWP_NOZORDER);
    SetWindowPos(edit, nullptr, editX, y, Scale(d, kStepEditW), Scale(d, kRowH), SWP_NOZORDER);
    SetWindowPos(st->labels[labelIdx + 1], nullptr, hintX, y + Scale(d, 5), w - hintX - m,
                 Scale(d, 20), SWP_NOZORDER);
}

void LayoutAll(HWND hwnd, State* st) {
    const UINT d = st->dpi;
    RECT client;
    GetClientRect(hwnd, &client);
    const int w = client.right - client.left;
    const int m = Scale(d, kMargin);
    const int labelW = Scale(d, kLabelW);
    const int editX = m + labelW + Scale(d, kGap);
    const int editW = w - editX - m;

    for (int i = 0; i < hotkey::kActionCount; ++i) {
        const int y = Scale(d, kRow0Y + kRowStep * i);
        SetWindowPos(st->labels[i], nullptr, m, y + Scale(d, 5), labelW, Scale(d, 20),
                     SWP_NOZORDER);
        SetWindowPos(st->record[i], nullptr, editX, y, editW, Scale(d, kRowH), SWP_NOZORDER);
    }

    LayoutNumberRow(st, w, hotkey::kActionCount, hotkey::kActionCount, st->stepEdit);
    LayoutNumberRow(st, w, hotkey::kActionCount + 1, hotkey::kActionCount + 2, st->popupEdit);

    SetWindowPos(st->autostart, nullptr, m, Scale(d, kCheckY), w - 2 * m, Scale(d, kCheckH),
                 SWP_NOZORDER);

    const int btnY = Scale(d, kBtnY);
    const int btnW = Scale(d, kBtnW);
    const int btnH = Scale(d, kBtnH);
    SetWindowPos(st->btnCancel, nullptr, w - m - btnW, btnY, btnW, btnH, SWP_NOZORDER);
    SetWindowPos(st->btnOk, nullptr, w - m - btnW * 2 - Scale(d, kGap), btnY, btnW, btnH,
                 SWP_NOZORDER);
}

void ResizeToContent(HWND hwnd, State* st) {
    RECT rc{0, 0, Scale(st->dpi, kClientW), Scale(st->dpi, kClientH)};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    AdjustWindowRectExForDpi(&rc, style, FALSE, exStyle, st->dpi);
    SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                 SWP_NOMOVE | SWP_NOZORDER);
}

void CenterOnOwner(HWND hwnd, HWND owner) {
    RECT me, ow;
    if (!GetWindowRect(hwnd, &me)) return;
    if (!owner || !IsWindowVisible(owner) || !GetWindowRect(owner, &ow)) {
        // 主窗口隐藏时以鼠标所在显示器为准
        POINT pt;
        GetCursorPos(&pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        ow = mi.rcWork;
    }
    const int w = me.right - me.left;
    const int h = me.bottom - me.top;
    int x = ow.left + ((ow.right - ow.left) - w) / 2;
    int y = ow.top + ((ow.bottom - ow.top) - h) / 2;

    // 主窗口靠边时，居中后的设置窗口会有一部分落到屏幕外。
    // 不能用 MonitorFromRect + MONITOR_DEFAULTTONULL 来判断：它只在矩形与所有
    // 显示器都毫无交集时才返回 null，「部分越界」根本挡不住。
    // 改为取最近的显示器，把窗口整体推回其工作区（用 rcWork 以避开任务栏）。
    RECT target{x, y, x + w, y + h};
    MONITORINFO wi{sizeof(wi)};
    if (GetMonitorInfoW(MonitorFromRect(&target, MONITOR_DEFAULTTONEAREST), &wi)) {
        // 先压右/下再压左/上，顺序不能反：窗口比工作区还大时后判的左上界胜出，
        // 至少保住标题栏与「确定」按钮，而不是把它们推到屏幕外
        if (x + w > wi.rcWork.right) x = wi.rcWork.right - w;
        if (y + h > wi.rcWork.bottom) y = wi.rcWork.bottom - h;
        if (x < wi.rcWork.left) x = wi.rcWork.left;
        if (y < wi.rcWork.top) y = wi.rcWork.top;
    }
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

// ---------------------------------------------------------------------------
// 确定：校验、写盘、通知主窗口
// ---------------------------------------------------------------------------
bool ApplyAndClose(HWND hwnd, State* st) {
    hotkey::Combo combos[hotkey::kActionCount];
    for (int i = 0; i < hotkey::kActionCount; ++i) {
        combos[i] = GetCombo(st->record[i]);
    }

    // 组合键不得互相重复，否则后注册的那个必然失败
    for (int i = 0; i < hotkey::kActionCount; ++i) {
        if (!combos[i].Valid()) continue;
        for (int j = i + 1; j < hotkey::kActionCount; ++j) {
            if (combos[j].Valid() && combos[i] == combos[j]) {
                wchar_t msg[256];
                swprintf_s(msg, L"「%s」与「%s」使用了相同的组合键，请改用不同的组合。",
                           kRowLabels[i], kRowLabels[j]);
                MessageBoxW(hwnd, msg, kTitle, MB_ICONWARNING | MB_OK);
                SetFocus(st->record[j]);
                return false;
            }
        }
    }

    // 先试注册，确认不与其他程序冲突后才写盘。
    // 主窗口的热键此刻已被解除，不会与自己冲突。
    const hotkey::RegisterOutcome outcome = hotkey::RegisterAll(st->owner, combos);
    // 无论成败都先解除：成功时由主窗口在收到销毁通知后统一重注册，
    // 避免热键的所有权散落在两处。
    hotkey::UnregisterAll(st->owner);

    if (!outcome.ok) {
        int first = -1;
        wchar_t body[512] = L"以下组合键已被其他程序占用，无法注册：\n";
        for (int i = 0; i < hotkey::kActionCount; ++i) {
            if (!outcome.failed[i]) continue;
            if (first < 0) first = i;
            wchar_t combo[64];
            hotkey::ToString(combos[i], combo, _countof(combo));
            wchar_t line[160];
            swprintf_s(line, L"\n　· %s：%s（错误码 %lu）", kRowLabels[i], combo,
                       outcome.error[i]);
            wcscat_s(body, line);
        }
        wcscat_s(body, L"\n\n请改用其他组合键。原有设置未被修改。");
        MessageBoxW(hwnd, body, kTitle, MB_ICONWARNING | MB_OK);
        if (first >= 0) SetFocus(st->record[first]);
        return false;
    }

    // Save* 内部会 clamp，此处直接透传原始输入即可
    hotkey::Save(combos);
    hotkey::SaveStep(static_cast<int>(GetDlgItemInt(hwnd, kIdStep, nullptr, FALSE)));
    hotkey::SavePopupMs(static_cast<int>(GetDlgItemInt(hwnd, kIdPopup, nullptr, FALSE)));

    const bool wantAutostart =
        SendMessageW(st->autostart, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (wantAutostart != autostart::IsEnabled() && !autostart::SetEnabled(wantAutostart)) {
        MessageBoxW(hwnd, L"开机自启动设置写入失败，其余设置已保存。", kTitle,
                    MB_ICONWARNING | MB_OK);
    }

    // 不在此处通知主窗口：统一在 WM_DESTROY 里发，
    // 这样「取消」路径也能让主窗口把热键重新注册回来。
    DestroyWindow(hwnd);
    return true;
}

// ---------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* st = new State();
        st->owner = reinterpret_cast<HWND>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    State* st = Get(hwnd);
    if (!st) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            HINSTANCE inst = cs->hInstance;
            st->dpi = GetDpiForWindow(hwnd);
            st->font = MakeFont(st->dpi);

            hotkey::Combo combos[hotkey::kActionCount];
            hotkey::Load(combos);

            for (int i = 0; i < hotkey::kActionCount; ++i) {
                st->labels[i] = CreateWindowExW(0, L"STATIC", kRowLabels[i],
                                                WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0,
                                                hwnd, nullptr, inst, nullptr);
                st->record[i] = CreateWindowExW(
                    WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_READONLY |
                        ES_AUTOHSCROLL,
                    0, 0, 0, 0, hwnd,
                    reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdRecordBase + i)), inst,
                    nullptr);
                SetWindowSubclass(st->record[i], RecordProc, 0, 0);
                SetCombo(st->record[i], combos[i]);
            }

            st->labels[hotkey::kActionCount] =
                CreateWindowExW(0, L"STATIC", L"调节步进值", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                0, 0, 0, 0, hwnd, nullptr, inst, nullptr);
            st->stepEdit = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_LEFT, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdStep)), inst, nullptr);
            SetDlgItemInt(hwnd, kIdStep, static_cast<UINT>(hotkey::LoadStep()), FALSE);

            wchar_t range[64];
            swprintf_s(range, L"（%d - %d）", hotkey::kStepMin, hotkey::kStepMax);
            st->labels[hotkey::kActionCount + 1] =
                CreateWindowExW(0, L"STATIC", range, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0,
                                0, hwnd, nullptr, inst, nullptr);

            st->labels[hotkey::kActionCount + 2] =
                CreateWindowExW(0, L"STATIC", L"弹窗停留", WS_CHILD | WS_VISIBLE | SS_LEFT, 0,
                                0, 0, 0, hwnd, nullptr, inst, nullptr);
            st->popupEdit = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_LEFT, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdPopup)), inst, nullptr);
            SetDlgItemInt(hwnd, kIdPopup, static_cast<UINT>(hotkey::LoadPopupMs()), FALSE);

            swprintf_s(range, L"（%d - %d 毫秒）", hotkey::kPopupMsMin, hotkey::kPopupMsMax);
            st->labels[hotkey::kActionCount + 3] =
                CreateWindowExW(0, L"STATIC", range, WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0,
                                0, hwnd, nullptr, inst, nullptr);

            st->autostart = CreateWindowExW(
                0, L"BUTTON", L"开机时自动启动（启动后直接收进托盘）",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdAutostart)), inst, nullptr);
            // 以注册表为唯一数据源，每次打开都实时查询
            SendMessageW(st->autostart, BM_SETCHECK,
                         autostart::IsEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);

            st->btnOk = CreateWindowExW(
                0, L"BUTTON", L"确定",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0, hwnd,
                reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdOk)), inst, nullptr);
            st->btnCancel = CreateWindowExW(
                0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kIdCancel)),
                inst, nullptr);

            for (HWND h : st->labels) ApplyFont(h, st->font);
            for (HWND h : st->record) ApplyFont(h, st->font);
            ApplyFont(st->stepEdit, st->font);
            ApplyFont(st->popupEdit, st->font);
            ApplyFont(st->autostart, st->font);
            ApplyFont(st->btnOk, st->font);
            ApplyFont(st->btnCancel, st->font);

            LayoutAll(hwnd, st);
            return 0;
        }

        case WM_SIZE:
            LayoutAll(hwnd, st);
            return 0;

        case WM_DPICHANGED: {
            st->dpi = HIWORD(wp);
            if (st->font) DeleteObject(st->font);
            st->font = MakeFont(st->dpi);
            for (HWND h : st->labels) ApplyFont(h, st->font);
            for (HWND h : st->record) ApplyFont(h, st->font);
            ApplyFont(st->stepEdit, st->font);
            ApplyFont(st->popupEdit, st->font);
            ApplyFont(st->autostart, st->font);
            ApplyFont(st->btnOk, st->font);
            ApplyFont(st->btnCancel, st->font);
            ResizeToContent(hwnd, st);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client;
            GetClientRect(hwnd, &client);
            FillRect(dc, &client, GetSysColorBrush(COLOR_3DFACE));

            // 提示文字直接绘制，省一个控件
            HGDIOBJ old = SelectObject(dc, st->font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
            RECT r{Scale(st->dpi, kMargin), Scale(st->dpi, kHintY),
                   client.right - Scale(st->dpi, kMargin),
                   Scale(st->dpi, kHintY + kHintH)};
            DrawTextW(dc,
                      L"点击输入框后直接按下组合键即可录制。\n"
                      L"按 Backspace 或 Delete 清除该项绑定。",
                      -1, &r, DT_LEFT | DT_TOP | DT_WORDBREAK);
            SelectObject(dc, old);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_3DFACE));

        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case kIdOk:
                    ApplyAndClose(hwnd, st);
                    return 0;
                case kIdCancel:
                    DestroyWindow(hwnd);
                    return 0;
            }
            break;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY: {
            // 确定与取消都走这里。主窗口收到后重新从配置加载并注册热键 ——
            // 取消时配置未变，等效于把打开设置前解除的热键原样恢复。
            HWND owner = st->owner;
            if (st->font) DeleteObject(st->font);
            delete st;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            g_window = nullptr;
            if (owner) PostMessageW(owner, SettingsAppliedMessage(), 0, 0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool EnsureClass(HINSTANCE inst) {
    static bool registered = false;
    if (registered) return true;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    registered = RegisterClassExW(&wc) != 0;
    return registered;
}

} // namespace

HWND SettingsWindow() {
    return g_window;
}

UINT SettingsAppliedMessage() {
    static const UINT msg = RegisterWindowMessageW(L"SdrLuminance.SettingsApplied");
    return msg;
}

void OpenSettings(HWND owner, HINSTANCE inst) {
    if (g_window) {
        SetForegroundWindow(g_window);
        return;
    }
    if (!EnsureClass(inst)) return;

    // 置顶以免被同为置顶的主窗口挡住；
    // 不加 WS_EX_NOACTIVATE —— 录制快捷键必须能接收键盘输入
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName, kTitle,
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
                                CW_USEDEFAULT, kClientW, kClientH, owner, nullptr, inst,
                                owner);
    if (!hwnd) return;

    g_window = hwnd;
    State* st = Get(hwnd);
    if (st) {
        st->dpi = GetDpiForWindow(hwnd);
        ResizeToContent(hwnd, st);
        LayoutAll(hwnd, st);
    }
    CenterOnOwner(hwnd, owner);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

} // namespace ui
