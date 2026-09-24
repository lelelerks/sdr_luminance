# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目性质

Windows 11 HDR 模式下调节「SDR 内容亮度」的托盘小工具。C++17 + 纯 Win32 API，无 UI 框架、无第三方依赖、静态链接 CRT。

设计目标（doc/需求文档.md FR-4）是硬约束，不是愿望：单文件 exe < 300 KB、常驻内存 2–4 MB、**空闲 CPU 0%**。任何引入后台线程、轮询、全局钩子或 UI 框架的改动都会破坏这些指标，做之前先确认是否真的必要。

## 构建

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64    # 只需首次执行
cmake --build build --config Release                     # 日常构建（增量）
cmake --build build --config Debug                       # 调试构建
```

- **仅支持 MSVC**，`CMakeLists.txt` 对非 MSVC 直接 `FATAL_ERROR`。需要 Windows SDK ≥ 10.0.22621（`DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2` 所需）。
- **构建前必须先退出正在运行的 `SdrLuminance.exe`**（托盘右键 → 退出），否则链接器报 `LNK1168`。
- 新增/删除源文件时要同步改 `CMakeLists.txt` 的 `add_executable`；只改 `CMakeLists.txt` 本身不必重新 configure，`ZERO_CHECK` 会自动重生成。
- Visual Studio 是多配置生成器，`CMAKE_BUILD_TYPE` 无效，实际配置由 `--config` 决定（VS Code 中由状态栏变体选择器决定）。
- 产物在 `build\Release\`：`SdrLuminance.exe` 与 `probe.exe`。

## 验证

项目**没有单元测试框架**，验证靠 `tools/probe.cpp` 编出的控制台探针 + 手动验证：

```
build\Release\probe.exe            # 只读查询：主显示器、HDR 状态、当前白电平
build\Release\probe.exe set 50     # 写入并回读校验
build\Release\probe.exe demo       # 30 → 70 → 恢复原值
```

修改 `src/sdr_api.cpp` 后先用 probe 验证系统调用层是否正常，再验主程序。系统接口疑似变更时，probe 也是区分「系统改了」与「我们改坏了」的第一手段。

## 架构

单线程、纯消息驱动。分层如下：

```
main.cpp          单实例互斥体、DPI 感知、消息循环
  └ ui_main.cpp   主窗口过程 —— 全部状态与事件的汇聚点
      ├ ui_settings.cpp   设置窗口（独立窗口类，非对话框资源）
      ├ tray.cpp / icon.cpp
      ├ hotkey.cpp        RegisterHotKey 管理 + 组合键↔字符串
      ├ config.cpp        ini 读写
      ├ autostart.cpp     注册表 Run 键
      └ sdr_api.cpp       唯一直接调用 DisplayConfig* 的模块
```

各模块以 `namespace` 划分（`sdr` / `ui` / `hotkey` / `tray` / `config` / `icon` / `autostart`），头文件里写清了「为什么这么做」，改之前先读。

### 关键不变量

以下几条一旦破坏会引发难查的问题：

1. **`sdr_api` 是唯一接触显示配置 API 的地方。** 其他模块不得直接调用 `DisplayConfigGetDeviceInfo` / `QueryDisplayConfig`。

2. **写入分级（`persist` 参数）。** `SetSlider(value, persist)` 的 `persist=false` 只让亮度立即生效不落盘，`true` 才写入系统持久化配置。拖动过程与快捷键长按连发期间一律传 `false`，停手后（拖动结束 / 400ms 定时器）才补一次 `true`。这是避免每秒几十次系统配置写入的核心机制，新增调节入口时必须遵循同样的模式。

3. **自动弹窗的所有权标志 `autoShown`。** 快捷键调节时若窗口是隐藏的，会自动弹出显示数值并起 `kTimerAutoHide` 倒计时；`autoShown` 标记「这个可见窗口是快捷键弹出的、尚未被用户接管」，**只有它为真时窗口才会自动收回**。用户一旦主动介入就必须调 `CancelAutoHide()` 转为常驻，否则会出现「正拖着滑杆窗口突然消失」。现有取消入口：`WndProc` 顶部的鼠标按下前置拦截（`WM_*BUTTONDOWN` / `WM_NC*BUTTONDOWN` / `WM_PARENTNOTIFY`，覆盖客户区、标题栏与子控件上的点击）、`ToggleWindow` / `OpenSettings` / `OnHScroll` / `ShowTrayMenu` 开头、`ActivateMessage` 的已可见分支、`WM_CLOSE`、`WM_DESTROY`。**新增任何用户交互入口时都要考虑是否该加进这个列表。**

**不要改用 `WM_MOUSEACTIVATE` 来接管点击** —— 它只在窗口「非活动」时发送，而 `ShowTrayMenu` 的 `SetForegroundWindow` 与设置窗口销毁后的激活交还都会让本窗口变成活动窗口，此后点击收不到该消息。这个坑踩过一次，症状是「偶尔点空白处/标题栏无法转常驻」，且因依赖历史状态而极难复现。

注意「唤起窗口」快捷键**不在**此列：它是纯开关（直接 `ToggleWindow`），自动弹出的窗口按它是隐藏而非留住。

4. **主显示器信息有缓存，只在 `WM_DISPLAYCHANGE` 时失效。** 收到该消息必须调 `sdr::Invalidate()`。HDR 状态检测同样完全事件驱动，**任何轮询都是错的**。三个定时器（节流 / 落盘 / 自动收回）全部是一次性的，触发即 `KillTimer` —— 不要改成周期性定时器。

5. **主窗口是 `WS_EX_NOACTIVATE`**（配合 `WS_EX_TOPMOST` + `WS_EX_TOOLWINDOW`，并在 `WM_WINDOWPOSCHANGING` 中强制 `HWND_TOPMOST`）。它永远拿不到键盘焦点 —— 这是「不打断全屏游戏」的前提，不要为了做键盘交互而去掉。设置窗口是刻意的例外：快捷键录制需要键盘输入，所以它不带 `NOACTIVATE`，并由 `main.cpp` 的消息循环用 `IsDialogMessageW` 提供 Tab/回车导航。

6. **快捷键录制期间必须临时 `UnregisterAll`**，否则程序会拦截自己正在录制的按键。

7. **快捷键注册尽力而为，不整体回滚。** 某一项与其他程序冲突时其余项照常生效，逐项错误码记在 `RegisterOutcome` 里用于提示。

8. **自启动状态以注册表为唯一数据源**，不写 ini，避免两处不一致。设置页与托盘菜单共享同一状态。

### 消息约定

主窗口的自定义消息集中在 `ui_main.cpp` 顶部（`WM_APP + n`）。两个反直觉之处已在注释里说明，改动时注意：

- `WM_FIT_CONTENT` 必须在窗口创建**之后**发送再定尺寸，在 `WM_CREATE` 里做会被窗口创建流程覆盖。
- `WM_HOTKEY_WARNING` 必须 `Post` 而非在注册处直接弹窗 —— 在 `WM_CREATE` 中弹模态框会卡死整个窗口创建流程。

### 非公开 API

写入 SDR 白电平用的 info type `0xFFFFFFEE` 及其结构体 Microsoft 未公开文档化（`sdr_api.cpp` 顶部）。读取路径与 HDR 检测走公开 API。换算关系：

```
nits          = 80 + slider * 4       // slider 0..100 → 80..480 nits
SDRWhiteLevel = 1000 + slider * 50
```

HDR 检测优先用 `GET_ADVANCED_COLOR_INFO_2`（枚举值 **15**，不是 14），失败回退 `GET_ADVANCED_COLOR_INFO`。

`CMakeLists.txt` 里的 `NTDDI_VERSION=0x0A00000F` 不能删：少了它 `wingdi.h` 会把 `DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2` 用 `#if` 屏蔽掉。

### 已明确排除的方案

以下都是已做过决策的否决项，不要重新提议（理由见 doc/需求文档.md 第 4 节）：

- 注入 DLL / Hook D3D 交换链来覆盖独占全屏 —— 反作弊封号风险，明确不采用。
- 多显示器支持 —— YAGNI，仅作用于主显示器。
- 全局鼠标钩子（`WH_MOUSE_LL`）实现「组合键 + 滚轮」 —— 与 CPU 0% 冲突。
- 启动时主动写入上次的亮度值 —— 只读同步，不在用户无感知时改系统设置。
- `.ico` / `.rc` 资源文件 —— 图标运行时按 DPI 绘制（`icon.cpp`），manifest 由 `main.cpp` 的 `#pragma comment(linker, ...)` 提供。

## 文档

`doc/需求文档.md` 是设计说明与决策记录，含逐条功能需求（FR-1…FR-6）、性能手段、阶段划分。做设计层面的改动前读对应章节；改动落地后同步更新它与 `README.md`。

代码注释与文档一律中文，注释说明「为什么」而非「是什么」——沿用现有风格。
