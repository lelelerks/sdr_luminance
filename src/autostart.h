// autostart.h — 开机自启动开关
//
// 采用 HKCU\...\CurrentVersion\Run 注册表键，无需管理员权限。
// 注册表是该状态的唯一数据源，不写入 ini —— 避免两处状态不一致。
#pragma once

#include <windows.h>

namespace autostart {

bool IsEnabled();

// 启用时写入「"<exe 绝对路径>" --tray」，禁用时删除该值。
// 返回是否操作成功。
bool SetEnabled(bool enable);

// 已启用但记录的路径与当前 exe 不符时（程序被移动过）静默修正。
// 在启动阶段调用一次即可，省去用户手动重新开关。
void RefreshPath();

} // namespace autostart
