#include "icon.h"

#include <vector>

namespace icon {
namespace {

// 图形：一个圆，左半为强调色、右半为亮色 —— 亮度/对比度的通用符号。
// 蓝白配色在浅色与深色任务栏上都能辨识。
struct Rgb {
    int r, g, b;
};

constexpr Rgb kLeftOn{0, 120, 215};    // Windows 强调蓝
constexpr Rgb kRightOn{250, 250, 250}; // 近白
constexpr Rgb kLeftOff{110, 110, 110};
constexpr Rgb kRightOff{175, 175, 175};

// GDI 的 Ellipse 无抗锯齿，小尺寸下锯齿明显。
// 这里直接按像素做 4×4 超采样，用覆盖率同时得到边缘颜色与 alpha。
constexpr int kSuper = 4;

} // namespace

HICON Create(int size, bool enabled) {
    if (size < 4) size = 4;

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size; // 负数 = 自上而下
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS,
                                     &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color) return nullptr;

    const Rgb left = enabled ? kLeftOn : kLeftOff;
    const Rgb right = enabled ? kRightOn : kRightOff;
    const double center = size / 2.0;
    const double radius = size * 0.46;
    const double radiusSq = radius * radius;
    const int samples = kSuper * kSuper;

    auto* px = static_cast<DWORD*>(bits);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int inLeft = 0, inRight = 0;
            for (int sy = 0; sy < kSuper; ++sy) {
                for (int sx = 0; sx < kSuper; ++sx) {
                    const double fx = x + (sx + 0.5) / kSuper;
                    const double fy = y + (sy + 0.5) / kSuper;
                    const double dx = fx - center;
                    const double dy = fy - center;
                    if (dx * dx + dy * dy > radiusSq) continue;
                    if (fx < center) {
                        ++inLeft;
                    } else {
                        ++inRight;
                    }
                }
            }

            const int covered = inLeft + inRight;
            if (covered == 0) {
                px[y * size + x] = 0; // 完全透明
                continue;
            }

            // 边缘像素同时按左右两色的覆盖比例混色，alpha 取总覆盖率
            const int r = (left.r * inLeft + right.r * inRight) / covered;
            const int g = (left.g * inLeft + right.g * inRight) / covered;
            const int b = (left.b * inLeft + right.b * inRight) / covered;
            const int a = 255 * covered / samples;
            px[y * size + x] = (static_cast<DWORD>(a) << 24) | (static_cast<DWORD>(r) << 16) |
                               (static_cast<DWORD>(g) << 8) | static_cast<DWORD>(b);
        }
    }

    // 32 位图标由 alpha 通道决定透明度，掩码位图内容无关紧要，给个全 0 即可
    std::vector<BYTE> maskBits(static_cast<size_t>(((size + 31) / 32) * 4) * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON result = CreateIconIndirect(&ii);

    DeleteObject(color);
    DeleteObject(mask);
    return result;
}

} // namespace icon
