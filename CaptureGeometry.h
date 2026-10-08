#pragma once

#include <windows.h>
#include <algorithm>
#include <cwctype>
#include "Settings.h"

// Shared by the GPU crop and the settings resolution selector. Keep window matching,
// monitor clipping and fallback behavior identical in both places.
namespace CaptureGeometry
{
struct WindowSearch
{
    std::wstring title;
    HWND found = nullptr;
};

inline BOOL CALLBACK FindWindowCallback(HWND window, LPARAM parameter)
{
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    if (!IsWindowVisible(window)) return TRUE;
    wchar_t title[512]{};
    GetWindowTextW(window, title, ARRAYSIZE(title));
    std::wstring candidate = title, requested = search.title;
    std::transform(candidate.begin(), candidate.end(), candidate.begin(), towlower);
    std::transform(requested.begin(), requested.end(), requested.begin(), towlower);
    if (!requested.empty() && candidate.find(requested) != std::wstring::npos)
    {
        search.found = window;
        return FALSE;
    }
    return TRUE;
}

inline HWND FindWindow(const std::wstring& title)
{
    WindowSearch search{ title };
    EnumWindows(FindWindowCallback, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

inline RECT Rectangle(const AppSettings& settings, RECT desktop, HWND foreground)
{
    if (desktop.right <= desktop.left || desktop.bottom <= desktop.top) return {};
    RECT result = desktop;
    if (settings.captureMode == 1)
    {
        result.left = desktop.left + settings.captureRegionX;
        result.top = desktop.top + settings.captureRegionY;
        result.right = result.left + static_cast<LONG>(settings.captureRegionWidth);
        result.bottom = result.top + static_cast<LONG>(settings.captureRegionHeight);
    }
    else if (settings.captureMode == 2 || settings.captureMode == 3)
    {
        HWND window = settings.captureMode == 3 ? foreground : FindWindow(settings.captureWindowTitle);
        RECT windowRect{};
        if (window && GetWindowRect(window, &windowRect)) result = windowRect;
    }
    result.left = std::clamp(result.left, desktop.left, desktop.right - 1);
    result.top = std::clamp(result.top, desktop.top, desktop.bottom - 1);
    result.right = std::clamp(result.right, result.left + 1, desktop.right);
    result.bottom = std::clamp(result.bottom, result.top + 1, desktop.bottom);
    return result;
}
}
