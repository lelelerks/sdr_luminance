// probe.cpp — 非公开 SDR 白电平 API 验证程序
//
// 目的：验证 DisplayConfigSetDeviceInfo 的非公开 info type 0xFFFFFFEE
//       能否真正改变 Win11「设置 → 系统 → 屏幕 → HDR → SDR 内容亮度」。
//
// 用法：
//   probe.exe            仅查询，只读不修改（安全）
//   probe.exe set <0-100>  设置滑杆值并回读校验
//   probe.exe demo       30 → 70 → 恢复原值，每步停留 3 秒供肉眼观察

// 以下三个宏在 CMake 构建时已由 CMakeLists.txt 统一定义；
// 此处保留 #ifndef 兜底，便于脱离 CMake 直接用 cl 单独编译本文件。
// NTDDI_VERSION 必须为 NTDDI_WIN11_GA，否则 GET_ADVANCED_COLOR_INFO_2 不可见。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A00000F
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>

// ---------------------------------------------------------------------------
// 非公开定义：设置 SDR 白电平
// 来源：ledoge/set_maxtml。Microsoft 未公开文档化此 info type。
// ---------------------------------------------------------------------------
static const DISPLAYCONFIG_DEVICE_INFO_TYPE kSetSdrWhiteLevel =
    static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(0xFFFFFFEE);

struct SetSdrWhiteLevel {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header; // 20 字节
    UINT32 SDRWhiteLevel;                    // 偏移 20
    BYTE finalValue;                         // 偏移 24；0 = 仅生效，1 = 持久化
};                                           // 默认对齐后 sizeof = 28

// ---------------------------------------------------------------------------
// 滑杆刻度 <-> SDRWhiteLevel 换算
//   nits          = 80 + slider * 4          (slider 0..100 → 80..480 nits)
//   SDRWhiteLevel = nits * 1000 / 80 = 1000 + slider * 50
// ---------------------------------------------------------------------------
static UINT32 SliderToWhiteLevel(int slider) {
    return 1000u + static_cast<UINT32>(slider) * 50u;
}

static int WhiteLevelToSlider(UINT32 level) {
    if (level < 1000u) return 0;
    return static_cast<int>((level - 1000u + 25u) / 50u); // 四舍五入
}

static double WhiteLevelToNits(UINT32 level) {
    return level * 80.0 / 1000.0;
}

// ---------------------------------------------------------------------------
// 定位主显示器的 adapterId / targetId
// ---------------------------------------------------------------------------
struct TargetRef {
    LUID adapterId{};
    UINT32 id = 0;
    WCHAR gdiName[CCHDEVICENAME]{};
};

static bool FindPrimaryTarget(TargetRef& out) {
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) {
        printf("  [错误] GetMonitorInfoW 失败，GetLastError = %lu\n", GetLastError());
        return false;
    }
    printf("  主显示器 GDI 名称：%ls\n", mi.szDevice);

    UINT32 pathCount = 0, modeCount = 0;
    LONG rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (rc != ERROR_SUCCESS) {
        printf("  [错误] GetDisplayConfigBufferSizes 失败，返回 %ld\n", rc);
        return false;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                            modes.data(), nullptr);
    if (rc != ERROR_SUCCESS) {
        printf("  [错误] QueryDisplayConfig 失败，返回 %ld\n", rc);
        return false;
    }
    printf("  活动显示路径数：%u\n", pathCount);

    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;

        printf("    path[%u] source = %ls\n", i, src.viewGdiDeviceName);
        if (wcscmp(src.viewGdiDeviceName, mi.szDevice) == 0) {
            // 关键：adapterId / id 必须取自 targetInfo，而非 sourceInfo
            out.adapterId = paths[i].targetInfo.adapterId;
            out.id = paths[i].targetInfo.id;
            wcscpy_s(out.gdiName, mi.szDevice);
            return true;
        }
    }
    printf("  [错误] 未在活动路径中匹配到主显示器\n");
    return false;
}

// ---------------------------------------------------------------------------
// HDR 状态查询
// ---------------------------------------------------------------------------
static void QueryHdrState(const TargetRef& t) {
    // 首选：GET_ADVANCED_COLOR_INFO_2 (=15)，Win11 22H2+
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 ci2{};
    ci2.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
    ci2.header.size = sizeof(ci2);
    ci2.header.adapterId = t.adapterId;
    ci2.header.id = t.id;
    LONG rc = DisplayConfigGetDeviceInfo(&ci2.header);
    if (rc == ERROR_SUCCESS) {
        const char* mode = "未知";
        switch (ci2.activeColorMode) {
            case DISPLAYCONFIG_ADVANCED_COLOR_MODE_SDR: mode = "SDR"; break;
            case DISPLAYCONFIG_ADVANCED_COLOR_MODE_WCG: mode = "WCG（广色域）"; break;
            case DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR: mode = "HDR"; break;
        }
        printf("  [type 15] 可用\n");
        printf("    activeColorMode          = %s\n", mode);
        printf("    highDynamicRangeSupported= %u\n", ci2.highDynamicRangeSupported);
        printf("    highDynamicRangeUserEnabled = %u\n", ci2.highDynamicRangeUserEnabled);
        printf("    advancedColorActive      = %u\n", ci2.advancedColorActive);
    } else {
        printf("  [type 15] 不可用，返回 %ld\n", rc);
    }

    // 回退：GET_ADVANCED_COLOR_INFO (=9)
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO ci{};
    ci.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    ci.header.size = sizeof(ci);
    ci.header.adapterId = t.adapterId;
    ci.header.id = t.id;
    rc = DisplayConfigGetDeviceInfo(&ci.header);
    if (rc == ERROR_SUCCESS) {
        printf("  [type 9 ] 可用：advancedColorSupported=%u, advancedColorEnabled=%u\n",
               ci.advancedColorSupported, ci.advancedColorEnabled);
    } else {
        printf("  [type 9 ] 不可用，返回 %ld\n", rc);
    }
}

static bool IsHdrActive(const TargetRef& t) {
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 ci2{};
    ci2.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
    ci2.header.size = sizeof(ci2);
    ci2.header.adapterId = t.adapterId;
    ci2.header.id = t.id;
    if (DisplayConfigGetDeviceInfo(&ci2.header) == ERROR_SUCCESS)
        return ci2.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR;

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO ci{};
    ci.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    ci.header.size = sizeof(ci);
    ci.header.adapterId = t.adapterId;
    ci.header.id = t.id;
    if (DisplayConfigGetDeviceInfo(&ci.header) == ERROR_SUCCESS)
        return ci.advancedColorEnabled != 0;

    return false;
}

// ---------------------------------------------------------------------------
// 读取当前 SDR 白电平（公开 API，type 11）
// ---------------------------------------------------------------------------
static bool GetSdrWhiteLevel(const TargetRef& t, UINT32& outLevel) {
    DISPLAYCONFIG_SDR_WHITE_LEVEL wl{};
    wl.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    wl.header.size = sizeof(wl);
    wl.header.adapterId = t.adapterId;
    wl.header.id = t.id;
    LONG rc = DisplayConfigGetDeviceInfo(&wl.header);
    if (rc != ERROR_SUCCESS) {
        printf("  [错误] GET_SDR_WHITE_LEVEL 失败，返回 %ld\n", rc);
        return false;
    }
    outLevel = static_cast<UINT32>(wl.SDRWhiteLevel);
    return true;
}

// ---------------------------------------------------------------------------
// 写入 SDR 白电平（非公开 API）
// headerSize 用于试探结构体大小：28（默认对齐）或 25（紧凑）
// ---------------------------------------------------------------------------
static LONG SetSdrWhiteLevelRaw(const TargetRef& t, UINT32 level, bool finalValue,
                                UINT32 headerSize) {
    SetSdrWhiteLevel s{};
    s.header.type = kSetSdrWhiteLevel;
    s.header.size = headerSize;
    s.header.adapterId = t.adapterId;
    s.header.id = t.id;
    s.SDRWhiteLevel = level;
    s.finalValue = finalValue ? 1 : 0;
    return DisplayConfigSetDeviceInfo(&s.header);
}

// 试探两种 size，返回成功时使用的 size；失败返回 0
static UINT32 g_workingSize = 0;

static bool SetSlider(const TargetRef& t, int slider, bool finalValue) {
    const UINT32 level = SliderToWhiteLevel(slider);
    const UINT32 candidates[] = {sizeof(SetSdrWhiteLevel), 25};

    if (g_workingSize != 0) {
        LONG rc = SetSdrWhiteLevelRaw(t, level, finalValue, g_workingSize);
        if (rc == ERROR_SUCCESS) return true;
        printf("  [警告] 已知可用 size=%u 本次失败，返回 %ld，重新试探\n", g_workingSize, rc);
        g_workingSize = 0;
    }

    for (UINT32 sz : candidates) {
        LONG rc = SetSdrWhiteLevelRaw(t, level, finalValue, sz);
        printf("  SetDeviceInfo(type=0x%08X, size=%u, level=%u, final=%d) → %ld %s\n",
               static_cast<unsigned>(kSetSdrWhiteLevel), sz, level, finalValue ? 1 : 0, rc,
               rc == ERROR_SUCCESS ? "成功" : "失败");
        if (rc == ERROR_SUCCESS) {
            g_workingSize = sz;
            return true;
        }
        if (rc == ERROR_INVALID_PARAMETER) printf("    （ERROR_INVALID_PARAMETER）\n");
        if (rc == ERROR_ACCESS_DENIED) printf("    （ERROR_ACCESS_DENIED）\n");
        if (rc == ERROR_NOT_SUPPORTED) printf("    （ERROR_NOT_SUPPORTED）\n");
    }
    return false;
}

// 设置 + 回读校验
static bool SetAndVerify(const TargetRef& t, int slider) {
    printf("\n>>> 设置滑杆 = %d（SDRWhiteLevel = %u，%.0f nits）\n", slider,
           SliderToWhiteLevel(slider), WhiteLevelToNits(SliderToWhiteLevel(slider)));
    if (!SetSlider(t, slider, true)) {
        printf("  ❌ 写入失败\n");
        return false;
    }
    UINT32 back = 0;
    if (!GetSdrWhiteLevel(t, back)) return false;
    const int backSlider = WhiteLevelToSlider(back);
    printf("  回读：SDRWhiteLevel = %u → 滑杆 %d（%.0f nits） %s\n", back, backSlider,
           WhiteLevelToNits(back), backSlider == slider ? "✅ 一致" : "❌ 不一致");
    return backSlider == slider;
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setlocale(LC_ALL, ".UTF8");

    printf("=== SDR 白电平 API 探针 ===\n");
    printf("sizeof(DISPLAYCONFIG_DEVICE_INFO_HEADER) = %zu\n",
           sizeof(DISPLAYCONFIG_DEVICE_INFO_HEADER));
    printf("sizeof(SetSdrWhiteLevel)                 = %zu\n", sizeof(SetSdrWhiteLevel));

    printf("\n[1] 定位主显示器\n");
    TargetRef t;
    if (!FindPrimaryTarget(t)) return 1;
    printf("  adapterId = %08lX:%08lX, targetId = %u\n", t.adapterId.HighPart,
           t.adapterId.LowPart, t.id);

    printf("\n[2] HDR 状态\n");
    QueryHdrState(t);
    const bool hdr = IsHdrActive(t);
    printf("  ==> 判定：HDR %s\n", hdr ? "已开启" : "未开启");

    printf("\n[3] 当前 SDR 白电平\n");
    UINT32 original = 0;
    if (!GetSdrWhiteLevel(t, original)) return 1;
    printf("  SDRWhiteLevel = %u → 滑杆 %d（%.0f nits）\n", original,
           WhiteLevelToSlider(original), WhiteLevelToNits(original));

    const char* mode = (argc > 1) ? argv[1] : "";

    if (mode[0] == '\0') {
        printf("\n仅查询模式，未修改任何设置。\n");
        printf("如需验证写入，请运行：probe.exe demo\n");
        return 0;
    }

    if (!hdr) {
        printf("\n❌ 当前未开启 HDR，写入验证无意义。请先在系统设置中开启 HDR。\n");
        return 2;
    }

    printf("\n[4] 写入验证\n");

    if (strcmp(mode, "set") == 0 && argc > 2) {
        int v = atoi(argv[2]);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        return SetAndVerify(t, v) ? 0 : 3;
    }

    if (strcmp(mode, "demo") == 0) {
        printf("请打开「设置 → 系统 → 屏幕 → HDR」页面观察 SDR 内容亮度滑杆。\n");
        bool ok = true;
        ok &= SetAndVerify(t, 30);
        printf("  （停留 3 秒）\n");
        Sleep(3000);
        ok &= SetAndVerify(t, 70);
        printf("  （停留 3 秒）\n");
        Sleep(3000);

        printf("\n>>> 恢复原值 SDRWhiteLevel = %u（滑杆 %d）\n", original,
               WhiteLevelToSlider(original));
        SetSlider(t, WhiteLevelToSlider(original), true);
        UINT32 back = 0;
        GetSdrWhiteLevel(t, back);
        printf("  回读：SDRWhiteLevel = %u\n", back);

        printf("\n=== 结论：非公开写入 API %s ===\n", ok ? "✅ 可用" : "❌ 不可用");
        return ok ? 0 : 3;
    }

    printf("未知参数：%s\n", mode);
    return 1;
}
