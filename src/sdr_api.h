// sdr_api.h — 主显示器定位、HDR 状态检测、SDR 白电平读写
//
// 本模块是全项目唯一直接接触 DisplayConfig* 系统 API 的地方。
// 写入路径依赖一个 Microsoft 未公开文档化的 info type，故所有接口
// 均返回明确的错误码而非 bool，以便上层给出可诊断的提示（见需求文档 8.2）。
#pragma once

#include <windows.h>

namespace sdr {

// 滑杆刻度范围，与 Windows 设置页中的「SDR 内容亮度」一一对应
inline constexpr int kSliderMin = 0;
inline constexpr int kSliderMax = 100;

enum class Status {
    Ok = 0,
    NoPrimaryTarget, // 未能在活动显示路径中定位主显示器
    QueryFailed,     // 显示配置查询失败
    NotHdr,          // 主显示器当前未处于 HDR 模式
    ReadFailed,      // 读取 SDR 白电平失败
    WriteFailed,     // 非公开写入 API 调用失败
};

// 返回可直接展示给用户的中文描述
const wchar_t* StatusText(Status s);

// 使缓存的主显示器信息失效。须在收到 WM_DISPLAYCHANGE 时调用。
void Invalidate();

// 主显示器当前是否处于 HDR 模式。查询失败一律视为 false。
bool IsHdrActive();

// 读取当前滑杆值（0-100）。不要求 HDR 已开启。
Status GetSlider(int& outSlider);

// 写入滑杆值（自动 clamp 到 0-100）。
//   persist = false —— 亮度立即生效但不写入系统持久化配置，用于滑杆拖动过程
//   persist = true  —— 亮度生效并持久化，用于拖动结束、快捷键单次调节
// 非 HDR 模式下返回 Status::NotHdr 且不做任何写入。
Status SetSlider(int slider, bool persist);

// 供诊断用：返回上一次系统 API 调用的原始返回码
LONG LastSystemError();

} // namespace sdr
