#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace Monitors {

struct MonitorInfo {
    std::wstring name;
    RECT rect;
    bool primary;
};

bool GetPrimaryMonitorInfo(std::wstring& outDeviceName, RECT& outRect);
std::vector<MonitorInfo> EnumerateMonitors();

// Moves a window to the specified monitor.
// If `fullscreen` is true, positions/sizes the window to cover the monitor and keeps it topmost.
// If `fullscreen` is false, preserves the current window size and moves it with padding.
bool MoveWindowToMonitor(HWND hwnd, const MonitorInfo& mon, bool fullscreen);

} // namespace Monitors
