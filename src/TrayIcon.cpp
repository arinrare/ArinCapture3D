#include "TrayIcon.h"

#include "WindowTargeting.h"

static constexpr UINT kCmdExit = 1;
static constexpr UINT kCmdStopCapture = 2;
static constexpr UINT kCmdStartCaptureMonitor = 4;
static constexpr UINT kCmdStartCaptureWindow = 5;
static constexpr UINT kCmdStartCaptureActiveWindow = 7;
static constexpr UINT kCmdCycleOutput = 3;
static constexpr UINT kCmdToggleFullscreen = 6;
static constexpr UINT kCmdSelectOutputBase = 1000;
static constexpr UINT kCmdFramerateBase = 2000;
static constexpr UINT kCmdDiagnosticsOverlay = 3000;
static constexpr UINT kCmdDiagnosticsOverlaySizeBase = 3100;
static constexpr UINT kCmdDiagnosticsOverlayModeBase = 3200;
static constexpr UINT kCmdRenderResBase = 4000;
static constexpr UINT kCmdToggleStereo = 5000;
static constexpr UINT kCmdStereoDepth = 5001;
static constexpr UINT kCmdToggleClickThrough = 5002;
static constexpr UINT kCmdToggleVSync = 5003;
static constexpr UINT kCmdToggleExcludeFromCapture = 5005;
static constexpr UINT kCmdToggleCursorOverlay = 5006;
static constexpr UINT kCmdOverlayPosBase = 6000;

bool TrayIcon::Init(HINSTANCE hInstance, HWND hWnd) {
    hWnd_ = hWnd;
    hInstance_ = hInstance;
    ZeroMemory(&nid_, sizeof(nid_));
    nid_.cbSize = sizeof(NOTIFYICONDATA);
    nid_.hWnd = hWnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_APP + 1;
    nid_.hIcon = LoadIcon(hInstance_, MAKEINTRESOURCE(101)); // Use custom icon from resources
    lstrcpy(nid_.szTip, TEXT("ArinCapture"));
    Shell_NotifyIcon(NIM_ADD, &nid_);
    // Create popup menu
    hMenu_ = CreatePopupMenu();
    UpdateMenu({}, -1, false);
    return true;
}

static int TrackPopupMenuAtPoint(HMENU hMenu, HWND ownerHwnd, POINT pt) {
    if (!hMenu || !ownerHwnd) return 0;

    // Ensure the menu is positioned within the monitor work-area (not behind the taskbar).
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    RECT work{};
    if (mon && GetMonitorInfo(mon, &mi)) {
        work = mi.rcWork;
    } else {
        SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
    }

    // Clamp point into work area.
    int x = pt.x;
    int y = pt.y;
    if (x < work.left) x = work.left;
    if (x > work.right) x = work.right;
    if (y < work.top) y = work.top;
    if (y > work.bottom) y = work.bottom;

    // Prefer anchoring to the bottom of the work area so the menu grows upward.
    y = work.bottom;

    // Choose horizontal alignment to keep the menu on-screen.
    const int midX = (work.left + work.right) / 2;
    const UINT horizAlign = (pt.x >= midX) ? TPM_RIGHTALIGN : TPM_LEFTALIGN;

    SetForegroundWindow(ownerHwnd);
    int cmd = TrackPopupMenuEx(
        hMenu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | horizAlign,
        x,
        y,
        ownerHwnd,
        nullptr);
    PostMessage(ownerHwnd, WM_NULL, 0, 0);
    return cmd;
}

void TrayIcon::ShowMenu(const std::vector<std::wstring>& outputMonitorNames, int currentOutputIndex, bool isFullscreen) {
    UpdateMenu(outputMonitorNames, currentOutputIndex, isFullscreen);
    POINT pt{};
    GetCursorPos(&pt);

    const int cmd = TrackPopupMenuAtPoint(hMenu_, hWnd_, pt);
    if (cmd == 0) return;

    if (cmd == kCmdExit) {
        PostMessage(hWnd_, WM_CLOSE, 0, 0);
    } else if (cmd == kCmdStopCapture) {
        SetCaptureActive(false);
        PostMessage(hWnd_, WM_APP + 2, 0, 0);
    } else if (cmd == kCmdStartCaptureMonitor) {
        SetCaptureActive(true);
        PostMessage(hWnd_, WM_APP + 2, 1, 0);
    } else if (cmd == kCmdStartCaptureWindow) {
        SetCaptureActive(true);
        PostMessage(hWnd_, WM_APP + 2, 2, 0);
    } else if (cmd == kCmdStartCaptureActiveWindow) {
        SetCaptureActive(true);
        // Defer target selection: user can Alt+Tab to the desired window after closing the tray menu.
        PostMessage(hWnd_, WM_APP + 2, 3, 0);
    } else if (cmd == kCmdCycleOutput) {
        // Cycle output monitor
        PostMessage(hWnd_, WM_APP + 3, 0, 0);
    } else if (cmd == kCmdToggleFullscreen) {
        PostMessage(hWnd_, WM_APP + 4, 0, 0);
    } else if (cmd >= (int)kCmdSelectOutputBase && cmd < (int)kCmdFramerateBase) {
        // Direct select output monitor
        const UINT idx = (UINT)(cmd - kCmdSelectOutputBase);
        PostMessage(hWnd_, WM_APP + 3, (WPARAM)(idx + 1), 0);
    } else if (cmd >= (int)kCmdFramerateBase && cmd < (int)kCmdFramerateBase + 5) {
        // Framerate selection
        int idx = (int)(cmd - kCmdFramerateBase);
        SetFramerateIndex(idx);
        // Notify main window (WM_APP+10, wParam=framerate index)
        PostMessage(hWnd_, WM_APP + 10, (WPARAM)idx, 0);
    } else if (cmd == kCmdDiagnosticsOverlay) {
        SetDiagnosticsOverlay(!GetDiagnosticsOverlay());
        PostMessage(hWnd_, WM_APP + 11, (WPARAM)GetDiagnosticsOverlay(), 0);
    } else if (cmd >= (int)kCmdDiagnosticsOverlaySizeBase && cmd < (int)kCmdDiagnosticsOverlaySizeBase + 3) {
        int idx = (int)(cmd - kCmdDiagnosticsOverlaySizeBase);
        SetDiagnosticsOverlaySizeIndex(idx);
        UpdateMenu({}, -1, false);
        PostMessage(hWnd_, WM_APP + 19, (WPARAM)idx, 0);
    } else if (cmd >= (int)kCmdDiagnosticsOverlayModeBase && cmd < (int)kCmdDiagnosticsOverlayModeBase + 2) {
        int idx = (int)(cmd - kCmdDiagnosticsOverlayModeBase);
        SetDiagnosticsOverlayCompact(idx == 0);
        UpdateMenu({}, -1, false);
        PostMessage(hWnd_, WM_APP + 20, (WPARAM)(idx == 0 ? 1 : 0), 0);
    } else if (cmd >= (int)kCmdOverlayPosBase && cmd < (int)kCmdOverlayPosBase + 5) {
        int idx = (int)(cmd - kCmdOverlayPosBase);
        SetOverlayPositionIndex(idx);
        UpdateMenu({}, -1, false);
        PostMessage(hWnd_, WM_APP + 15, (WPARAM)idx, 0);
    } else if (cmd == kCmdToggleStereo) {
        SetStereoEnabled(!GetStereoEnabled());
        // Pass the desired new state explicitly to avoid any refactor-induced desync.
        PostMessage(hWnd_, WM_APP + 13, (WPARAM)GetStereoEnabled(), 0);
    } else if (cmd == kCmdStereoDepth) {
        PostMessage(hWnd_, WM_APP + 14, 0, 0);
    } else if (cmd == kCmdToggleClickThrough) {
        SetClickThroughEnabled(!GetClickThroughEnabled());
        PostMessage(hWnd_, WM_APP + 16, 0, 0);
    } else if (cmd == kCmdToggleCursorOverlay) {
        SetCursorOverlayEnabled(!GetCursorOverlayEnabled());
        PostMessage(hWnd_, WM_APP + 26, (WPARAM)(GetCursorOverlayEnabled() ? 1 : 0), 0);
    } else if (cmd == kCmdToggleVSync) {
        SetVSyncEnabled(!GetVSyncEnabled());
        PostMessage(hWnd_, WM_APP + 18, (WPARAM)GetVSyncEnabled(), 0);
    } else if (cmd == kCmdToggleExcludeFromCapture) {
        SetExcludeFromCaptureEnabled(!GetExcludeFromCaptureEnabled());
        PostMessage(hWnd_, WM_APP + 23, (WPARAM)GetExcludeFromCaptureEnabled(), 0);
    } else if (cmd >= (int)kCmdRenderResBase && cmd < (int)kCmdRenderResBase + 10) {
        // Render resolution preset selection (output-side downscale; does NOT resize the source window)
        int idx = (int)(cmd - kCmdRenderResBase);
        SetRenderResolutionIndex(idx);
        UpdateMenu({}, -1, false);
        PostMessage(hWnd_, WM_APP + 12, (WPARAM)idx, 0);
    }
}

void TrayIcon::ShowMenuAt(POINT anchorPt, const std::vector<std::wstring>& outputMonitorNames, int currentOutputIndex, bool isFullscreen) {
    UpdateMenu(outputMonitorNames, currentOutputIndex, isFullscreen);

    const int cmd = TrackPopupMenuAtPoint(hMenu_, hWnd_, anchorPt);
    if (cmd == 0) return;

    // Re-use the same dispatch logic as ShowMenu by synthesizing a menu selection.
    // (Keep this in sync with ShowMenu.)
    if (cmd == kCmdExit) {
        PostMessage(hWnd_, WM_CLOSE, 0, 0);
    } else if (cmd == kCmdStopCapture) {
        SetCaptureActive(false);
        PostMessage(hWnd_, WM_APP + 2, 0, 0);
    } else if (cmd == kCmdStartCaptureMonitor) {
        SetCaptureActive(true);
        PostMessage(hWnd_, WM_APP + 2, 1, 0);
    } else if (cmd == kCmdStartCaptureWindow) {
        SetCaptureActive(true);
        PostMessage(hWnd_, WM_APP + 2, 2, 0);
    } else if (cmd == kCmdStartCaptureActiveWindow) {
        SetCaptureActive(true);
        PostMessage(hWnd_, WM_APP + 2, 3, 0);
    } else if (cmd == kCmdCycleOutput) {
        PostMessage(hWnd_, WM_APP + 3, 0, 0);
    } else if (cmd == kCmdToggleFullscreen) {
        PostMessage(hWnd_, WM_APP + 4, 0, 0);
    } else if (cmd >= (int)kCmdSelectOutputBase && cmd < (int)kCmdFramerateBase) {
        const UINT idx = (UINT)(cmd - kCmdSelectOutputBase);
        PostMessage(hWnd_, WM_APP + 3, (WPARAM)(idx + 1), 0);
    } else if (cmd >= (int)kCmdFramerateBase && cmd < (int)kCmdFramerateBase + 5) {
        int idx = (int)(cmd - kCmdFramerateBase);
        SetFramerateIndex(idx);
        PostMessage(hWnd_, WM_APP + 10, (WPARAM)idx, 0);
    } else if (cmd == kCmdDiagnosticsOverlay) {
        SetDiagnosticsOverlay(!GetDiagnosticsOverlay());
        PostMessage(hWnd_, WM_APP + 11, (WPARAM)GetDiagnosticsOverlay(), 0);
    } else if (cmd >= (int)kCmdDiagnosticsOverlaySizeBase && cmd < (int)kCmdDiagnosticsOverlaySizeBase + 3) {
        int idx = (int)(cmd - kCmdDiagnosticsOverlaySizeBase);
        SetDiagnosticsOverlaySizeIndex(idx);
        UpdateMenu({}, -1, false);
        PostMessage(hWnd_, WM_APP + 19, (WPARAM)idx, 0);
    } else if (cmd >= (int)kCmdDiagnosticsOverlayModeBase && cmd < (int)kCmdDiagnosticsOverlayModeBase + 2) {
        int idx = (int)(cmd - kCmdDiagnosticsOverlayModeBase);
        SetDiagnosticsOverlayCompact(idx == 0);
        UpdateMenu({}, -1, false);
        PostMessage(hWnd_, WM_APP + 20, (WPARAM)(idx == 0 ? 1 : 0), 0);
    } else if (cmd >= (int)kCmdOverlayPosBase && cmd < (int)kCmdOverlayPosBase + 5) {
        int idx = (int)(cmd - kCmdOverlayPosBase);
        SetOverlayPositionIndex(idx);
        UpdateMenu({}, -1, false);
        PostMessage(hWnd_, WM_APP + 15, (WPARAM)idx, 0);
    } else if (cmd == kCmdToggleStereo) {
        SetStereoEnabled(!GetStereoEnabled());
        PostMessage(hWnd_, WM_APP + 13, (WPARAM)GetStereoEnabled(), 0);
    } else if (cmd == kCmdStereoDepth) {
        PostMessage(hWnd_, WM_APP + 14, 0, 0);
    } else if (cmd == kCmdToggleClickThrough) {
        SetClickThroughEnabled(!GetClickThroughEnabled());
        PostMessage(hWnd_, WM_APP + 16, 0, 0);
    } else if (cmd == kCmdToggleCursorOverlay) {
        SetCursorOverlayEnabled(!GetCursorOverlayEnabled());
        PostMessage(hWnd_, WM_APP + 26, (WPARAM)(GetCursorOverlayEnabled() ? 1 : 0), 0);
    } else if (cmd == kCmdToggleVSync) {
        SetVSyncEnabled(!GetVSyncEnabled());
        PostMessage(hWnd_, WM_APP + 18, (WPARAM)GetVSyncEnabled(), 0);
    } else if (cmd == kCmdToggleExcludeFromCapture) {
        SetExcludeFromCaptureEnabled(!GetExcludeFromCaptureEnabled());
        PostMessage(hWnd_, WM_APP + 23, (WPARAM)GetExcludeFromCaptureEnabled(), 0);
    } else if (cmd >= (int)kCmdRenderResBase && cmd < (int)kCmdRenderResBase + 10) {
        int idx = (int)(cmd - kCmdRenderResBase);
        SetRenderResolutionIndex(idx);
        UpdateMenu({}, -1, false);
        PostMessage(hWnd_, WM_APP + 12, (WPARAM)idx, 0);
    }
}

void TrayIcon::Cleanup() {
    Shell_NotifyIcon(NIM_DELETE, &nid_);
    if (hMenu_) {
        DestroyMenu(hMenu_);
        hMenu_ = nullptr;
    }
}

void TrayIcon::SetCaptureActive(bool active) {
    captureActive_ = active;
    UpdateMenu({}, -1, false);
}

void TrayIcon::ShowPopup(const TCHAR* title, const TCHAR* text, UINT flags) {
    // Use a real popup dialog because Shell_NotifyIcon(NIF_INFO) balloons are unreliable
    // when the taskbar is configured to autohide.
    // Keep this lightweight (no custom UI) and always topmost so it isn't lost behind fullscreen apps.
    MessageBox(hWnd_, text ? text : TEXT(""), title ? title : TEXT("ArinCapture"), flags);
}

void TrayIcon::ShowBalloon(const TCHAR* title, const TCHAR* text) {
    ShowPopup(title, text);
}

void TrayIcon::UpdateMenu(const std::vector<std::wstring>& outputMonitorNames, int currentOutputIndex, bool isFullscreen) {
    if (!hMenu_) return;
    // Clear and rebuild menu
    while (DeleteMenu(hMenu_, 0, MF_BYPOSITION));
    AppendMenu(hMenu_, MF_STRING | MF_DISABLED, 0, TEXT("ArinCapture"));
    // Diagnostics overlay option always at the top
    AppendMenu(hMenu_, MF_STRING | (diagnosticsOverlay_ ? MF_CHECKED : 0), kCmdDiagnosticsOverlay, TEXT("Diagnostics Overlay"));

    // Diagnostics overlay size submenu
    {
        HMENU sizeMenu = CreatePopupMenu();
        static const struct { const wchar_t* label; } sizeOpts[] = {
            { L"Small" },
            { L"Medium" },
            { L"Large" },
        };
        for (int i = 0; i < 3; ++i) {
            UINT flags = MF_STRING;
            if (i == diagnosticsOverlaySizeIndex_) flags |= MF_CHECKED;
            AppendMenuW(sizeMenu, flags, kCmdDiagnosticsOverlaySizeBase + i, sizeOpts[i].label);
        }
        AppendMenu(hMenu_, MF_POPUP, (UINT_PTR)sizeMenu, TEXT("Diagnostics Overlay Size"));
    }

    // Diagnostics overlay content mode
    {
        HMENU modeMenu = CreatePopupMenu();
        UINT flagsCompact = MF_STRING | (diagnosticsOverlayCompact_ ? MF_CHECKED : 0);
        UINT flagsFull = MF_STRING | (!diagnosticsOverlayCompact_ ? MF_CHECKED : 0);
        AppendMenuW(modeMenu, flagsCompact, kCmdDiagnosticsOverlayModeBase + 0, L"Compact");
        AppendMenuW(modeMenu, flagsFull, kCmdDiagnosticsOverlayModeBase + 1, L"Full");
        AppendMenu(hMenu_, MF_POPUP, (UINT_PTR)modeMenu, TEXT("Diagnostics Overlay Content"));
    }

    // Overlay position submenu
    {
        HMENU posMenu = CreatePopupMenu();
        static const struct { const wchar_t* label; } posOpts[] = {
            { L"Top Left" },
            { L"Top Right" },
            { L"Bottom Left" },
            { L"Bottom Right" },
            { L"Center" },
        };
        for (int i = 0; i < 5; ++i) {
            UINT flags = MF_STRING;
            if (i == overlayPosIndex_) flags |= MF_CHECKED;
            AppendMenuW(posMenu, flags, kCmdOverlayPosBase + i, posOpts[i].label);
        }
        AppendMenu(hMenu_, MF_POPUP, (UINT_PTR)posMenu, TEXT("Overlay Position"));
    }

    // Stereo controls
    AppendMenu(hMenu_, MF_STRING | (stereoEnabled_ ? MF_CHECKED : 0), kCmdToggleStereo, TEXT("Stereo (Half-SBS)"));
    {
        TCHAR buf[128];
        wsprintf(buf, TEXT("Stereo Settings... (Depth %d)"), stereoDepthLevel_);
        // Allow configuring depth before capture starts.
        AppendMenu(hMenu_, MF_STRING, kCmdStereoDepth, buf);
    }
    AppendMenu(hMenu_, MF_SEPARATOR, 0, nullptr);

    // Framerate submenu
    HMENU frMenu = CreatePopupMenu();
    static const struct { const wchar_t* label; } frOpts[] = {
        { L"60 FPS" }, { L"72 FPS" }, { L"90 FPS" }, { L"120 FPS" }, { L"Unlimited" }
    };
    for (int i = 0; i < 5; ++i) {
        UINT flags = MF_STRING;
        if (i == framerateIndex_) flags |= MF_CHECKED;
        AppendMenuW(frMenu, flags, kCmdFramerateBase + i, frOpts[i].label);
    }
    AppendMenu(hMenu_, MF_POPUP, (UINT_PTR)frMenu, TEXT("Framerate"));

    // Present / VSync toggle (diagnostic)
    AppendMenu(hMenu_, MF_STRING | (vsyncEnabled_ ? MF_CHECKED : 0), kCmdToggleVSync, TEXT("VSync (Present sync)"));

    // Render resolution submenu (output-side downscale; does NOT resize the source window)
    HMENU resMenu = CreatePopupMenu();
    static const struct { const wchar_t* label; } resOpts[] = {
        { L"Native (no downscale)" },
        { L"1280 x 720" },
        { L"1600 x 900" },
        { L"1920 x 1080" },
        { L"2560 x 1440" },
        { L"3840 x 2160" },
    };
    for (int i = 0; i < 6; ++i) {
        UINT flags = MF_STRING;
        if (i == renderResIndex_) flags |= MF_CHECKED;
        // Allow preselecting a preset even when not capturing; it applies immediately once rendering starts.
        AppendMenuW(resMenu, flags, kCmdRenderResBase + i, resOpts[i].label);
    }
    AppendMenu(hMenu_, MF_POPUP, (UINT_PTR)resMenu, TEXT("Render Resolution"));
    if (captureActive_) {
        AppendMenu(hMenu_, MF_STRING, kCmdStopCapture, TEXT("Stop Capture"));
    } else {
        AppendMenu(hMenu_, MF_STRING, kCmdStartCaptureMonitor, TEXT("Start Capture (Monitor)"));
        const bool hasCandidateWindows = WindowTargeting::HasAnyCandidateCapturedTargetWindow(hWnd_, nullptr);
        if (hasCandidateWindows) {
            AppendMenu(hMenu_, MF_STRING, kCmdStartCaptureWindow, TEXT("Start Capture (Select Window...)"));
        } else {
            AppendMenu(hMenu_, MF_STRING | MF_DISABLED, 0, TEXT("Start Capture (Select Window...) (No windows found)"));
        }
        AppendMenu(hMenu_, MF_STRING, kCmdStartCaptureActiveWindow, TEXT("Start Capture (Active Window)"));
    }

    // Output selection controls
    AppendMenu(hMenu_, MF_SEPARATOR, 0, nullptr);
    // Allow preselecting output-related options before capture starts.
    AppendMenu(hMenu_, MF_STRING, kCmdCycleOutput, TEXT("Cycle Output Monitor"));
    AppendMenu(hMenu_, MF_STRING | (isFullscreen ? MF_CHECKED : 0), kCmdToggleFullscreen, TEXT("Borderless Fullscreen"));
    AppendMenu(hMenu_, MF_STRING | (clickThroughEnabled_ ? MF_CHECKED : 0), kCmdToggleClickThrough, TEXT("Input Passthrough (Click-through)"));
    AppendMenu(hMenu_, MF_STRING | (cursorOverlayEnabled_ ? MF_CHECKED : 0), kCmdToggleCursorOverlay, TEXT("Cursor Overlay (Show Source Cursor)"));
    // If disabled, screen-capture apps (e.g., Virtual Desktop) can capture this window.
    AppendMenu(hMenu_, MF_STRING | (excludeFromCaptureEnabled_ ? MF_CHECKED : 0), kCmdToggleExcludeFromCapture, TEXT("Exclude Output Window From Capture"));

    if (!outputMonitorNames.empty()) {
        HMENU sub = CreatePopupMenu();
        for (UINT i = 0; i < (UINT)outputMonitorNames.size(); ++i) {
            UINT flags = MF_STRING;
            if ((int)i == currentOutputIndex) flags |= MF_CHECKED;
            AppendMenuW(sub, flags, kCmdSelectOutputBase + i, outputMonitorNames[i].c_str());
        }
        AppendMenu(hMenu_, MF_POPUP, (UINT_PTR)sub, TEXT("Output Monitor"));
    } else {
        AppendMenu(hMenu_, MF_STRING | MF_DISABLED, 0, TEXT("Output Monitor"));
    }

    AppendMenu(hMenu_, MF_SEPARATOR, 0, nullptr);
    AppendMenu(hMenu_, MF_STRING, kCmdExit, TEXT("Exit"));
}
