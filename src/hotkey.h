// hotkey.h — 全局快捷键的注册管理与组合键序列化
#pragma once

#include <windows.h>

namespace hotkey {

enum class Action {
    Up = 0,  // 调亮一档
    Down,    // 调暗一档
    Show,    // 唤起窗口
    Count
};

constexpr int kActionCount = static_cast<int>(Action::Count);

struct Combo {
    UINT mods = 0; // MOD_CONTROL / MOD_ALT / MOD_SHIFT / MOD_WIN 的组合
    UINT vk = 0;   // 0 表示未设置

    bool Valid() const { return vk != 0; }
    bool operator==(const Combo& o) const { return mods == o.mods && vk == o.vk; }
};

// 组合键 <-> 显示字符串，如 "Ctrl+Alt+Up"。空组合返回「未设置」。
void ToString(const Combo& c, wchar_t* buf, size_t cch);
Combo FromString(const wchar_t* s);

struct RegisterOutcome {
    bool ok = true;                // 全部成功（未设置的项视为成功）
    bool failed[kActionCount]{};   // 逐项是否失败
    DWORD error[kActionCount]{};   // 对应的 GetLastError
};

// 尽力注册：某一项与其他程序冲突不影响其余项。
// 不做整体回滚 —— 一个键冲突就让全部快捷键失效，对用户毫无价值。
RegisterOutcome RegisterAll(HWND hwnd, const Combo* combos);
void UnregisterAll(HWND hwnd);

// 动作的中文名，用于提示文案
const wchar_t* ActionName(Action a);

// 由 WM_HOTKEY 的 id 还原出对应动作
bool ActionFromId(int id, Action& out);

Combo Default(Action a);

// 与配置文件的读写。Load 对缺失项填入默认值。
void Load(Combo* out);
void Save(const Combo* combos);

// 调节步进值（1-50）
int LoadStep();
void SaveStep(int step);
constexpr int kStepMin = 1;
constexpr int kStepMax = 50;
constexpr int kStepDefault = 5;

// 快捷键调节时自动弹出的窗口停留多久（毫秒），自最后一次按键起算。
// 单位取毫秒而非秒：设置页复用 ES_NUMBER 输入框，它不接受小数点。
int LoadPopupMs();
void SavePopupMs(int ms);
constexpr int kPopupMsMin = 500;
constexpr int kPopupMsMax = 10000;
constexpr int kPopupMsDefault = 1500;

} // namespace hotkey
