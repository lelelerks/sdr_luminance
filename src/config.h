// config.h — ini 配置读写
//
// 配置文件位于 %LOCALAPPDATA%\SdrLuminance\config.ini。
// 选择该位置而非程序同目录，是为避免程序被放入 Program Files 时无写入权限。
#pragma once

#include <windows.h>

namespace config {

// 解析配置文件路径并确保目录存在。须在任何读写之前调用一次。
void Init();

int GetInt(const wchar_t* key, int fallback);
void SetInt(const wchar_t* key, int value);

void GetString(const wchar_t* key, wchar_t* buf, DWORD cch, const wchar_t* fallback);
void SetString(const wchar_t* key, const wchar_t* value);

} // namespace config
