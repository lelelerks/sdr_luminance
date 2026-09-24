// tray.h — 托盘图标
#pragma once

#include <windows.h>

namespace tray {

// 添加托盘图标。callbackMsg 为自定义消息，事件通过它回送到 hwnd。
bool Add(HWND hwnd, UINT callbackMsg, HICON iconHandle, const wchar_t* tip);

// 更新图标与提示文字（用于 HDR 状态变化、亮度值变化）
void Update(HWND hwnd, HICON iconHandle, const wchar_t* tip);

void Remove(HWND hwnd);

// explorer.exe 崩溃重启后会广播此消息，收到时须重建图标
UINT TaskbarCreatedMessage();

} // namespace tray
