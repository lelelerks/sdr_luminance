#include "config.h"

#include <cstdio>

namespace config {
namespace {

constexpr wchar_t kSection[] = L"General";
constexpr wchar_t kFolderName[] = L"SdrLuminance";
constexpr wchar_t kFileName[] = L"config.ini";

wchar_t g_path[MAX_PATH] = L"";

// 退路：程序所在目录。仅在环境变量缺失这种极端情况下使用。
bool ExeDirectory(wchar_t* out, DWORD cch) {
    if (!GetModuleFileNameW(nullptr, out, cch)) return false;
    wchar_t* slash = wcsrchr(out, L'\\');
    if (!slash) return false;
    *slash = L'\0';
    return true;
}

} // namespace

void Init() {
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) &&
        !GetEnvironmentVariableW(L"APPDATA", base, MAX_PATH) &&
        !ExeDirectory(base, MAX_PATH)) {
        return; // g_path 保持为空，后续读写均走 fallback 且不落盘
    }

    wchar_t dir[MAX_PATH];
    swprintf_s(dir, L"%s\\%s", base, kFolderName);
    CreateDirectoryW(dir, nullptr); // 已存在时返回 ERROR_ALREADY_EXISTS，忽略
    swprintf_s(g_path, L"%s\\%s", dir, kFileName);
}

int GetInt(const wchar_t* key, int fallback) {
    if (!g_path[0]) return fallback;
    return static_cast<int>(GetPrivateProfileIntW(kSection, key, fallback, g_path));
}

void SetInt(const wchar_t* key, int value) {
    wchar_t buf[32];
    swprintf_s(buf, L"%d", value);
    SetString(key, buf);
}

void GetString(const wchar_t* key, wchar_t* buf, DWORD cch, const wchar_t* fallback) {
    if (!g_path[0]) {
        wcscpy_s(buf, cch, fallback);
        return;
    }
    GetPrivateProfileStringW(kSection, key, fallback, buf, cch, g_path);
}

void SetString(const wchar_t* key, const wchar_t* value) {
    if (!g_path[0]) return;
    WritePrivateProfileStringW(kSection, key, value, g_path);
}

} // namespace config
