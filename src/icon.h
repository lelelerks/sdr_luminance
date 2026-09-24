// icon.h — 运行时生成图标
//
// 不使用 .ico 资源文件：图标在运行时按所需尺寸绘制，既省去二进制资源，
// 也能直接按 DPI 生成匹配尺寸，避免缩放模糊。生成开销仅在启动与 DPI
// 变化时各一次，可忽略。
#pragma once

#include <windows.h>

namespace icon {

// 生成一枚 size×size 的图标。enabled=false 时使用灰色版本，
// 用于 HDR 未开启的状态指示（FR-3）。返回的 HICON 由调用方 DestroyIcon。
HICON Create(int size, bool enabled);

} // namespace icon
