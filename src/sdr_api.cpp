#include "sdr_api.h"

#include <vector>

namespace sdr {
namespace {

// ---------------------------------------------------------------------------
// 非公开定义：设置 SDR 白电平
//
// Microsoft 未公开文档化此 info type。已在 Win11 24H2 (10.0.26100) 实测可用，
// header.size 取默认对齐下的 sizeof（28 字节）即可，无需 #pragma pack(1)。
// 详见 tools/probe.cpp 与需求文档 3.1。
// ---------------------------------------------------------------------------
constexpr DISPLAYCONFIG_DEVICE_INFO_TYPE kSetSdrWhiteLevel =
    static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(0xFFFFFFEE);

struct SetSdrWhiteLevelPacket {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header; // 20 字节
    UINT32 SDRWhiteLevel;                    // 偏移 20
    BYTE finalValue;                         // 偏移 24；0 = 仅生效，1 = 持久化
};

// ---------------------------------------------------------------------------
// 滑杆刻度 <-> SDRWhiteLevel
//   nits          = 80 + slider * 4          (0..100 → 80..480 nits)
//   SDRWhiteLevel = nits * 1000 / 80 = 1000 + slider * 50
// ---------------------------------------------------------------------------
constexpr UINT32 kWhiteLevelBase = 1000;
constexpr UINT32 kWhiteLevelStep = 50;

UINT32 SliderToWhiteLevel(int slider) {
    return kWhiteLevelBase + static_cast<UINT32>(slider) * kWhiteLevelStep;
}

int WhiteLevelToSlider(UINT32 level) {
    if (level <= kWhiteLevelBase) return kSliderMin;
    const int v = static_cast<int>((level - kWhiteLevelBase + kWhiteLevelStep / 2) /
                                   kWhiteLevelStep);
    return v > kSliderMax ? kSliderMax : v;
}

int Clamp(int v) {
    if (v < kSliderMin) return kSliderMin;
    if (v > kSliderMax) return kSliderMax;
    return v;
}

// ---------------------------------------------------------------------------
// 主显示器目标缓存
//
// QueryDisplayConfig 是一次完整的显示路径枚举（约 0.1-1ms），不应在每次调节时
// 重复执行。结果缓存于此，仅在 Invalidate()（即 WM_DISPLAYCHANGE）时失效。
// ---------------------------------------------------------------------------
struct TargetRef {
    LUID adapterId{};
    UINT32 id = 0;
};

TargetRef g_target;
bool g_targetValid = false;
LONG g_lastError = ERROR_SUCCESS;

bool ResolvePrimaryTarget(TargetRef& out) {
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) {
        g_lastError = static_cast<LONG>(GetLastError());
        return false;
    }

    UINT32 pathCount = 0, modeCount = 0;
    g_lastError = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
    if (g_lastError != ERROR_SUCCESS) return false;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    g_lastError = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(),
                                     &modeCount, modes.data(), nullptr);
    if (g_lastError != ERROR_SUCCESS) return false;

    for (UINT32 i = 0; i < pathCount; ++i) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
        src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        src.header.size = sizeof(src);
        src.header.adapterId = paths[i].sourceInfo.adapterId;
        src.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
        if (wcscmp(src.viewGdiDeviceName, mi.szDevice) != 0) continue;

        // adapterId / id 必须取自 targetInfo，取 sourceInfo 会导致后续调用失败
        out.adapterId = paths[i].targetInfo.adapterId;
        out.id = paths[i].targetInfo.id;
        return true;
    }

    g_lastError = ERROR_NOT_FOUND;
    return false;
}

// 返回缓存的主显示器目标，必要时重新解析
const TargetRef* Target() {
    if (!g_targetValid) {
        g_targetValid = ResolvePrimaryTarget(g_target);
    }
    return g_targetValid ? &g_target : nullptr;
}

} // namespace

// ---------------------------------------------------------------------------

const wchar_t* StatusText(Status s) {
    switch (s) {
        case Status::Ok:              return L"正常";
        case Status::NoPrimaryTarget: return L"未能识别主显示器";
        case Status::QueryFailed:     return L"显示配置查询失败";
        case Status::NotHdr:          return L"当前显示器未开启 HDR，此功能不可用";
        case Status::ReadFailed:      return L"读取系统 SDR 亮度失败";
        case Status::WriteFailed:     return L"写入系统 SDR 亮度失败（系统接口可能已变更）";
    }
    return L"未知错误";
}

void Invalidate() {
    g_targetValid = false;
}

bool IsHdrActive() {
    const TargetRef* t = Target();
    if (!t) return false;

    // 首选 GET_ADVANCED_COLOR_INFO_2（枚举值 15，注意不是 14）
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 ci2{};
    ci2.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
    ci2.header.size = sizeof(ci2);
    ci2.header.adapterId = t->adapterId;
    ci2.header.id = t->id;
    if (DisplayConfigGetDeviceInfo(&ci2.header) == ERROR_SUCCESS) {
        return ci2.activeColorMode == DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR;
    }

    // 回退到 GET_ADVANCED_COLOR_INFO（枚举值 9），兼容 Win11 GA 之前的系统
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO ci{};
    ci.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    ci.header.size = sizeof(ci);
    ci.header.adapterId = t->adapterId;
    ci.header.id = t->id;
    if (DisplayConfigGetDeviceInfo(&ci.header) == ERROR_SUCCESS) {
        return ci.advancedColorEnabled != 0;
    }

    return false;
}

Status GetSlider(int& outSlider) {
    const TargetRef* t = Target();
    if (!t) return Status::NoPrimaryTarget;

    DISPLAYCONFIG_SDR_WHITE_LEVEL wl{};
    wl.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    wl.header.size = sizeof(wl);
    wl.header.adapterId = t->adapterId;
    wl.header.id = t->id;
    g_lastError = DisplayConfigGetDeviceInfo(&wl.header);
    if (g_lastError != ERROR_SUCCESS) return Status::ReadFailed;

    outSlider = WhiteLevelToSlider(static_cast<UINT32>(wl.SDRWhiteLevel));
    return Status::Ok;
}

Status SetSlider(int slider, bool persist) {
    const TargetRef* t = Target();
    if (!t) return Status::NoPrimaryTarget;
    if (!IsHdrActive()) return Status::NotHdr;

    SetSdrWhiteLevelPacket pkt{};
    pkt.header.type = kSetSdrWhiteLevel;
    pkt.header.size = sizeof(pkt);
    pkt.header.adapterId = t->adapterId;
    pkt.header.id = t->id;
    pkt.SDRWhiteLevel = SliderToWhiteLevel(Clamp(slider));
    pkt.finalValue = persist ? 1 : 0;

    g_lastError = DisplayConfigSetDeviceInfo(&pkt.header);
    return g_lastError == ERROR_SUCCESS ? Status::Ok : Status::WriteFailed;
}

LONG LastSystemError() {
    return g_lastError;
}

} // namespace sdr
