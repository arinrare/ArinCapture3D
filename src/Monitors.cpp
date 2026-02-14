#include "Monitors.h"
#include "Log.h"

namespace Monitors {

static bool TryGetMonitorRectFromDeviceName(const std::wstring& deviceName, RECT& outRect) {
    if (deviceName.empty()) return false;

    struct FindCtx {
        const std::wstring* name = nullptr;
        RECT rect{0, 0, 0, 0};
        bool found = false;
    } ctx;

    ctx.name = &deviceName;
    EnumDisplayMonitors(nullptr, nullptr,
        (MONITORENUMPROC)+[](HMONITOR hMon, HDC, LPRECT, LPARAM lparam) -> BOOL {
            auto* c = reinterpret_cast<FindCtx*>(lparam);
            MONITORINFOEXW mi = { sizeof(mi) };
            if (GetMonitorInfoW(hMon, &mi)) {
                if (*c->name == mi.szDevice) {
                    c->rect = mi.rcMonitor;
                    c->found = true;
                    return FALSE;
                }
            }
            return TRUE;
        },
        (LPARAM)&ctx);

    if (!ctx.found) return false;
    outRect = ctx.rect;
    return true;
}

bool GetPrimaryMonitorInfo(std::wstring& outDeviceName, RECT& outRect) {
    struct PrimaryMon {
        bool found = false;
        std::wstring name;
        RECT rect = {0, 0, 0, 0};
    } pm;

    EnumDisplayMonitors(nullptr, nullptr,
        (MONITORENUMPROC)+[](HMONITOR hMon, HDC, LPRECT lprc, LPARAM lparam) -> BOOL {
            PrimaryMon* p = reinterpret_cast<PrimaryMon*>(lparam);
            MONITORINFOEXW mi = {sizeof(mi)};
            if (GetMonitorInfoW(hMon, &mi) && (mi.dwFlags & MONITORINFOF_PRIMARY)) {
                p->found = true;
                p->name = mi.szDevice;
                p->rect = *lprc;
                return FALSE;
            }
            return TRUE;
        },
        (LPARAM)&pm);

    if (!pm.found) return false;
    outDeviceName = pm.name;
    outRect = pm.rect;
    return true;
}

std::vector<MonitorInfo> EnumerateMonitors() {
    // Virtual Desktop / other virtual display drivers can add/remove displays dynamically.
    // EnumDisplayDevices + EnumDisplaySettingsEx tends to be more reliable for listing all
    // displays attached to the desktop than EnumDisplayMonitors in those cases.
    std::vector<MonitorInfo> monitors;

    std::wstring primaryDisplayName;
    RECT primaryRect = {0, 0, 0, 0};
    GetPrimaryMonitorInfo(primaryDisplayName, primaryRect);

    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) {
            break;
        }

        if ((dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0) {
            continue;
        }

        DEVMODEW dm{};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsExW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm, 0)) {
            continue;
        }

        MonitorInfo info;
        info.name = dd.DeviceName; // e.g. \\\.\\DISPLAY2
        info.rect.left = dm.dmPosition.x;
        info.rect.top = dm.dmPosition.y;
        info.rect.right = dm.dmPosition.x + (LONG)dm.dmPelsWidth;
        info.rect.bottom = dm.dmPosition.y + (LONG)dm.dmPelsHeight;
        info.primary = (!primaryDisplayName.empty() && info.name == primaryDisplayName);

        // Some display setups (virtual display drivers, mixed DPI, etc.) can report misleading
        // geometry via EnumDisplaySettingsEx. Prefer the actual HMONITOR rect when available.
        RECT miRect{};
        if (TryGetMonitorRectFromDeviceName(info.name, miRect)) {
            info.rect = miRect;
        }

        // Skip obviously invalid entries.
        if (info.rect.right <= info.rect.left || info.rect.bottom <= info.rect.top) {
            continue;
        }

        monitors.push_back(info);
    }

    // Fallback if device enumeration yields nothing.
    if (monitors.empty()) {
        EnumDisplayMonitors(nullptr, nullptr,
            (MONITORENUMPROC)+[](HMONITOR hMon, HDC, LPRECT lprc, LPARAM lparam) -> BOOL {
                auto* vec = reinterpret_cast<std::vector<MonitorInfo>*>(lparam);
                MONITORINFOEXW mi = {sizeof(mi)};
                if (GetMonitorInfoW(hMon, &mi)) {
                    MonitorInfo info;
                    info.name = mi.szDevice;
                    info.rect = *lprc;
                    info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
                    vec->push_back(info);
                }
                return TRUE;
            },
            (LPARAM)&monitors);
    }

    return monitors;
}

bool MoveWindowToMonitor(HWND hwnd, const MonitorInfo& mon, bool fullscreen) {
    if (!hwnd) return false;

    RECT targetRect = mon.rect;
    TryGetMonitorRectFromDeviceName(mon.name, targetRect);

    RECT cur = {0, 0, 0, 0};
    GetWindowRect(hwnd, &cur);
    int w = cur.right - cur.left;
    int h = cur.bottom - cur.top;
    if (w <= 0) w = 1280;
    if (h <= 0) h = 720;

    std::string monName;
    int needed = WideCharToMultiByte(CP_UTF8, 0, mon.name.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (needed > 1) {
        monName.resize(static_cast<size_t>(needed - 1));
        WideCharToMultiByte(CP_UTF8, 0, mon.name.c_str(), -1, monName.data(), needed, nullptr, nullptr);
    }

    Log::Info("Moving render window to monitor " + monName + " at (" + std::to_string(targetRect.left) +
              "," + std::to_string(targetRect.top) + ") size (" + std::to_string(w) + "x" +
              std::to_string(h) + ")");

    auto getVirtualScreen = []() -> RECT {
        RECT r{};
        r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        r.right = r.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        r.bottom = r.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        return r;
    };

    auto ensureOnVirtualScreen = [&]() {
        RECT vr = getVirtualScreen();
        RECT wr{};
        if (!GetWindowRect(hwnd, &wr)) return;
        RECT inter{};
        if (!IntersectRect(&inter, &vr, &wr)) {
            Log::Error("Render window moved off the virtual screen; clamping back into view");
            const int padX = 50;
            const int padY = 50;
            SetWindowPos(hwnd, HWND_NOTOPMOST, vr.left + padX, vr.top + padY, 0, 0,
                         SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        }
    };

    if (fullscreen) {
        const int fw = targetRect.right - targetRect.left;
        const int fh = targetRect.bottom - targetRect.top;
        BOOL ok = SetWindowPos(hwnd, HWND_TOPMOST, targetRect.left, targetRect.top, fw, fh,
                               SWP_SHOWWINDOW | SWP_FRAMECHANGED | SWP_NOACTIVATE) != FALSE;
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        ensureOnVirtualScreen();
        return ok != FALSE;
    }

    // Windowed preview: keep size, just move to the selected monitor with some padding.
    const int padX = 50;
    const int padY = 50;
    BOOL ok = SetWindowPos(hwnd, HWND_NOTOPMOST, targetRect.left + padX, targetRect.top + padY, w, h,
                           SWP_SHOWWINDOW | SWP_NOACTIVATE) != FALSE;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    ensureOnVirtualScreen();
    return ok != FALSE;
}

} // namespace Monitors
