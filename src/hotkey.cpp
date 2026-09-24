#include "hotkey.h"

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <stdlib.h>

#include "config.h"

namespace hotkey {
namespace {

// 热键 id 与 Action 一一对应，从 1 起（RegisterHotKey 的 id 需大于 0）
int IdOf(Action a) {
    return static_cast<int>(a) + 1;
}

struct KeyName {
    UINT vk;
    const wchar_t* name;
};

// 仅收录不能由字符/序号规则推导出的按键
constexpr KeyName kNames[] = {
    {VK_UP, L"Up"},          {VK_DOWN, L"Down"},      {VK_LEFT, L"Left"},
    {VK_RIGHT, L"Right"},    {VK_PRIOR, L"PageUp"},   {VK_NEXT, L"PageDown"},
    {VK_HOME, L"Home"},      {VK_END, L"End"},        {VK_INSERT, L"Insert"},
    {VK_DELETE, L"Delete"},  {VK_SPACE, L"Space"},    {VK_RETURN, L"Enter"},
    {VK_TAB, L"Tab"},        {VK_ESCAPE, L"Esc"},     {VK_BACK, L"Backspace"},
    {VK_OEM_PLUS, L"="},     {VK_OEM_MINUS, L"-"},    {VK_OEM_COMMA, L","},
    {VK_OEM_PERIOD, L"."},   {VK_OEM_1, L";"},        {VK_OEM_2, L"/"},
    {VK_OEM_3, L"`"},        {VK_OEM_4, L"["},        {VK_OEM_5, L"\\"},
    {VK_OEM_6, L"]"},        {VK_OEM_7, L"'"},        {VK_ADD, L"Num+"},
    {VK_SUBTRACT, L"Num-"},  {VK_MULTIPLY, L"Num*"},  {VK_DIVIDE, L"Num/"},
};

void KeyToString(UINT vk, wchar_t* buf, size_t cch) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        swprintf_s(buf, cch, L"%c", static_cast<wchar_t>(vk));
        return;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        swprintf_s(buf, cch, L"F%u", vk - VK_F1 + 1);
        return;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        swprintf_s(buf, cch, L"Num%u", vk - VK_NUMPAD0);
        return;
    }
    for (const KeyName& kn : kNames) {
        if (kn.vk == vk) {
            wcscpy_s(buf, cch, kn.name);
            return;
        }
    }
    swprintf_s(buf, cch, L"0x%02X", vk); // 兜底，保证可往返
}

UINT KeyFromString(const wchar_t* s) {
    if (!s || !s[0]) return 0;

    if (!s[1]) {
        const wchar_t c = s[0];
        if (c >= L'a' && c <= L'z') return static_cast<UINT>(c - L'a' + 'A');
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) return static_cast<UINT>(c);
    }

    if ((s[0] == L'F' || s[0] == L'f') && s[1] >= L'0' && s[1] <= L'9') {
        const int n = _wtoi(s + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }

    if (_wcsnicmp(s, L"Num", 3) == 0 && s[3] >= L'0' && s[3] <= L'9' && !s[4]) {
        return VK_NUMPAD0 + (s[3] - L'0');
    }

    for (const KeyName& kn : kNames) {
        if (_wcsicmp(kn.name, s) == 0) return kn.vk;
    }

    if (s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')) {
        return static_cast<UINT>(wcstoul(s + 2, nullptr, 16));
    }
    return 0;
}

const wchar_t* kConfigKeys[kActionCount] = {L"HotkeyUp", L"HotkeyDown", L"HotkeyShow"};
constexpr wchar_t kStepKey[] = L"Step";
constexpr wchar_t kPopupMsKey[] = L"PopupMs";

} // namespace

void ToString(const Combo& c, wchar_t* buf, size_t cch) {
    if (!c.Valid()) {
        wcscpy_s(buf, cch, L"未设置");
        return;
    }
    buf[0] = L'\0';
    if (c.mods & MOD_CONTROL) wcscat_s(buf, cch, L"Ctrl+");
    if (c.mods & MOD_ALT) wcscat_s(buf, cch, L"Alt+");
    if (c.mods & MOD_SHIFT) wcscat_s(buf, cch, L"Shift+");
    if (c.mods & MOD_WIN) wcscat_s(buf, cch, L"Win+");

    wchar_t key[32];
    KeyToString(c.vk, key, _countof(key));
    wcscat_s(buf, cch, key);
}

Combo FromString(const wchar_t* s) {
    Combo c;
    if (!s || !s[0]) return c;

    // 从前往后贪婪匹配修饰键前缀。不能简单按 '+' 分割 ——
    // 键名本身可能就是 "+" 或 "Num+"，分割会把它切碎。
    const wchar_t* p = s;
    for (;;) {
        if (_wcsnicmp(p, L"Ctrl+", 5) == 0) {
            c.mods |= MOD_CONTROL;
            p += 5;
        } else if (_wcsnicmp(p, L"Alt+", 4) == 0) {
            c.mods |= MOD_ALT;
            p += 4;
        } else if (_wcsnicmp(p, L"Shift+", 6) == 0) {
            c.mods |= MOD_SHIFT;
            p += 6;
        } else if (_wcsnicmp(p, L"Win+", 4) == 0) {
            c.mods |= MOD_WIN;
            p += 4;
        } else {
            break;
        }
    }

    c.vk = KeyFromString(p);
    if (!c.vk) c.mods = 0;
    return c;
}

RegisterOutcome RegisterAll(HWND hwnd, const Combo* combos) {
    RegisterOutcome out;
    for (int i = 0; i < kActionCount; ++i) {
        const Combo& c = combos[i];
        if (!c.Valid()) continue; // 允许留空不绑定

        // 不加 MOD_NOREPEAT：让系统的键盘重复自然实现长按连发，零成本
        if (RegisterHotKey(hwnd, IdOf(static_cast<Action>(i)), c.mods, c.vk)) continue;

        out.ok = false;
        out.failed[i] = true;
        out.error[i] = GetLastError();
    }
    return out;
}

const wchar_t* ActionName(Action a) {
    switch (a) {
        case Action::Up:   return L"调亮快捷键";
        case Action::Down: return L"调暗快捷键";
        case Action::Show: return L"唤起窗口";
        default:           return L"";
    }
}

void UnregisterAll(HWND hwnd) {
    for (int i = 0; i < kActionCount; ++i) {
        UnregisterHotKey(hwnd, IdOf(static_cast<Action>(i)));
    }
}

bool ActionFromId(int id, Action& out) {
    if (id < 1 || id > kActionCount) return false;
    out = static_cast<Action>(id - 1);
    return true;
}

Combo Default(Action a) {
    // 不用 Ctrl+Alt+↑/↓：这两个组合被常驻类软件（媒体播放器、启动器等）
    // 大量占用，开发机上实测即被占（RegisterHotKey 返回 1409）。
    // PageUp/PageDown 语义相近，冲突概率低得多。
    // 无论默认值如何，冲突都由注册失败提示 + 设置页改绑来兜底。
    switch (a) {
        case Action::Up:   return Combo{MOD_CONTROL | MOD_ALT, VK_PRIOR};
        case Action::Down: return Combo{MOD_CONTROL | MOD_ALT, VK_NEXT};
        case Action::Show: return Combo{MOD_CONTROL | MOD_ALT, 'B'};
        default:           return Combo{};
    }
}

void Load(Combo* out) {
    for (int i = 0; i < kActionCount; ++i) {
        wchar_t buf[64];
        wchar_t def[64];
        ToString(Default(static_cast<Action>(i)), def, _countof(def));
        config::GetString(kConfigKeys[i], buf, _countof(buf), def);
        out[i] = FromString(buf);
    }
}

void Save(const Combo* combos) {
    for (int i = 0; i < kActionCount; ++i) {
        wchar_t buf[64];
        ToString(combos[i], buf, _countof(buf));
        config::SetString(kConfigKeys[i], combos[i].Valid() ? buf : L"");
    }
}

int LoadStep() {
    int v = config::GetInt(kStepKey, kStepDefault);
    if (v < kStepMin) v = kStepMin;
    if (v > kStepMax) v = kStepMax;
    return v;
}

void SaveStep(int step) {
    if (step < kStepMin) step = kStepMin;
    if (step > kStepMax) step = kStepMax;
    config::SetInt(kStepKey, step);
}

int LoadPopupMs() {
    int v = config::GetInt(kPopupMsKey, kPopupMsDefault);
    if (v < kPopupMsMin) v = kPopupMsMin;
    if (v > kPopupMsMax) v = kPopupMsMax;
    return v;
}

void SavePopupMs(int ms) {
    if (ms < kPopupMsMin) ms = kPopupMsMin;
    if (ms > kPopupMsMax) ms = kPopupMsMax;
    config::SetInt(kPopupMsKey, ms);
}

} // namespace hotkey
