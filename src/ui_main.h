// ui_main.h — 主窗口：滑杆、数值显示、状态提示、托盘
#pragma once

#include <windows.h>

namespace ui {

// 注册窗口类并创建主窗口。失败返回 nullptr。
HWND CreateMainWindow(HINSTANCE inst);

// 窗口类名。第二个实例用它 FindWindow 定位已有窗口。
const wchar_t* WindowClassName();

// 跨实例唤起消息。第二个实例向已有窗口发送它以唤起窗口后自行退出。
UINT ActivateMessage();

} // namespace ui
