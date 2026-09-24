// ui_settings.h — 设置窗口：快捷键录制、步进值
#pragma once

#include <windows.h>

namespace ui {

// 打开设置窗口（同一时刻只允许一个）。owner 为主窗口。
// 确定时会写入配置并向 owner 发送 SettingsAppliedMessage()。
void OpenSettings(HWND owner, HINSTANCE inst);

// 设置窗口句柄；未打开时为 nullptr。消息循环用它做键盘导航。
HWND SettingsWindow();

// 设置已保存的通知，主窗口收到后重新加载配置并重注册快捷键
UINT SettingsAppliedMessage();

} // namespace ui
