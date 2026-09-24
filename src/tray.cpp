#include "tray.h"

#include <shellapi.h>

namespace tray {
namespace {

constexpr UINT kIconId = 1;

NOTIFYICONDATAW MakeBase(HWND hwnd) {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kIconId;
    return nid;
}

void FillIconAndTip(NOTIFYICONDATAW& nid, HICON iconHandle, const wchar_t* tip) {
    if (iconHandle) {
        nid.uFlags |= NIF_ICON;
        nid.hIcon = iconHandle;
    }
    if (tip) {
        nid.uFlags |= NIF_TIP | NIF_SHOWTIP;
        wcscpy_s(nid.szTip, tip);
    }
}

} // namespace

bool Add(HWND hwnd, UINT callbackMsg, HICON iconHandle, const wchar_t* tip) {
    NOTIFYICONDATAW nid = MakeBase(hwnd);
    nid.uFlags = NIF_MESSAGE;
    nid.uCallbackMessage = callbackMsg;
    FillIconAndTip(nid, iconHandle, tip);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) return false;

    // 版本 4 的回调语义更清晰：鼠标坐标在 wParam，事件在 LOWORD(lParam)
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
    return true;
}

void Update(HWND hwnd, HICON iconHandle, const wchar_t* tip) {
    NOTIFYICONDATAW nid = MakeBase(hwnd);
    FillIconAndTip(nid, iconHandle, tip);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void Remove(HWND hwnd) {
    NOTIFYICONDATAW nid = MakeBase(hwnd);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

UINT TaskbarCreatedMessage() {
    static const UINT msg = RegisterWindowMessageW(L"TaskbarCreated");
    return msg;
}

} // namespace tray
