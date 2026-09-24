#include "autostart.h"

#include <cstdio>
#include <cwchar>
#include <stdlib.h>

namespace autostart {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"SdrLuminance";

// --tray：自启动时直接隐藏到托盘，不弹窗口
bool BuildCommand(wchar_t* out, size_t cch) {
    wchar_t exe[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    swprintf_s(out, cch, L"\"%s\" --tray", exe);
    return true;
}

// 读取当前登记的命令行；不存在返回 false
bool ReadCommand(wchar_t* out, DWORD cch) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD bytes = cch * sizeof(wchar_t);
    const LSTATUS rc =
        RegQueryValueExW(key, kValueName, nullptr, &type, reinterpret_cast<BYTE*>(out), &bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ) return false;

    // RegQueryValueEx 不保证串以 \0 结尾
    const DWORD chars = bytes / sizeof(wchar_t);
    out[chars < cch ? chars : cch - 1] = L'\0';
    return true;
}

} // namespace

bool IsEnabled() {
    wchar_t cmd[MAX_PATH + 32];
    return ReadCommand(cmd, _countof(cmd));
}

bool SetEnabled(bool enable) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        return false;
    }

    LSTATUS rc;
    if (enable) {
        wchar_t cmd[MAX_PATH + 32];
        if (!BuildCommand(cmd, _countof(cmd))) {
            RegCloseKey(key);
            return false;
        }
        const DWORD bytes = static_cast<DWORD>((wcslen(cmd) + 1) * sizeof(wchar_t));
        rc = RegSetValueExW(key, kValueName, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(cmd), bytes);
    } else {
        rc = RegDeleteValueW(key, kValueName);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS; // 本就未启用，视作成功
    }

    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

void RefreshPath() {
    wchar_t current[MAX_PATH + 32];
    if (!ReadCommand(current, _countof(current))) return; // 未启用，无需处理

    wchar_t expected[MAX_PATH + 32];
    if (!BuildCommand(expected, _countof(expected))) return;

    if (_wcsicmp(current, expected) != 0) SetEnabled(true);
}

} // namespace autostart
