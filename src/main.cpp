#include "CaptureDXGI.h"
#include "CaptureWGC.h"
#include "TrayIcon.h"
#include "Renderer.h"
#include "DepthDialog.h"
#include "Log.h"
#include "Monitors.h"
#include "DxgiCrop.h"
#include "WindowTargeting.h"
#include "Settings.h"
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <atomic>
#include <thread>
#include <mmsystem.h>
#include <algorithm>
#include <string>
#include <cwctype>
#include <vector>
#include <winrt/base.h>

// DPI awareness for thos on high DPI displays who use scaling
// Without it, Windows can DPI-virtualize window/client sizes (e.g. 4K at 225% -> ~1707x960),
// and DXGI swapchains end up with the virtualised texture sixes, which causes blurry output
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE ((DPI_AWARENESS_CONTEXT)-3)
#endif

static void EnableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;

    using SetDpiCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto setCtx = (SetDpiCtxFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    if (setCtx) {
        if (setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) || setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) {
            Log::Info("DPI awareness: Per-monitor enabled");
        } else {
            Log::Info("DPI awareness: SetProcessDpiAwarenessContext failed (continuing)");
        }
        return;
    }

    // Fallback for older systems.
    using SetDpiAwareFn = BOOL(WINAPI*)();
    auto setAware = (SetDpiAwareFn)GetProcAddress(user32, "SetProcessDPIAware");
    if (setAware) {
        setAware();
        Log::Info("DPI awareness: System-aware enabled");
    }
}

// executable versioning info, embedded into the binary at compile
#ifndef AC_BUILD_CONFIG
#define AC_BUILD_CONFIG "1.0.0"
#endif
#ifndef AC_GIT_SHA
#define AC_GIT_SHA "nogit"
#endif
#ifndef AC_PROJECT_VERSION
#define AC_PROJECT_VERSION "Alpha 0.0.14"
#endif

static std::string BuildIdString() {
    // Embedded into the binary via string literals so packaged builds can be traced.
    return std::string("BuildId v") + AC_PROJECT_VERSION + " " + AC_BUILD_CONFIG +
        " git=" + AC_GIT_SHA +
        " built=" + std::string(__DATE__) + " " + std::string(__TIME__);
}

// Some SDKs may not define this yet; value is documented by Microsoft.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// Application private window messages
static constexpr UINT WM_APP_UPDATE_RENDER_TIMER = WM_APP + 17;
static constexpr UINT WM_APP_FINALIZE_WINDOW_PICK = WM_APP + 21;

// Tray menu mirroring: keep the real Windows tray menu (for focus/input), but mirror its pixels
// into the SBS output so Virtual Desktop window-capture can see it.
static std::atomic<DWORD> g_trayMenuThreadId{ 0 };
static std::atomic<bool> g_trayMenuThreadActive{ false };

static HWND FindVisiblePopupMenuWindowForThread(DWORD tid);
static bool CaptureWindowToBGRA(HWND hwnd, std::vector<BYTE>& outBGRA, int& outW, int& outH);
static void UpdateTrayMenuOverlayFromSystem();

// Capture output window state
static bool g_outputFullscreen = false;
static bool g_defaultOutputFullscreen = true;
static DWORD g_uiThreadId = 0;
static bool g_renderWndNoActivate = false;
static bool g_cursorOverlay = false;
static bool g_windowSelectFollowTopmost = false;
static HWND g_windowSelectTargetRoot = nullptr;
static DWORD g_windowSelectTargetPid = 0;
static std::wstring g_windowSelectTitleHint;
static UINT g_windowSelectExpectedW = 0;
static UINT g_windowSelectExpectedH = 0;

enum class CaptureMode {
    Monitor,
    Window,
};

static CaptureMode g_captureMode = CaptureMode::Monitor;

// Window Select fallback: use DXGI monitor capture + crop to selected window client rect.
// NOTE: Disabled by default because DXGI output duplication can return black for some content
// like hardware overlay/video, which breaks SBS output in headset capture workflows.
static bool g_windowSelectPreferDxgiCrop = false;
static bool g_windowSelectDxgiCropActive = false;
static HWND g_windowSelectDxgiCropTarget = nullptr;
static RECT g_windowSelectDxgiCropMonitorRect = {0, 0, 0, 0};

// Active Window fallback: use DXGI monitor capture + crop to target window client rect.
// NOTE: Disabled by default for the same reason as Window Select.
static bool g_activeWindowPreferDxgiCrop = false;
static bool g_activeWindowDxgiCropActive = false;
static HWND g_activeWindowDxgiCropTarget = nullptr;
static RECT g_activeWindowDxgiCropMonitorRect = {0, 0, 0, 0};


// Click-through is implemented via WM_NCHITTEST (HTTRANSPARENT for HTCLIENT).
// In fullscreen, there is no non-client frame to interact with, so using WS_EX_TRANSPARENT is a reliable way to ensure passthrough even in edge cases.
// In windowed mode, avoid WS_EX_TRANSPARENT because it makes the frame non-interactable, and you can't move, resize, maximise/minimise it.
static void ApplyRenderWindowClickThrough(HWND hwnd, bool enabled) {
    if (!hwnd) return;
    LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (enabled) {
        ex |= WS_EX_LAYERED;
        if (g_outputFullscreen) {
            ex |= WS_EX_TRANSPARENT;
        } else {
            ex &= ~((LONG_PTR)WS_EX_TRANSPARENT);
        }
    } else {
        ex &= ~((LONG_PTR)WS_EX_LAYERED);
        ex &= ~((LONG_PTR)WS_EX_TRANSPARENT);
    }
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

// Prevents recursion
static void ApplyRenderWindowExcludeFromCapture(HWND hwnd, bool enabled) {
    if (!hwnd) return;
    const DWORD affinity = enabled ? (DWORD)WDA_EXCLUDEFROMCAPTURE : (DWORD)WDA_NONE;

    DWORD before = 0;
    const BOOL gotBefore = GetWindowDisplayAffinity(hwnd, &before);

    if (!SetWindowDisplayAffinity(hwnd, affinity)) {
        // Older OS versions or some window configurations may not support it.
        Log::Error(std::string("SetWindowDisplayAffinity failed (exclude-from-capture=" ) + (enabled ? "true" : "false") + ")");
        return;
    }

    DWORD after = 0;
    const BOOL gotAfter = GetWindowDisplayAffinity(hwnd, &after);

    Log::Info(
        std::string("DisplayAffinity set (exclude=") + (enabled ? "true" : "false") +
        ") requested=" + std::to_string((unsigned long long)affinity) +
        " before=" + (gotBefore ? std::to_string((unsigned long long)before) : std::string("?") ) +
        " after=" + (gotAfter ? std::to_string((unsigned long long)after) : std::string("?") )
    );
}

// Set the capture window to be on top for Window Select and Active Window
static void ApplyRenderWindowTopmost(HWND hwnd, bool topmost) {
    if (!hwnd) return;
    SetWindowPos(hwnd,
        topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
        0, 0, 0, 0,
    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | (topmost ? SWP_SHOWWINDOW : 0));
}

// Taskbar buttons are suppressed if the window is owned or marked as a tool window.
// TODO: This may not be functioning - need to test
static void EnsureRenderWindowShowsInTaskbar(HWND hwnd) {
    if (!hwnd) return;
    SetWindowLongPtr(hwnd, GWLP_HWNDPARENT, 0);

    LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    ex &= ~((LONG_PTR)WS_EX_TOOLWINDOW);
    ex |= (LONG_PTR)WS_EX_APPWINDOW;

    // Toggle WS_EX_APPWINDOW to force the shell/taskbar to re-evaluate.
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex & ~((LONG_PTR)WS_EX_APPWINDOW));

    // Double call to ensure the window is correctly re-evaluated for taskbar presence.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
}

// Some games throttle Present when they think they're fully occluded by another top-level window (frequently the case in single-monitor setups).
// TODO: This mayh not be functioning - need to test
static void UpdateRenderWindowAntiOcclusionRegion(HWND hwnd);

// When trying to restore the target window to the foreground, we may be blocked by Windows' foreground activation rules.
static void BeginForegroundRestoreAttempts(
    HWND trayHwnd,
    HWND target,
    const std::wstring& pickerTitle = L"",
    UINT expectedW = 0,
    UINT expectedH = 0);

// True if we successfully forced the target window to the foreground (or it was already foreground).
static bool TryForceForegroundToTargetWindow(HWND target) {
    if (!target || !IsWindow(target)) return false;

    // Allow any process to set foreground
    AllowSetForegroundWindow(ASFW_ANY);

    HWND fg = GetForegroundWindow();

    const DWORD ourThreadId = GetCurrentThreadId();
    const DWORD fgThreadId = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD targetThreadId = GetWindowThreadProcessId(target, nullptr);

    const bool attachedFg = (fgThreadId != 0 && ourThreadId != 0 && fgThreadId != ourThreadId &&
                             AttachThreadInput(ourThreadId, fgThreadId, TRUE) != FALSE);
    const bool attachedTarget = (targetThreadId != 0 && ourThreadId != 0 && targetThreadId != ourThreadId &&
                                 AttachThreadInput(ourThreadId, targetThreadId, TRUE) != FALSE);

    // Even if we can't attach to the target thread, we may still be able to bring it to the foreground
    ShowWindow(target, SW_SHOW);
    BringWindowToTop(target);
    SetForegroundWindow(target);

    if (attachedTarget) AttachThreadInput(ourThreadId, targetThreadId, FALSE);
    if (attachedFg) AttachThreadInput(ourThreadId, fgThreadId, FALSE);

    HWND nowFg = GetForegroundWindow();
    return (WindowTargeting::GetRootWindowOrSelf(nowFg) == WindowTargeting::GetRootWindowOrSelf(target));
}

// Set the window to fullscreen borderless if user has that preference enabled
static void ApplyOutputFullscreen(bool fullscreen);
// To avoid recursion when doing direct monitor capture without using WDA_EXCLUDEFROMCAPTURE, we can try to keep the output window on a different monitor from the captured one. This function checks if that's the case, and if not, moves the output to a different monitor if possible.
// TODO: May not be functioning - need to test
static bool GetEffectiveExcludeFromCapture();


// Forward declarations
LRESULT CALLBACK RenderWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Global handles for capture/render
static CaptureDXGI g_capture;
static CaptureWGC g_captureWgc;
static Renderer g_renderer;
static HWND g_trayWnd = nullptr;
static HWND g_renderWnd = nullptr;
static bool g_capturing = false;
static HWND g_activeWindowTarget = nullptr;
static HWND g_activeWindowTargetRoot = nullptr;
static bool g_activeWindowMode = false;
static std::wstring g_activeWindowTitleHint;
static bool g_windowSelectAwaitingTarget = false;
static HWND g_windowSelectLastForegroundRoot = nullptr;
static bool g_windowSelectIgnoreFirstForeground = false;

static HWND FindVisiblePopupMenuWindowForThread(DWORD tid) {
    if (tid == 0) return nullptr;
    struct Ctx {
        HWND found = nullptr;
    } ctx;

    EnumThreadWindows(
        tid,
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* c = (Ctx*)lParam;
            if (!c || c->found) return TRUE;
            if (!IsWindowVisible(hwnd)) return TRUE;
            wchar_t cls[64] = {};
            GetClassNameW(hwnd, cls, (int)(sizeof(cls) / sizeof(cls[0])));
            if (wcscmp(cls, L"#32768") != 0) return TRUE;
            c->found = hwnd;
            return FALSE;
        },
        (LPARAM)&ctx);

    return ctx.found;
}

static bool CaptureWindowToBGRA(HWND hwnd, std::vector<BYTE>& outBGRA, int& outW, int& outH) {
    outW = 0;
    outH = 0;
    if (!hwnd || !IsWindow(hwnd)) return false;

    RECT wr{};
    if (!GetWindowRect(hwnd, &wr)) return false;
    const int w = (int)(wr.right - wr.left);
    const int h = (int)(wr.bottom - wr.top);
    if (w <= 0 || h <= 0) return false;

    const size_t bytes = (size_t)w * (size_t)h * 4;
    outBGRA.resize(bytes);

    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC mem = CreateCompatibleDC(screen);
    if (!mem) {
        ReleaseDC(nullptr, screen);
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }

    HGDIOBJ old = SelectObject(mem, dib);

    BOOL ok = PrintWindow(hwnd, mem, 0);
    if (!ok) {
        ok = BitBlt(mem, 0, 0, w, h, screen, wr.left, wr.top, SRCCOPY | CAPTUREBLT);
    }

    if (ok) {
        memcpy(outBGRA.data(), bits, bytes);
        outW = w;
        outH = h;
    }

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    return ok != FALSE;
}

static void UpdateTrayMenuOverlayFromSystem() {
    static ULONGLONG s_lastCaptureMs = 0;
    static HWND s_lastMenuHwnd = nullptr;

    const DWORD tid = g_trayMenuThreadId.load();
    HWND menuHwnd = FindVisiblePopupMenuWindowForThread(tid);
    if (!menuHwnd) {
        g_renderer.SetMenuOverlayEnabled(false);
        s_lastMenuHwnd = nullptr;
        return;
    }

    RECT mr{};
    if (!GetWindowRect(menuHwnd, &mr)) {
        g_renderer.SetMenuOverlayEnabled(false);
        return;
    }

    HMONITOR mon = MonitorFromWindow(menuHwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!mon || !GetMonitorInfo(mon, &mi)) {
        g_renderer.SetMenuOverlayEnabled(false);
        return;
    }

    const RECT work = mi.rcWork;
    const int workW = (int)(work.right - work.left);
    const int workH = (int)(work.bottom - work.top);
    const float ww = (float)((workW > 1) ? workW : 1);
    const float wh = (float)((workH > 1) ? workH : 1);
    float l = ((float)mr.left - (float)work.left) / ww;
    float r = ((float)mr.right - (float)work.left) / ww;
    float t = ((float)mr.top - (float)work.top) / wh;
    float b = ((float)mr.bottom - (float)work.top) / wh;
    l = (l < 0.0f) ? 0.0f : (l > 1.0f ? 1.0f : l);
    r = (r < 0.0f) ? 0.0f : (r > 1.0f ? 1.0f : r);
    t = (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t);
    b = (b < 0.0f) ? 0.0f : (b > 1.0f ? 1.0f : b);

    // Enable + position can update every frame; the expensive pixel capture is throttled.
    g_renderer.SetMenuOverlayEnabled(true);
    g_renderer.SetMenuOverlayRectNormalized(l, t, r, b);

    const ULONGLONG nowMs = GetTickCount64();
    const bool menuChanged = (menuHwnd != s_lastMenuHwnd);
    if (!menuChanged && (nowMs - s_lastCaptureMs) < 33) {
        return;
    }

    std::vector<BYTE> bgra;
    int w = 0;
    int h = 0;
    if (!CaptureWindowToBGRA(menuHwnd, bgra, w, h) || w <= 0 || h <= 0) {
        g_renderer.SetMenuOverlayEnabled(false);
        s_lastMenuHwnd = nullptr;
        return;
    }
    g_renderer.UpdateMenuOverlayImageBGRA(bgra.data(), (UINT)w, (UINT)h);

    s_lastMenuHwnd = menuHwnd;
    s_lastCaptureMs = nowMs;
}

static ULONGLONG g_windowSelectPickCompleteMs = 0;
static bool g_windowPickFinalizePending = false;
static HWINEVENTHOOK g_foregroundHook = nullptr;
static int g_outputMonIndex = -1;
static bool g_stereoEnabled = false;
static int g_stereoDepthLevel = 12; // 1..20
static int g_stereoParallaxStrengthPercent = 50; // 0..100
static HWND g_stereoSettingsDlgHwnd = nullptr;
static int g_overlayPosIndex = 0; // 0=TL,1=TR,2=BL,3=BR,4=Center
static bool g_clickThrough = false;
static bool g_vsyncEnabled = true;
static bool g_excludeFromCapture = true;
static RECT g_outputWindowedRect = {0, 0, 0, 0};
static LONG_PTR g_outputWindowedStyle = 0;
static LONG_PTR g_outputWindowedExStyle = 0;

// Single-monitor usability: some games throttle Present when fully occluded by another top-level window.
// Keep a tiny "hole" in the fullscreen output window region so the game remains technically visible.
//TODO: This may not be functioning - need to test
static bool g_antiOcclusionHole = true;

// TODO: This may not be functioning - need to test. Some games may still consider the window occluded if it's fully covered by another top-level window, even with a tiny region excluded. In that case, we may need to dynamically update the region based on the position of other windows, or allow the user to specify a custom hole position.
static void UpdateRenderWindowAntiOcclusionRegion(HWND hwnd) {
    if (!hwnd) return;

    // Only apply this mitigation while capturing and fullscreen.
    if (!g_antiOcclusionHole || !g_capturing || !g_outputFullscreen) {
        SetWindowRgn(hwnd, nullptr, TRUE);
        return;
    }

    RECT cr{};
    if (!GetClientRect(hwnd, &cr)) {
        SetWindowRgn(hwnd, nullptr, TRUE);
        return;
    }

    const int w = (int)(cr.right - cr.left);
    const int h = (int)(cr.bottom - cr.top);
    if (w <= 4 || h <= 4) {
        SetWindowRgn(hwnd, nullptr, TRUE);
        return;
    }

    // NOTE: Some occlusion/throttle heuristics appear to be tile/threshold based.
    // A tiny 2x2 hole can still be treated as effectively occluded; use a slightly larger hole.
    const int hole = 32; // pixels
    HRGN full = CreateRectRgn(0, 0, w, h);
    HRGN holeRgn = CreateRectRgn(w - hole, h - hole, w, h);
    if (!full || !holeRgn) {
        if (holeRgn) DeleteObject(holeRgn);
        if (full) DeleteObject(full);
        SetWindowRgn(hwnd, nullptr, TRUE);
        return;
    }

    CombineRgn(full, full, holeRgn, RGN_DIFF);
    DeleteObject(holeRgn);

    // On success, the system owns the region handle.
    if (SetWindowRgn(hwnd, full, TRUE) == 0) {
        DeleteObject(full);
    }
}

//TODO: Up to here
static void UpdateActiveWindowOverlayTopmost(HWND foreground) {
    if (!g_activeWindowMode) return;

    HWND fgRoot = WindowTargeting::GetRootWindowOrSelf(foreground);

    // Picker mode: we may not have an HWND target; let the user manually switch to the chosen app.
    // Lock onto the first real foreground window the user switches to after selection.
    if (!g_activeWindowTargetRoot) {
        if (g_windowSelectAwaitingTarget && g_windowSelectIgnoreFirstForeground) {
            // The first foreground transition after the picker closes is often Windows restoring focus
            // to something unrelated (frequently File Explorer). Don't lock onto it unless it matches.
            g_windowSelectIgnoreFirstForeground = false;
            if (!fgRoot ||
                !WindowTargeting::IsCandidateCapturedTargetWindow(fgRoot, g_trayWnd, g_renderWnd) ||
                WindowTargeting::IsProbablyShellOrExplorerWindow(fgRoot)) {
                g_windowSelectLastForegroundRoot = fgRoot;
                return;
            }
            // Otherwise: allow lock below.
        }

        // Window Select no longer uses foreground-lock heuristics here.
        // The picker already returns the exact GraphicsCaptureItem we want to capture.
        // Active-window mode uses this path; window-select does not.
        return;
    }

    // Only apply topmost once the output window exists.
    if (!g_capturing || !g_renderWnd) return;

    const bool shouldBeTopmost = (fgRoot && fgRoot == g_activeWindowTargetRoot);
    ApplyRenderWindowTopmost(g_renderWnd, shouldBeTopmost);
}

static void UpdateWindowSelectOverlayTopmost(HWND foreground) {
    if (!g_capturing || !g_renderWnd) return;
    if (!g_windowSelectFollowTopmost) return;

    HWND fgRoot = WindowTargeting::GetRootWindowOrSelf(foreground);

    DWORD fgPid = WindowTargeting::GetWindowProcessIdSafe(fgRoot);

    // If the game recreates its top-level HWND on Alt+Tab, keep the target fresh by PID.
    if (g_windowSelectTargetPid != 0 && fgPid != 0 && fgPid == g_windowSelectTargetPid) {
        if (fgRoot &&
            WindowTargeting::IsCandidateCapturedTargetWindow(fgRoot, g_trayWnd, g_renderWnd) &&
            !WindowTargeting::IsProbablyShellOrExplorerWindow(fgRoot)) {
            g_windowSelectTargetRoot = fgRoot;
        }
    }

    // If we don't yet have a confirmed target (foreground-restore can fail due to Windows restrictions),
    // lock onto the first real foreground window that matches our picker hint.
    if (!g_windowSelectTargetRoot) {
        if (fgRoot &&
            WindowTargeting::IsCandidateCapturedTargetWindow(fgRoot, g_trayWnd, g_renderWnd) &&
            !WindowTargeting::IsProbablyShellOrExplorerWindow(fgRoot)) {
            bool matchesHint = false;
            if (!g_windowSelectTitleHint.empty()) {
                const std::wstring titleLower = WindowTargeting::ToLowerCopy(WindowTargeting::GetWindowTitleSafe(fgRoot));
                const std::wstring hintLower = WindowTargeting::ToLowerCopy(g_windowSelectTitleHint);
                if (!titleLower.empty() && !hintLower.empty() && titleLower.find(hintLower) != std::wstring::npos) {
                    matchesHint = true;
                }
            }

            bool matchesSize = false;
            if (g_windowSelectExpectedW > 0 && g_windowSelectExpectedH > 0) {
                UINT cw = 0, ch = 0;
                if (WindowTargeting::GetClientSizeSafe(fgRoot, &cw, &ch)) {
                    const int dw = (int)cw - (int)g_windowSelectExpectedW;
                    const int dh = (int)ch - (int)g_windowSelectExpectedH;
                    const int adw = dw < 0 ? -dw : dw;
                    const int adh = dh < 0 ? -dh : dh;
                    if (adw <= 32 && adh <= 32) {
                        matchesSize = true;
                    }
                }
            }

            if (matchesHint || matchesSize) {
                g_windowSelectTargetRoot = fgRoot;
                g_windowSelectTargetPid = fgPid;
            }
        }
    }

    // Prefer PID matching (robust across HWND changes). Fallback to root HWND match.
    bool shouldBeTopmost = false;
    if (g_windowSelectTargetPid != 0) {
        shouldBeTopmost = (fgPid != 0 && fgPid == g_windowSelectTargetPid);
    } else {
        shouldBeTopmost = (fgRoot && fgRoot == g_windowSelectTargetRoot);
    }

    ApplyRenderWindowTopmost(g_renderWnd, shouldBeTopmost);
}

static void CALLBACK ForegroundWinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
    LONG idObject, LONG idChild, DWORD, DWORD) {
    if (event != EVENT_SYSTEM_FOREGROUND) return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    UpdateActiveWindowOverlayTopmost(hwnd);
    UpdateWindowSelectOverlayTopmost(hwnd);
}

static void SetActiveWindowForegroundHookEnabled(bool enabled) {
    if (enabled) {
        if (g_foregroundHook) return;
        g_foregroundHook = SetWinEventHook(
            EVENT_SYSTEM_FOREGROUND,
            EVENT_SYSTEM_FOREGROUND,
            nullptr,
            ForegroundWinEventProc,
            0,
            0,
            WINEVENT_OUTOFCONTEXT);
    } else {
        if (g_foregroundHook) {
            UnhookWinEvent(g_foregroundHook);
            g_foregroundHook = nullptr;
        }
    }
}

// Improve WM_TIMER granularity while capturing.
static UINT g_timerResolutionRefCount = 0;
static void BeginHighResTimers() {
    if (g_timerResolutionRefCount++ == 0) {
        timeBeginPeriod(1);
    }
}
static void EndHighResTimers() {
    if (g_timerResolutionRefCount == 0) return;
    if (--g_timerResolutionRefCount == 0) {
        timeEndPeriod(1);
    }
}

static void UpdateOutputMonitorIndexFromWindow(HWND hwnd) {
    if (!hwnd) return;
    HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi = { sizeof(mi) };
    if (!GetMonitorInfoW(hMon, &mi)) return;

    auto monitors = Monitors::EnumerateMonitors();
    for (int i = 0; i < (int)monitors.size(); ++i) {
        if (monitors[i].name == mi.szDevice) {
            g_outputMonIndex = i;
            return;
        }
    }
}

static void ApplyOutputFullscreen(bool fullscreen) {
    if (!g_renderWnd) return;
    if (fullscreen == g_outputFullscreen) return;

    // If we're entering fullscreen, ensure the selected output monitor index matches
    // the monitor the window is currently on (users can move the window manually).
    if (fullscreen) {
        UpdateOutputMonitorIndexFromWindow(g_renderWnd);
    }

    auto monitors = Monitors::EnumerateMonitors();
    if (monitors.empty()) return;
    if (g_outputMonIndex < 0 || g_outputMonIndex >= (int)monitors.size()) g_outputMonIndex = 0;

    if (fullscreen) {
        GetWindowRect(g_renderWnd, &g_outputWindowedRect);
        g_outputWindowedStyle = GetWindowLongPtr(g_renderWnd, GWL_STYLE);
        g_outputWindowedExStyle = GetWindowLongPtr(g_renderWnd, GWL_EXSTYLE);

        SetWindowLongPtr(g_renderWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        // Keep the output window reachable (taskbar/Alt+Tab) even in Window Select.
        // WS_EX_TOOLWINDOW hides it from the taskbar and can strand the user.
        LONG_PTR ex = WS_EX_TOPMOST | WS_EX_APPWINDOW;
        // Avoid WS_EX_NOACTIVATE here: it can remove the window from the taskbar on some systems.
        // We still prevent focus stealing via SWP_NOACTIVATE + WM_MOUSEACTIVATE.
        SetWindowLongPtr(g_renderWnd, GWL_EXSTYLE, ex);
        g_outputFullscreen = true;

        // Overlay should not steal activation from the captured app in window-capture modes.
        g_renderWndNoActivate = (g_captureMode == CaptureMode::Window);

        EnsureRenderWindowShowsInTaskbar(g_renderWnd);

        const auto& mon = monitors[g_outputMonIndex];
        const UINT outW = (UINT)(mon.rect.right - mon.rect.left);
        const UINT outH = (UINT)(mon.rect.bottom - mon.rect.top);
        SetWindowPos(g_renderWnd, HWND_TOPMOST, mon.rect.left, mon.rect.top, (int)outW, (int)outH,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        g_renderer.Resize(outW, outH);

        // Re-apply click-through flag (fullscreen toggle rewrites exstyle).
        ApplyRenderWindowClickThrough(g_renderWnd, g_clickThrough);

        // Exclusion should only apply for direct monitor capture (otherwise it hides the window from headset capture).
        ApplyRenderWindowExcludeFromCapture(g_renderWnd, GetEffectiveExcludeFromCapture());
        UpdateRenderWindowAntiOcclusionRegion(g_renderWnd);
    } else {
        SetWindowLongPtr(g_renderWnd, GWL_STYLE,
            g_outputWindowedStyle ? g_outputWindowedStyle : (WS_OVERLAPPEDWINDOW | WS_VISIBLE));
        SetWindowLongPtr(g_renderWnd, GWL_EXSTYLE, g_outputWindowedExStyle);
        g_outputFullscreen = false;

        // Even when windowed, keep game focus/gamepad working in window-capture modes.
        g_renderWndNoActivate = (g_captureMode == CaptureMode::Window);

        EnsureRenderWindowShowsInTaskbar(g_renderWnd);

        const int w = g_outputWindowedRect.right - g_outputWindowedRect.left;
        const int h = g_outputWindowedRect.bottom - g_outputWindowedRect.top;
        SetWindowPos(g_renderWnd, HWND_NOTOPMOST,
            g_outputWindowedRect.left, g_outputWindowedRect.top, w, h,
            SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);

        RECT cr = {0, 0, 0, 0};
        GetClientRect(g_renderWnd, &cr);
        const UINT cw = (UINT)(cr.right - cr.left);
        const UINT ch = (UINT)(cr.bottom - cr.top);
        // NOTE: Do not resize swapchain buffers in windowed mode.
        // We keep the swapchain sized to the capture/native init size and let the OS scale it to the window.
        // This avoids timing-dependent WM_SIZE ordering differences between Debug/Release affecting output.

        // Re-apply click-through flag (fullscreen toggle rewrites exstyle).
        ApplyRenderWindowClickThrough(g_renderWnd, g_clickThrough);

        // Exclusion should only apply for direct monitor capture (otherwise it hides the window from headset capture).
        ApplyRenderWindowExcludeFromCapture(g_renderWnd, GetEffectiveExcludeFromCapture());
        UpdateRenderWindowAntiOcclusionRegion(g_renderWnd);
    }
}
// True only for Start Capture (Monitor). This lets us special-case recursion avoidance
// without changing Window Select / Active Window behavior.
static bool g_directMonitorCapture = false;
static std::wstring g_directMonitorCaptureDeviceName;
// When using DXGI output duplication (including DXGI-crop fallbacks), this is the device name we're capturing.
// Used to avoid recursion by moving the output window to a different monitor when possible.
static std::wstring g_dxgiCaptureDeviceName;

// Track which monitor the output window is currently on so we can re-apply display affinity
// if the user drags the window to a different monitor.
static HMONITOR g_renderWndLastMonitorForAffinity = nullptr;
static bool g_windowPickPending = false;
static int g_renderResPresetIndex = 0; // 0=Native (no downscale), 1..N presets
static bool g_pendingActiveWindowCapture = false;
static constexpr UINT_PTR kTimerStartActiveWindowCapture = 0xAC01;
static constexpr UINT_PTR kTimerRestoreForeground = 0xAC03;
static constexpr UINT_PTR kTimerWindowPickFinalizeWatchdog = 0xAC04;

static std::atomic<int> g_windowPickFinalizeStage{ 0 };
static std::atomic<ULONGLONG> g_windowPickFinalizeStartMs{ 0 };

static HWND g_pendingForegroundTarget = nullptr;
static int g_pendingForegroundAttempts = 0;
static std::wstring g_pendingPickerTitle;
static UINT g_pendingPickerExpectedW = 0;
static UINT g_pendingPickerExpectedH = 0;

static bool GetEffectiveExcludeFromCapture() {
    // WDA_EXCLUDEFROMCAPTURE makes the output window invisible to capture-based headset/streaming apps.
    // We only need recursion protection for *direct* monitor capture.
    if (!g_excludeFromCapture) return false;
    if (!g_directMonitorCapture) return false;

    // Safety: if we're in any window-select/active-window mode (including DXGI crop fallbacks),
    // do NOT exclude from capture; it breaks headset visibility.
    if (g_activeWindowMode) return false;
    if (g_windowSelectFollowTopmost) return false;
    if (g_windowSelectDxgiCropActive) return false;
    if (g_activeWindowDxgiCropActive) return false;

    // If the output window is NOT on the captured monitor, there's no recursion risk.
    // In that case, do NOT exclude from capture so headset apps can still see the output.
    if (g_renderWnd) {
        std::wstring captured = !g_dxgiCaptureDeviceName.empty() ? g_dxgiCaptureDeviceName : g_directMonitorCaptureDeviceName;
        if (!captured.empty()) {
            HMONITOR outMon = MonitorFromWindow(g_renderWnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXW mi{sizeof(mi)};
            if (outMon && GetMonitorInfoW(outMon, &mi)) {
                if (captured != mi.szDevice) {
                    return false;
                }
            }
        }
    }

    return true;
}

static void ChooseOutputMonitorAvoidingDxgiRecursion(const std::vector<Monitors::MonitorInfo>& monitors) {
    if (monitors.size() <= 1) return;
    if (g_captureMode != CaptureMode::Monitor) return;
    if (GetEffectiveExcludeFromCapture()) return;
    if (g_dxgiCaptureDeviceName.empty()) return;

    // If we aren't excluding the output window from capture, ensure the output is *not* on the captured monitor.
    // This avoids infinite mirror/recursion when using DXGI output duplication (including DXGI-crop fallbacks).
    for (int i = 0; i < (int)monitors.size(); ++i) {
        if (monitors[i].name != g_dxgiCaptureDeviceName) {
            if (g_outputMonIndex != i) {
                g_outputMonIndex = i;
                Log::Info("Output moved off captured monitor ('" + WindowTargeting::WideToUtf8(g_dxgiCaptureDeviceName) + "') to avoid recursion without display-affinity exclusion");
            }
            return;
        }
    }
}

static void ChooseDefaultOutputMonitorForMonitorCapture(const std::vector<Monitors::MonitorInfo>& monitors) {
    if (monitors.size() <= 1) return;
    if (!g_directMonitorCapture) return;

    const std::wstring captured = !g_dxgiCaptureDeviceName.empty() ? g_dxgiCaptureDeviceName : g_directMonitorCaptureDeviceName;
    if (captured.empty()) return;

    // Default output away from the captured monitor.
    // This avoids recursion even when we keep the window capturable (no WDA_EXCLUDEFROMCAPTURE),
    // which is required for headset apps that use screen/window capture.
    if (g_outputMonIndex >= 0 && g_outputMonIndex < (int)monitors.size()) {
        if (monitors[g_outputMonIndex].name != captured) return;
    }

    for (int i = 0; i < (int)monitors.size(); ++i) {
        if (monitors[i].name != captured) {
            g_outputMonIndex = i;
            Log::Info("Monitor capture: defaulting output to a different monitor to avoid recursion without hiding from capture apps");
            return;
        }
    }
}

static void ChooseOutputMonitorMatchingTargetWindow(const std::vector<Monitors::MonitorInfo>& monitors, HWND targetWindow) {
    if (monitors.empty()) return;
    if (!targetWindow || !IsWindow(targetWindow)) return;

    HMONITOR targetMon = MonitorFromWindow(targetWindow, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi{sizeof(mi)};
    if (!targetMon || !GetMonitorInfoW(targetMon, &mi)) return;

    const std::wstring targetDevice = mi.szDevice;
    if (targetDevice.empty()) return;

    for (int i = 0; i < (int)monitors.size(); ++i) {
        if (monitors[i].name == targetDevice) {
            g_outputMonIndex = i;
            Log::Info("Window capture: forcing output to same monitor as target window");
            return;
        }
    }
}

static void BestEffortRestoreTargetWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return;
    // Some apps produce no WGC frames while minimized.
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
}

static void LogExcludeFromCaptureState(const char* where) {
    Log::Info(
        std::string(where ? where : "ExcludeFromCapture") +
        ": opt=" + (g_excludeFromCapture ? "1" : "0") +
        " effective=" + (GetEffectiveExcludeFromCapture() ? "1" : "0") +
        " directMon=" + (g_directMonitorCapture ? "1" : "0") +
        " mode=" + std::string(g_captureMode == CaptureMode::Monitor ? "Monitor" : "Window") +
        " activeWin=" + (g_activeWindowMode ? "1" : "0") +
        " winSelFollow=" + (g_windowSelectFollowTopmost ? "1" : "0") +
        " winSelDxgiCrop=" + (g_windowSelectDxgiCropActive ? "1" : "0") +
        " activeDxgiCrop=" + (g_activeWindowDxgiCropActive ? "1" : "0")
    );
}

static void BeginForegroundRestoreAttempts(HWND trayHwnd, HWND target, const std::wstring& pickerTitle, UINT expectedW, UINT expectedH) {
    if (!trayHwnd) return;
    g_pendingForegroundTarget = target;
    g_pendingPickerTitle = pickerTitle;
    g_pendingPickerExpectedW = expectedW;
    g_pendingPickerExpectedH = expectedH;
    g_pendingForegroundAttempts = 0;
    SetTimer(trayHwnd, kTimerRestoreForeground, 50, nullptr);
}

static UINT GetRenderTimerPeriodMs() {
    const double interval = g_renderer.GetFrameInterval();
    // Unlimited: don't hammer the UI thread with a 1ms timer; it can starve input/message processing.
    if (interval <= 0.0) return 8;
    const double ms = interval * 1000.0;
    UINT out = (UINT)(ms + 0.5);
    if (out < 1) out = 1;
    return out;
}

static void SaveSettingsFromState(const TrayIcon& tray) {
    AppSettings s;
    s.stereoEnabled = g_stereoEnabled;
    s.stereoDepthLevel = g_stereoDepthLevel;
    s.stereoParallaxStrengthPercent = g_stereoParallaxStrengthPercent;

    s.vsyncEnabled = g_vsyncEnabled;
    s.clickThrough = g_clickThrough;
    s.cursorOverlay = g_cursorOverlay;
    s.excludeFromCapture = g_excludeFromCapture;
    s.overlayPosIndex = g_overlayPosIndex;

    s.diagnosticsOverlay = tray.GetDiagnosticsOverlay();
    s.diagnosticsOverlaySizeIndex = tray.GetDiagnosticsOverlaySizeIndex();
    s.diagnosticsOverlayCompact = tray.GetDiagnosticsOverlayCompact();

    s.framerateIndex = tray.GetFramerateIndex();
    s.renderResPresetIndex = g_renderResPresetIndex;

    s.Save();
}

static bool GetWindowClientRectInScreen(HWND hwnd, RECT* out) {
    if (!out) return false;
    if (!hwnd || !IsWindow(hwnd)) return false;
    RECT cr{};
    if (!GetClientRect(hwnd, &cr)) return false;
    POINT tl{ cr.left, cr.top };
    POINT br{ cr.right, cr.bottom };
    if (!ClientToScreen(hwnd, &tl)) return false;
    if (!ClientToScreen(hwnd, &br)) return false;
    out->left = tl.x;
    out->top = tl.y;
    out->right = br.x;
    out->bottom = br.y;
    return (out->right > out->left) && (out->bottom > out->top);
}

static bool TryGetCapturedSourceRectInScreen(RECT* out) {
    if (!out) return false;

    if (g_captureMode == CaptureMode::Monitor) {
        // Prefer window-client mapping when we are using a DXGI-crop fallback.
        if (g_activeWindowDxgiCropActive && g_activeWindowTargetRoot && IsWindow(g_activeWindowTargetRoot)) {
            return GetWindowClientRectInScreen(g_activeWindowTargetRoot, out);
        }
        if (g_windowSelectDxgiCropActive && g_windowSelectDxgiCropTarget && IsWindow(g_windowSelectDxgiCropTarget)) {
            HWND root = WindowTargeting::GetRootWindowOrSelf(g_windowSelectDxgiCropTarget);
            return GetWindowClientRectInScreen(root, out);
        }

        // Otherwise, map to the captured monitor rect in virtual-screen coordinates.
        const std::wstring captured = !g_dxgiCaptureDeviceName.empty() ? g_dxgiCaptureDeviceName : g_directMonitorCaptureDeviceName;
        if (!captured.empty()) {
            const auto monitors = Monitors::EnumerateMonitors();
            for (const auto& m : monitors) {
                if (m.name == captured) {
                    *out = m.rect;
                    return (out->right > out->left) && (out->bottom > out->top);
                }
            }
        }
        return false;
    }

    // WGC window capture: we can map if we know the target root HWND.
    if (g_activeWindowMode && g_activeWindowTargetRoot && IsWindow(g_activeWindowTargetRoot)) {
        return GetWindowClientRectInScreen(g_activeWindowTargetRoot, out);
    }
    if (g_windowSelectTargetRoot && IsWindow(g_windowSelectTargetRoot)) {
        return GetWindowClientRectInScreen(g_windowSelectTargetRoot, out);
    }
    return false;
}

static void UpdateSoftwareCursorFromSource() {
    if (!g_cursorOverlay) {
        g_renderer.SetSoftwareCursorEnabled(false);
        return;
    }

    RECT src{};
    if (!TryGetCapturedSourceRectInScreen(&src)) {
        g_renderer.SetSoftwareCursorEnabled(false);
        return;
    }

    POINT p{};
    if (!GetCursorPos(&p)) {
        g_renderer.SetSoftwareCursorEnabled(false);
        return;
    }

    if (p.x < src.left || p.x >= src.right || p.y < src.top || p.y >= src.bottom) {
        g_renderer.SetSoftwareCursorEnabled(false);
        return;
    }

    const int w = (int)(src.right - src.left);
    const int h = (int)(src.bottom - src.top);
    if (w <= 0 || h <= 0) {
        g_renderer.SetSoftwareCursorEnabled(false);
        return;
    }

    float x01 = (float)(p.x - src.left) / (float)w;
    float y01 = (float)(p.y - src.top) / (float)h;
    if (x01 < 0.0f) x01 = 0.0f;
    if (x01 > 1.0f) x01 = 1.0f;
    if (y01 < 0.0f) y01 = 0.0f;
    if (y01 > 1.0f) y01 = 1.0f;

    g_renderer.SetSoftwareCursorEnabled(true);
    g_renderer.SetSoftwareCursorPosNormalized(x01, y01);
}

static void RenderOneFrame(HWND hWnd) {
    static bool inRender = false;
    static ULONGLONG lastGoodFrameMs = 0;
    static bool stallStopPosted = false;
    if (inRender) return;
    if (!g_capturing) return;
    if (!hWnd || hWnd != g_renderWnd) return;
    if (IsIconic(hWnd)) return;

    inRender = true;

    UpdateSoftwareCursorFromSource();
    UpdateTrayMenuOverlayFromSystem();

    // If Window Select is using DXGI-crop fallback, keep the crop rect updated.
    // This is cheap (just a few Win32 calls + constant buffer update).
    if (g_captureMode == CaptureMode::Monitor) {
        DxgiCrop::CropState active;
        active.active = g_activeWindowDxgiCropActive;
        active.target = g_activeWindowDxgiCropTarget;
        active.monitorRect = g_activeWindowDxgiCropMonitorRect;

        DxgiCrop::CropState windowSelect;
        windowSelect.active = g_windowSelectDxgiCropActive;
        windowSelect.target = g_windowSelectDxgiCropTarget;
        windowSelect.monitorRect = g_windowSelectDxgiCropMonitorRect;

        DxgiCrop::UpdateDxgiWindowCropForRenderer(g_renderer, active, windowSelect);
    } else {
        g_renderer.ClearSourceCrop();
    }

    ID3D11Texture2D* frame = nullptr;
    INT64 frameTimestamp = 0;
    bool got = false;
    if (g_captureMode == CaptureMode::Monitor) {
        got = g_capture.GetFrame(&frame, &frameTimestamp);
    } else {
        got = g_captureWgc.GetFrame(&frame, &frameTimestamp);
    }

    // Capture stall watchdog:
    // - DXGI duplication can hard-fail (ACCESS_LOST/INVALID_CALL) when a fullscreen app changes modes.
    // When the capture is broken, stop capture so the tray/UI doesn't remain stuck in "running".
    {
        const ULONGLONG nowMs = GetTickCount64();
        if (got) {
            lastGoodFrameMs = nowMs;
            stallStopPosted = false;
        }
        if (g_captureMode == CaptureMode::Monitor) {
            // DXGI: don't treat WAIT_TIMEOUT as a stall (it just means no screen changes).
            const HRESULT hr = g_capture.GetLastAcquireNextFrameHr();
            if (!stallStopPosted && (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL || hr == DXGI_ERROR_ACCESS_DENIED)) {
                stallStopPosted = true;
                Log::Error("Capture lost (DXGI duplication): stopping capture to reset UI state");
                if (g_trayWnd) {
                    PostMessage(g_trayWnd, WM_APP + 2, 0, 2);
                }
            }
        }
    }

    // Push capture backend diagnostics (for HUD Cap: ...).
    if (g_captureMode == CaptureMode::Monitor) {
        g_renderer.SetCaptureStatsDXGI(g_capture.GetProducedFramesTotal(), g_capture.GetLastAccumulatedFrames());
    } else {
        g_renderer.SetCaptureStatsWGC(
            g_captureWgc.GetFrameArrivedCount(),
            g_captureWgc.GetFrameProducedCount(),
            g_captureWgc.GetFrameConsumedCount());
    }

    if (got) {
        g_renderer.UpdateRepeat(frameTimestamp);
        g_renderer.Render(frame, 0.0f);
        if (g_captureMode == CaptureMode::Monitor) {
            g_capture.ReleaseFrame();
        } else {
            g_captureWgc.ReleaseFrame();
        }
        frame->Release();
    } else {
        // Still present cached frame/overlay even if capture stalls.
        g_renderer.Render(nullptr, 0.0f);
    }

    inRender = false;
}

LRESULT CALLBACK TrayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static TrayIcon tray;
    switch (msg) {
    case WM_TIMER:
        if (wParam == kTimerStartActiveWindowCapture) {
            KillTimer(hWnd, kTimerStartActiveWindowCapture);
            if (!g_pendingActiveWindowCapture) break;
            g_pendingActiveWindowCapture = false;

            if (g_capturing) break;

            HWND target = GetForegroundWindow();
            if (!target || !IsWindow(target) || target == hWnd) {
                Log::Error("Active-window capture failed: please focus the target window and try again");
                tray.ShowPopup(
                    TEXT("ArinCapture"),
                    TEXT("Active-window capture failed.\r\nFocus the target window first, then try again."),
                    MB_OK | MB_ICONERROR | MB_TOPMOST
                );
                tray.SetCaptureActive(false);
                break;
            }

            Log::Info("Active-window capture: capturing foreground HWND after tray menu closed");

            // Re-use the normal start flow by posting WM_APP+2 again with the resolved HWND.
            PostMessage(hWnd, WM_APP + 2, 3, (LPARAM)target);
        } else if (wParam == kTimerRestoreForeground) {
            // Foreground restore is used by Active Window and Window Select.
            if (!g_capturing || (g_pendingForegroundTarget == nullptr && g_pendingPickerTitle.empty())) {
                KillTimer(hWnd, kTimerRestoreForeground);
                g_pendingForegroundTarget = nullptr;
                g_pendingPickerTitle.clear();
                g_pendingPickerExpectedW = 0;
                g_pendingPickerExpectedH = 0;
                g_pendingForegroundAttempts = 0;
                break;
            }

            // If we don't yet have a valid target (picker inference can be delayed), keep trying to discover it.
            if (!WindowTargeting::IsCandidateCapturedTargetWindow(g_pendingForegroundTarget, g_trayWnd, g_renderWnd)) {
                HWND candidate = nullptr;
                if (!g_pendingPickerTitle.empty() || (g_pendingPickerExpectedW > 0 && g_pendingPickerExpectedH > 0)) {
                    candidate = WindowTargeting::FindBestTopLevelWindowForFocusHint(
                        g_pendingPickerTitle,
                        g_pendingPickerExpectedW,
                        g_pendingPickerExpectedH,
                        g_trayWnd,
                        g_renderWnd);
                }
                if (!WindowTargeting::IsCandidateCapturedTargetWindow(candidate, g_trayWnd, g_renderWnd) && !g_pendingPickerTitle.empty()) {
                    candidate = WindowTargeting::FindTopLevelWindowByTitleExact(g_pendingPickerTitle, g_trayWnd, g_renderWnd);
                }
                if (!WindowTargeting::IsCandidateCapturedTargetWindow(candidate, g_trayWnd, g_renderWnd)) {
                    candidate = GetForegroundWindow();
                }
                if (WindowTargeting::IsCandidateCapturedTargetWindow(candidate, g_trayWnd, g_renderWnd)) {
                    g_pendingForegroundTarget = candidate;
                    if (g_activeWindowMode) {
                        g_activeWindowTarget = candidate;
                        g_activeWindowTargetRoot = WindowTargeting::GetRootWindowOrSelf(candidate);
                    }
                }
            }

            bool ok = WindowTargeting::IsCandidateCapturedTargetWindow(g_pendingForegroundTarget, g_trayWnd, g_renderWnd) &&
                      TryForceForegroundToTargetWindow(g_pendingForegroundTarget);

            // Window Select: once we actually restored foreground to the target, lock that root for topmost-follow.
            if (ok && g_windowSelectFollowTopmost && !g_windowSelectTargetRoot) {
                g_windowSelectTargetRoot = WindowTargeting::GetRootWindowOrSelf(g_pendingForegroundTarget);
                g_windowSelectTargetPid = WindowTargeting::GetWindowProcessIdSafe(g_pendingForegroundTarget);
            }

            // Window Select: don't fight the user.
            // If the user Alt+Tabs away while we're still attempting a best-effort focus restore,
            // stop trying immediately.
            if (!ok && g_renderWndNoActivate && g_pendingForegroundAttempts > 0) {
                HWND fg = GetForegroundWindow();
                HWND fgRoot = WindowTargeting::GetRootWindowOrSelf(fg);
                HWND targetRoot = WindowTargeting::GetRootWindowOrSelf(g_pendingForegroundTarget);
                if (fgRoot && fgRoot != targetRoot &&
                    fgRoot != g_trayWnd && fgRoot != g_renderWnd &&
                    !WindowTargeting::IsProbablyShellOrExplorerWindow(fgRoot)) {
                    ok = true;
                }
            }

            if (g_activeWindowMode) {
                UpdateActiveWindowOverlayTopmost(GetForegroundWindow());
            }
            UpdateWindowSelectOverlayTopmost(GetForegroundWindow());

            if (ok || (++g_pendingForegroundAttempts >= 12)) {
                KillTimer(hWnd, kTimerRestoreForeground);
                g_pendingForegroundTarget = nullptr;
                g_pendingPickerTitle.clear();
                g_pendingPickerExpectedW = 0;
                g_pendingPickerExpectedH = 0;
                g_pendingForegroundAttempts = 0;
            }
        } else if (wParam == kTimerWindowPickFinalizeWatchdog) {
            KillTimer(hWnd, kTimerWindowPickFinalizeWatchdog);
            const int stage = g_windowPickFinalizeStage.load(std::memory_order_relaxed);
            if (stage != 0) {
                const ULONGLONG start = g_windowPickFinalizeStartMs.load(std::memory_order_relaxed);
                const ULONGLONG now = GetTickCount64();
                Log::Error(
                    "Window select appears stuck (stage=" + std::to_string(stage) +
                    ", elapsedMs=" + std::to_string((unsigned long long)(now - start)) + ")"
                );
            }
        }
        break;
    case WM_APP + 12: {
        // Render resolution preset selection (output-side downscale; does NOT resize the source window)
        const int idx = (int)wParam;
        g_renderResPresetIndex = idx;
        tray.SetRenderResolutionIndex(idx);
        g_renderer.SetRenderResolutionIndex(idx);
        Log::Info("TrayWndProc: Render resolution preset index set to " + std::to_string(idx));
        SaveSettingsFromState(tray);
        break;
    }
    case WM_APP + 13:
        // Stereo toggle (Half-SBS)
        // Prefer explicit state from tray to avoid desync.
        g_stereoEnabled = (wParam != 0);
        tray.SetStereoEnabled(g_stereoEnabled);
        g_renderer.SetStereoEnabled(g_stereoEnabled);
        Log::Info(std::string("TrayWndProc: Stereo ") + (g_stereoEnabled ? "ON" : "OFF"));
        SaveSettingsFromState(tray);
        break;
    case WM_APP + 14: {
        // Stereo settings dialog (depth + parallax)
        if (g_stereoSettingsDlgHwnd && IsWindow(g_stereoSettingsDlgHwnd)) {
            SetForegroundWindow(g_stereoSettingsDlgHwnd);
            break;
        }

        DepthDialog dlg;

        const auto preview = [&](int depthPreview, int parallaxStrengthPreview) {
            tray.SetStereoDepthLevel(depthPreview);
            g_renderer.SetStereoDepthLevel(depthPreview);
            g_renderer.SetStereoParallaxStrengthPercent(parallaxStrengthPreview);
        };

        g_stereoSettingsDlgHwnd = dlg.ShowModeless(
            hWnd,
            g_stereoDepthLevel,
            g_stereoParallaxStrengthPercent,
            preview,
            [&](bool accepted, int depth, int parallaxStrengthPercent) {
                g_stereoSettingsDlgHwnd = nullptr;
                if (!accepted) return;

                g_stereoDepthLevel = depth;
                g_stereoParallaxStrengthPercent = parallaxStrengthPercent;
                preview(g_stereoDepthLevel, g_stereoParallaxStrengthPercent);

                Log::Info(
                    "TrayWndProc: Stereo depth=" + std::to_string(depth) +
                    " parallaxStrengthPercent=" + std::to_string(g_stereoParallaxStrengthPercent)
                );
                SaveSettingsFromState(tray);
            });

        if (!g_stereoSettingsDlgHwnd) {
            Log::Error("Failed to create Stereo Settings dialog");
        }
        break;
    }
    case WM_APP + 23:
        // Exclude output window from capture/recording (affects Virtual Desktop / OBS / etc)
        g_excludeFromCapture = (wParam != 0);
        tray.SetExcludeFromCaptureEnabled(g_excludeFromCapture);
        LogExcludeFromCaptureState("Tray toggle exclude");
        if (g_renderWnd) {
            ApplyRenderWindowExcludeFromCapture(g_renderWnd, GetEffectiveExcludeFromCapture());
        }
        Log::Info(std::string("TrayWndProc: Exclude-from-capture ") + (g_excludeFromCapture ? "ON" : "OFF"));
        SaveSettingsFromState(tray);
        break;
    case WM_APP + 15: {
        // Overlay position
        const int idx = (int)wParam;
        g_overlayPosIndex = idx;
        tray.SetOverlayPositionIndex(idx);
        g_renderer.SetOverlayPosition((Renderer::OverlayPosition)idx);
        Log::Info("TrayWndProc: Overlay position index set to " + std::to_string(idx));
        SaveSettingsFromState(tray);
        break;
    }
    case WM_APP + 16:
        // Input passthrough (click-through)
        g_clickThrough = !g_clickThrough;
        tray.SetClickThroughEnabled(g_clickThrough);
        g_renderWndNoActivate = (g_captureMode == CaptureMode::Window);
        if (g_renderWnd) {
            ApplyRenderWindowClickThrough(g_renderWnd, g_clickThrough);

            // Active Window: topmost should follow target foreground state (so Alt+Tab works).
            if (g_activeWindowMode) {
                UpdateActiveWindowOverlayTopmost(GetForegroundWindow());
            } else if (g_windowSelectFollowTopmost) {
                UpdateWindowSelectOverlayTopmost(GetForegroundWindow());
            } else {
                // Other modes: keep output window on top while still not stealing focus.
                // Fullscreen already forces TOPMOST; this ensures windowed mode behaves similarly when click-through is enabled.
                if (!g_outputFullscreen) {
                    ApplyRenderWindowTopmost(g_renderWnd, g_clickThrough);
                }
            }
        }
        Log::Info(std::string("TrayWndProc: Click-through ") + (g_clickThrough ? "ON" : "OFF"));
        SaveSettingsFromState(tray);
        break;

    case WM_APP + 26:
        // Cursor Overlay (draw cursor into output based on OS cursor over captured source)
        g_cursorOverlay = (wParam != 0);
        tray.SetCursorOverlayEnabled(g_cursorOverlay);
        if (!g_cursorOverlay) {
            g_renderer.SetSoftwareCursorEnabled(false);
        }
        Log::Info(std::string("TrayWndProc: Cursor Overlay ") + (g_cursorOverlay ? "ON" : "OFF"));
        SaveSettingsFromState(tray);
        break;

    case WM_APP + 18:
        // Present / VSync (diagnostic)
        g_vsyncEnabled = (wParam != 0);
        tray.SetVSyncEnabled(g_vsyncEnabled);
        g_renderer.SetVSyncEnabled(g_vsyncEnabled);
        Log::Info(std::string("TrayWndProc: VSync ") + (g_vsyncEnabled ? "ON" : "OFF"));
        SaveSettingsFromState(tray);
        break;
    case WM_APP + 11:
        // Diagnostics overlay toggle
        g_renderer.SetDiagnosticsOverlay(wParam != 0);
        Log::Info(std::string("TrayWndProc: Diagnostics overlay ") + (wParam ? "ON" : "OFF"));
        SaveSettingsFromState(tray);
        break;
    case WM_APP + 19:
        // Diagnostics overlay size (0..2)
        g_renderer.SetDiagnosticsOverlaySizeIndex((int)wParam);
        Log::Info("TrayWndProc: Diagnostics overlay size index set to " + std::to_string((int)wParam));
        SaveSettingsFromState(tray);
        break;
    case WM_APP + 20:
        // Diagnostics overlay content mode (1=Compact, 0=Full)
        g_renderer.SetDiagnosticsOverlayCompact(wParam != 0);
        Log::Info(std::string("TrayWndProc: Diagnostics overlay content ") + (wParam ? "Compact" : "Full"));
        SaveSettingsFromState(tray);
        break;
    case WM_APP + 10:
        // Framerate selection from tray
        g_renderer.SetFramerateIndex((int)wParam);
        Log::Info("TrayWndProc: Framerate set to index " + std::to_string((int)wParam));
        SaveSettingsFromState(tray);
        if (g_renderWnd) {
            PostMessage(g_renderWnd, WM_APP_UPDATE_RENDER_TIMER, 0, 0);
        }
        break;
    case WM_CREATE:
        tray.Init(((LPCREATESTRUCT)lParam)->hInstance, hWnd);
        // Apply loaded globals to tray + renderer (even before capture starts).
        tray.SetStereoEnabled(g_stereoEnabled);
        tray.SetStereoDepthLevel(g_stereoDepthLevel);
        tray.SetDiagnosticsOverlay(g_renderer.GetDiagnosticsOverlay());
        tray.SetDiagnosticsOverlaySizeIndex(g_renderer.GetDiagnosticsOverlaySizeIndex());
        tray.SetDiagnosticsOverlayCompact(g_renderer.GetDiagnosticsOverlayCompact());
        tray.SetFramerateIndex(g_renderer.GetFramerateIndex());
        tray.SetOverlayPositionIndex(g_overlayPosIndex);
        tray.SetClickThroughEnabled(g_clickThrough);
        tray.SetCursorOverlayEnabled(g_cursorOverlay);
        tray.SetVSyncEnabled(g_vsyncEnabled);
        tray.SetExcludeFromCaptureEnabled(g_excludeFromCapture);
        tray.SetRenderResolutionIndex(g_renderResPresetIndex);

        g_renderer.SetOverlayPosition((Renderer::OverlayPosition)g_overlayPosIndex);
        g_renderer.SetVSyncEnabled(g_vsyncEnabled);
        g_renderer.SetStereoEnabled(g_stereoEnabled);
        g_renderer.SetStereoDepthLevel(g_stereoDepthLevel);
        g_renderer.SetStereoParallaxStrengthPercent(g_stereoParallaxStrengthPercent);
        g_renderer.SetRenderResolutionIndex(g_renderResPresetIndex);
        g_renderer.SetStereoShaderMode(Renderer::StereoShaderMode::Depth3Pass);

        // Persist once on startup as a safe migration step:
        // - First run: creates the file
        // - Upgrades: writes newly-added keys (without changing the effective values)
        SaveSettingsFromState(tray);
        break;
    case WM_APP + 1:
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            std::vector<std::wstring> names;
            {
                // Allow selecting output monitor before capture starts.
                auto monitors = Monitors::EnumerateMonitors();
                names.reserve(monitors.size());
                for (const auto& m : monitors) {
                    std::wstring label = m.name;
                    if (m.primary) label += L" (Primary)";
                    const int w = m.rect.right - m.rect.left;
                    const int h = m.rect.bottom - m.rect.top;
                    label += L" [" + std::to_wstring(w) + L"x" + std::to_wstring(h) + L"]";
                    names.push_back(label);
                }
            }

            // Standard tray menu behavior (usable on the desktop). We intentionally do NOT mirror
            // the popup menu into the SBS output anymore to avoid duplicate menus.
            const bool menuFullscreenState = (g_capturing ? g_outputFullscreen : g_defaultOutputFullscreen);
            tray.ShowMenu(names, g_outputMonIndex, menuFullscreenState);
        }
        break;
    case WM_APP + 2:
        // Start/stop capture toggled from tray
        if (wParam != 0) {
            if (!g_capturing) {
                Log::Info("Starting capture...");
                // Determine Windows primary display device name (e.g. \\.\DISPLAY1)
                std::wstring primaryDisplayName;
                RECT primaryRect = {0, 0, 0, 0};
                if (!Monitors::GetPrimaryMonitorInfo(primaryDisplayName, primaryRect)) {
                    Log::Error("Failed to determine primary monitor; defaulting capture selection");
                }

                // Default output target: primary monitor (index into EnumerateMonitors)
                auto monitors = Monitors::EnumerateMonitors();
                if (g_outputMonIndex < 0 || g_outputMonIndex >= (int)monitors.size()) {
                    g_outputMonIndex = 0;
                    for (int i = 0; i < (int)monitors.size(); ++i) {
                        if (monitors[i].primary) {
                            g_outputMonIndex = i;
                            break;
                        }
                    }
                }
                // 1. Initialize capture first
                if (wParam == 1) {
                    g_captureMode = CaptureMode::Monitor;
                    g_directMonitorCapture = true;
                    g_directMonitorCaptureDeviceName = primaryDisplayName;
                    g_dxgiCaptureDeviceName = primaryDisplayName;
                    g_windowPickPending = false;
                    g_activeWindowMode = false;
                    g_activeWindowTarget = nullptr;
                    g_activeWindowTargetRoot = nullptr;
                    g_activeWindowTitleHint.clear();
                    g_windowSelectAwaitingTarget = false;
                    g_windowSelectLastForegroundRoot = nullptr;
                    SetActiveWindowForegroundHookEnabled(false);

                    // Recursion avoidance for direct monitor capture when exclusion is OFF:
                    // default the output window to a different monitor than the captured one.
                    if (!g_capture.Init(primaryDisplayName.empty() ? nullptr : primaryDisplayName.c_str())) {
                        Log::Error("Failed to initialize DXGI monitor capture.");
                    } else {
                        // Prefer the *actual* selected DXGI output name over any heuristic primary detection.
                        g_dxgiCaptureDeviceName = g_capture.GetCapturedOutputDeviceName();
                    }
                    LogExcludeFromCaptureState("StartCapture: Monitor");

                    // Now that we know which monitor DXGI is capturing, choose a safe default output monitor.
                    // (Avoid recursion without having to hide the output from capture apps/headsets.)
                    ChooseDefaultOutputMonitorForMonitorCapture(monitors);
                } else if (wParam == 2) {
                    g_captureMode = CaptureMode::Window;
                    g_directMonitorCapture = false;
                    g_directMonitorCaptureDeviceName.clear();
                    g_dxgiCaptureDeviceName.clear();
                    g_windowPickPending = true;
                    g_renderWndNoActivate = true;
                    g_activeWindowMode = false;
                    g_activeWindowTarget = nullptr;
                    g_activeWindowTargetRoot = nullptr;
                    g_activeWindowTitleHint.clear();
                    g_windowSelectAwaitingTarget = false;
                    g_windowSelectLastForegroundRoot = nullptr;
                    SetActiveWindowForegroundHookEnabled(false);
                    if (!g_captureWgc.InitPicker(hWnd)) {
                        Log::Error("Failed to initialize WGC window capture.");
                        g_windowPickPending = false;
                        g_captureWgc.Cleanup();
                        Log::Error("Window picker could not be opened.");
                        tray.ShowPopup(
                            TEXT("ArinCapture"),
                            TEXT("Window picker could not be opened.\r\n\r\nIf the target app is in exclusive fullscreen, switch it to windowed or borderless fullscreen and try again.\r\nOtherwise, use 'Start Capture (Active Window)'."),
                            MB_OK | MB_ICONERROR | MB_TOPMOST
                        );
                        tray.SetCaptureActive(false);
                        g_renderWndNoActivate = false;
                    }
                    LogExcludeFromCaptureState("StartCapture: Window Select");
                    // Picker is async; WM_APP+5 will finish starting capture.
                    break;
                } else if (wParam == 3) {
                    g_captureMode = CaptureMode::Window;
                    g_directMonitorCapture = false;
                    g_directMonitorCaptureDeviceName.clear();
                    g_dxgiCaptureDeviceName.clear();
                    g_windowPickPending = false;
                    g_activeWindowMode = true;
                    g_renderWndNoActivate = false;
                    g_activeWindowTitleHint.clear();
                    g_windowSelectAwaitingTarget = false;
                    g_windowSelectLastForegroundRoot = nullptr;

                    HWND target = (HWND)lParam;

                    // If no HWND provided, defer selection so the user can Alt+Tab to the target.
                    if (!target) {
                        g_activeWindowTarget = nullptr;
                        g_activeWindowTargetRoot = nullptr;
                        Log::Info("Active-window capture: focus the target window (Alt+Tab), starting in 3000ms...");
                        tray.ShowPopup(
                            TEXT("ArinCapture"),
                            TEXT("Select the window you want to capture now (Alt+Tab).\r\nCapture starts in 3 seconds."),
                            MB_OK | MB_ICONINFORMATION | MB_TOPMOST
                        );
                        g_pendingActiveWindowCapture = true;
                        SetTimer(hWnd, kTimerStartActiveWindowCapture, 3000, nullptr);
                        break;
                    }

                    g_activeWindowTarget = target;
                    g_activeWindowTargetRoot = WindowTargeting::GetRootWindowOrSelf(target);

                    if (!IsWindow(target) || target == hWnd) {
                        Log::Error("Active-window capture failed: invalid target window");
                        break;
                    }

                    // Ensure the target isn't minimized; minimized windows may not produce WGC frames.
                    BestEffortRestoreTargetWindow(g_activeWindowTargetRoot ? g_activeWindowTargetRoot : target);

                    // Output window: start on the primary monitor by default.
                    // (Users can move it manually; DXGI monitor-capture paths may still move it to avoid recursion.)

                    LogExcludeFromCaptureState("StartCapture: Active Window");

                    // Active Window: prefer DXGI monitor capture + crop for higher FPS (same idea as Window Select).
                    bool startedDxgiCrop = false;
                        if (g_activeWindowPreferDxgiCrop && g_excludeFromCapture) {
                        HMONITOR mon = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
                        MONITORINFOEXW mi{sizeof(mi)};
                        if (mon && GetMonitorInfoW(mon, &mi)) {
                            // If we can't safely exclude the output window and there's only one monitor,
                            // DXGI-crop fallback risks recursion and can break headset visibility. Use WGC instead.
                            auto monsNow = Monitors::EnumerateMonitors();
                            if (!GetEffectiveExcludeFromCapture() && monsNow.size() <= 1) {
                                Log::Info("Active-window capture: DXGI crop fallback disabled on single-monitor without display-affinity exclusion; using WGC");
                            } else if (g_capture.Init(mi.szDevice)) {
                                g_captureMode = CaptureMode::Monitor;
                                g_activeWindowDxgiCropActive = true;
                                g_activeWindowDxgiCropTarget = target;
                                g_activeWindowDxgiCropMonitorRect = mi.rcMonitor;
                                g_dxgiCaptureDeviceName = mi.szDevice;
                                startedDxgiCrop = true;
                                // Ensure WGC isn't holding resources.
                                g_captureWgc.Cleanup();
                                Log::Info("Active-window capture: using DXGI monitor capture + crop fallback");
                            }
                        }
                    }

                    if (!startedDxgiCrop) {
                        if (!g_captureWgc.StartCaptureFromWindow(target)) {
                            Log::Error("Failed to initialize WGC active-window capture.");
                            g_captureWgc.Cleanup();
                            break;
                        }
                        g_captureMode = CaptureMode::Window;
                        g_activeWindowDxgiCropActive = false;
                        g_activeWindowDxgiCropTarget = nullptr;
                        g_activeWindowDxgiCropMonitorRect = {0, 0, 0, 0};
                    }

                }
                // 2. Get capture frame size/format for swap chain
                ID3D11Texture2D* frame = nullptr;
                UINT width = 1280, height = 720;
                DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
                bool gotFirstFrame = false;
                if (g_captureMode == CaptureMode::Monitor) {
                    gotFirstFrame = g_capture.GetFrame(&frame);
                } else {
                    // WGC: avoid waiting on a first frame; the item size is known immediately.
                    {
                        UINT w = 0, h = 0;
                        if (g_captureWgc.GetCaptureItemSize(&w, &h) && w > 0 && h > 0) {
                            width = w;
                            height = h;
                        } else {
                            Log::Error("WGC: capture item size unavailable; using fallback 1280x720 for init");
                        }
                    }
                    format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    gotFirstFrame = false;
                }

                if (gotFirstFrame) {
                    D3D11_TEXTURE2D_DESC desc;
                    frame->GetDesc(&desc);
                    width = desc.Width;
                    height = desc.Height;
                    format = desc.Format;
                    if (g_captureMode == CaptureMode::Monitor) {
                        g_capture.ReleaseFrame();
                    } else {
                        g_captureWgc.ReleaseFrame();
                    }
                    frame->Release();
                }

                // If using DXGI output duplication (direct monitor capture or DXGI-crop fallbacks) and we're not
                // excluding the output window from capture, ensure the output is not placed on the captured monitor.
                ChooseOutputMonitorAvoidingDxgiRecursion(monitors);

                // Active Window DXGI crop: keep output sized to the target window (not the monitor).
                if (g_activeWindowDxgiCropActive && g_activeWindowTargetRoot) {
                    UINT cw = 0, ch = 0;
                    if (WindowTargeting::GetClientSizeSafe(g_activeWindowTargetRoot, &cw, &ch) && cw > 0 && ch > 0) {
                        width = cw;
                        height = ch;
                        format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    }
                }
                // 3. Create fullscreen borderless render window
                WNDCLASS rwc = {};
                rwc.lpfnWndProc = RenderWndProc;
                rwc.hInstance = GetModuleHandle(nullptr);
                rwc.lpszClassName = TEXT("ArinCaptureRenderClass");
                rwc.hCursor = LoadCursor(nullptr, IDC_ARROW);
                RegisterClass(&rwc);
                const UINT previewW = (width > 0 && width < 1280) ? width : 1280;
                const UINT previewH = (height > 0 && height < 720) ? height : 720;
                RECT wr = { 0, 0, (LONG)previewW, (LONG)previewH };
                AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
                g_outputFullscreen = false;
                const DWORD rwEx = WS_EX_APPWINDOW;
                g_renderWnd = CreateWindowEx(rwEx, rwc.lpszClassName, TEXT("ArinCapture Output"),
                    WS_OVERLAPPEDWINDOW,
                    0, 0, (wr.right - wr.left), (wr.bottom - wr.top),
                    nullptr, nullptr, rwc.hInstance, nullptr);
                if (!g_renderWnd) {
                    Log::Error("Failed to create render window.");
                } else {
                    EnsureRenderWindowShowsInTaskbar(g_renderWnd);
                    g_outputWindowedStyle = GetWindowLongPtr(g_renderWnd, GWL_STYLE);
                    g_outputWindowedExStyle = GetWindowLongPtr(g_renderWnd, GWL_EXSTYLE);
                    GetWindowRect(g_renderWnd, &g_outputWindowedRect);
                    // Move output window to selected output monitor (defaults to primary)
                    if (!monitors.empty() && g_outputMonIndex >= 0 && g_outputMonIndex < (int)monitors.size()) {
                        Monitors::MoveWindowToMonitor(g_renderWnd, monitors[g_outputMonIndex], g_outputFullscreen);
                    } else {
                        Log::Info("Moving render window to primary monitor at (" + std::to_string(primaryRect.left) + "," + std::to_string(primaryRect.top) + ")");
                        SetWindowPos(g_renderWnd, HWND_NOTOPMOST, primaryRect.left + 50, primaryRect.top + 50,
                            (wr.right - wr.left), (wr.bottom - wr.top), SWP_SHOWWINDOW | SWP_NOACTIVATE);
                    }

                    // If we're doing monitor capture and we put the output on that same captured monitor,
                    // disabling exclusion can create an infinite mirror/recursion effect.
                    if (!g_excludeFromCapture && g_directMonitorCapture &&
                        !monitors.empty() && g_outputMonIndex >= 0 && g_outputMonIndex < (int)monitors.size() &&
                        !primaryDisplayName.empty() && monitors[g_outputMonIndex].name == primaryDisplayName) {
                        Log::Info("Note: Output window is on the captured monitor with 'Exclude From Capture' disabled. This can cause recursion/mirror effects.");
                        tray.ShowPopup(
                            TEXT("ArinCapture"),
                            TEXT("Output is on the captured monitor with 'Exclude From Capture' OFF.\r\nThis may cause an infinite mirror effect.\r\nMove output to another monitor or enable exclusion."),
                            MB_OK | MB_ICONWARNING | MB_TOPMOST
                        );
                    }

                    ApplyRenderWindowClickThrough(g_renderWnd, g_clickThrough);
                    ApplyRenderWindowExcludeFromCapture(g_renderWnd, GetEffectiveExcludeFromCapture());
                    ShowWindow(g_renderWnd, SW_SHOWNOACTIVATE);
                }
                // 4. Initialize renderer (IMPORTANT: use the same D3D11 device/context as capture)
                ID3D11Device* dev = (g_captureMode == CaptureMode::Monitor) ? g_capture.GetDevice() : g_captureWgc.GetDevice();
                ID3D11DeviceContext* ctx = (g_captureMode == CaptureMode::Monitor) ? g_capture.GetContext() : g_captureWgc.GetContext();
                if (g_renderWnd && g_renderer.Init(g_renderWnd, width, height, format, dev, ctx)) {
                    BeginHighResTimers();
                    // Re-apply all user-facing render flags after Init/Cleanup.
                    g_renderer.SetDiagnosticsOverlay(tray.GetDiagnosticsOverlay());
                    g_renderer.SetDiagnosticsOverlaySizeIndex(tray.GetDiagnosticsOverlaySizeIndex());
                    g_renderer.SetDiagnosticsOverlayCompact(tray.GetDiagnosticsOverlayCompact());
                    g_renderer.SetRenderResolutionIndex(g_renderResPresetIndex);
                    g_renderer.SetStereoEnabled(g_stereoEnabled);
                    g_renderer.SetStereoDepthLevel(g_stereoDepthLevel);
                    g_renderer.SetStereoParallaxStrengthPercent(g_stereoParallaxStrengthPercent);
                    g_renderer.SetVSyncEnabled(g_vsyncEnabled);
                    tray.SetStereoEnabled(g_stereoEnabled);
                    tray.SetStereoDepthLevel(g_stereoDepthLevel);
                    tray.SetOverlayPositionIndex(g_overlayPosIndex);
                    g_renderer.SetOverlayPosition((Renderer::OverlayPosition)g_overlayPosIndex);
                    tray.SetClickThroughEnabled(g_clickThrough);
                    tray.SetCursorOverlayEnabled(g_cursorOverlay);
                    tray.SetVSyncEnabled(g_vsyncEnabled);
                    tray.SetExcludeFromCaptureEnabled(g_excludeFromCapture);
                    ApplyRenderWindowClickThrough(g_renderWnd, g_clickThrough);
                    ApplyRenderWindowExcludeFromCapture(g_renderWnd, GetEffectiveExcludeFromCapture());
                    g_renderWndNoActivate = (g_captureMode == CaptureMode::Window);
                    g_capturing = true;

                    // Default: open output window as borderless fullscreen.
                    if (g_defaultOutputFullscreen) {
                        UpdateOutputMonitorIndexFromWindow(g_renderWnd);
                        ApplyOutputFullscreen(true);
                    }

                    // Active-window mode: restore focus to the target so gamepad input works without Alt+Tab.
                    if (g_activeWindowMode && g_activeWindowTarget) {
                        SetActiveWindowForegroundHookEnabled(true);
                        BeginForegroundRestoreAttempts(hWnd, g_activeWindowTarget);
                    }

                    Log::Info("Capture started successfully.");
                } else {
                    Log::Error("Capture start failed. See previous errors.");
                }
            }
        } else {
            // Stop capture
            if (g_capturing) {
                if (lParam == 1) {
                    tray.ShowPopup(
                        TEXT("ArinCapture"),
                        TEXT("Capture stalled (no frames). Stopping capture to reset state.\r\n\r\nThis can happen if the captured window changes mode. Start capture again to resume."),
                        MB_OK | MB_ICONWARNING | MB_TOPMOST
                    );
                } else if (lParam == 2) {
                    tray.ShowPopup(
                        TEXT("ArinCapture"),
                        TEXT("Capture was lost (display mode change / fullscreen transition). Stopping capture to reset state.\r\n\r\nStart capture again to resume."),
                        MB_OK | MB_ICONWARNING | MB_TOPMOST
                    );
                }
                tray.SetCaptureActive(false);
                Log::Info("Stopping capture...");
                if (g_captureMode == CaptureMode::Monitor) {
                    g_capture.Cleanup();
                } else {
                    g_captureWgc.Cleanup();
                }
                g_renderer.Cleanup();
                EndHighResTimers();
                if (g_renderWnd) {
                    DestroyWindow(g_renderWnd);
                    g_renderWnd = nullptr;
                }
                g_capturing = false;
                g_outputFullscreen = false;
                g_directMonitorCapture = false;
                g_directMonitorCaptureDeviceName.clear();
                g_dxgiCaptureDeviceName.clear();
                g_activeWindowTarget = nullptr;
                g_activeWindowTargetRoot = nullptr;
                g_activeWindowMode = false;
                g_activeWindowTitleHint.clear();
                g_windowSelectAwaitingTarget = false;
                g_windowSelectLastForegroundRoot = nullptr;
                g_windowSelectFollowTopmost = false;
                g_windowSelectTargetRoot = nullptr;
                g_windowSelectTargetPid = 0;
                g_windowSelectTitleHint.clear();
                g_windowSelectExpectedW = 0;
                g_windowSelectExpectedH = 0;
                g_windowSelectDxgiCropActive = false;
                g_windowSelectDxgiCropTarget = nullptr;
                g_windowSelectDxgiCropMonitorRect = {0, 0, 0, 0};
                g_activeWindowDxgiCropActive = false;
                g_activeWindowDxgiCropTarget = nullptr;
                g_activeWindowDxgiCropMonitorRect = {0, 0, 0, 0};
                SetActiveWindowForegroundHookEnabled(false);
                KillTimer(hWnd, kTimerRestoreForeground);
                KillTimer(hWnd, kTimerWindowPickFinalizeWatchdog);
                g_pendingForegroundTarget = nullptr;
                g_pendingPickerTitle.clear();
                g_pendingForegroundAttempts = 0;
                g_windowPickFinalizePending = false;
                g_windowPickFinalizeStage.store(0, std::memory_order_relaxed);
                Log::Info("Capture stopped.");
            }
            if (g_windowPickPending) {
                g_captureWgc.Cleanup();
                g_windowPickPending = false;
            }
        }
        break;

    case WM_APP + 5: {
        // WGC picker completed (see CaptureWGC::InitPicker).
        if (!g_windowPickPending) break;
        g_windowPickPending = false;

        if (wParam == 0) {
            Log::Error("Window picker failed or canceled.");
            g_captureWgc.Cleanup();
            g_renderWndNoActivate = false;
            tray.ShowPopup(
                TEXT("ArinCapture"),
                TEXT("Window selection was canceled or failed.\r\n\r\nIf the target app is in exclusive fullscreen, switch it to windowed or borderless fullscreen and try again.\r\nOtherwise, try 'Start Capture (Active Window)'."),
                MB_OK | MB_ICONINFORMATION | MB_TOPMOST
            );
            tray.SetCaptureActive(false);
            break;
        }

        // Window Select: capture the literal picker-selected window.
        // Do not do any HWND inference/foreground locking here; it is error-prone.
        g_activeWindowMode = false;
        g_activeWindowTarget = nullptr;
        g_activeWindowTargetRoot = nullptr;
        g_activeWindowTitleHint.clear();
        // For Window Select, the picker provides no HWND. We lock onto the first real foreground window the user
        // switches to after selection (usually the game), and then use that for topmost-follow behavior.
        g_windowSelectAwaitingTarget = true;
        g_windowSelectLastForegroundRoot = nullptr;
        g_windowSelectIgnoreFirstForeground = true;
        g_windowSelectPickCompleteMs = GetTickCount64();
        SetActiveWindowForegroundHookEnabled(false);
        Log::Info("Window select: pick complete. Starting capture from picker item.");

        // Finalize on next message pump tick (keeps picker callback lightweight).
        if (!g_capturing) {
            g_windowPickFinalizePending = true;
            PostMessage(hWnd, WM_APP_FINALIZE_WINDOW_PICK, 0, 0);
        }

        break;

    }
    case WM_APP_FINALIZE_WINDOW_PICK: {
        if (!g_windowPickFinalizePending) break;
        if (g_capturing) break;

        g_windowPickFinalizePending = false;
        Log::Info("Window select: finalizing capture (deferred)...");

        g_windowPickFinalizeStartMs.store(GetTickCount64(), std::memory_order_relaxed);
        g_windowPickFinalizeStage.store(1, std::memory_order_relaxed);
        SetTimer(hWnd, kTimerWindowPickFinalizeWatchdog, 2000, nullptr);

        g_windowPickFinalizeStage.store(2, std::memory_order_relaxed);

        // Get picker hints up-front.
        std::wstring pickedName = g_captureWgc.GetPickedItemDisplayName();
        UINT pickedW = 1280, pickedH = 720;
        {
            UINT w = 0, h = 0;
            if (g_captureWgc.GetCaptureItemSize(&w, &h) && w > 0 && h > 0) {
                pickedW = w;
                pickedH = h;
            } else {
                Log::Error("Window select: picker item size unavailable; using fallback 1280x720 hints");
            }
        }

        // Prefer DXGI crop fallback if we can infer an HWND reliably.
        // If inference fails, fall back to literal WGC capture item.
        HWND inferredHwnd = nullptr;
        if (g_windowSelectPreferDxgiCrop && g_excludeFromCapture && !pickedName.empty()) {
            inferredHwnd = WindowTargeting::FindBestTopLevelWindowForFocusHint(pickedName, pickedW, pickedH, g_trayWnd, g_renderWnd);
            if (!WindowTargeting::IsCandidateCapturedTargetWindow(inferredHwnd, g_trayWnd, g_renderWnd)) {
                inferredHwnd = nullptr;
            }
        } else if (g_windowSelectPreferDxgiCrop && !g_excludeFromCapture) {
            Log::Info("Window select: DXGI crop fallback disabled because Exclude-from-capture is OFF (avoid recursion; using WGC). ");
        }

        LogExcludeFromCaptureState("Window select finalize");

        // Even when we choose WGC capture (no DXGI-crop), try to infer an HWND from picker hints.
        // Also best-effort restore the target if it is minimized (minimized windows may not produce WGC frames).
        HWND inferredTargetHwnd = nullptr;
        if (!pickedName.empty()) {
            inferredTargetHwnd = WindowTargeting::FindBestTopLevelWindowForFocusHint(pickedName, pickedW, pickedH, g_trayWnd, g_renderWnd);
            if (!WindowTargeting::IsCandidateCapturedTargetWindow(inferredTargetHwnd, g_trayWnd, g_renderWnd)) {
                inferredTargetHwnd = nullptr;
            }
        }

        // DXGI-crop fallback requires either:
        // - display-affinity exclusion (safe recursion avoidance but breaks headset capture), OR
        // - a second monitor to place the output window on (avoid recursion without breaking headset capture).
        const bool canAvoidRecursionWithoutAffinity = (Monitors::EnumerateMonitors().size() > 1);
        const bool allowDxgiCrop = (GetEffectiveExcludeFromCapture() || canAvoidRecursionWithoutAffinity);
        if (g_windowSelectPreferDxgiCrop && g_excludeFromCapture && !allowDxgiCrop) {
            Log::Info("Window select: DXGI crop fallback disabled on single-monitor without display-affinity exclusion; using WGC");
        }

        const bool useDxgiCrop = (g_windowSelectPreferDxgiCrop && g_excludeFromCapture && allowDxgiCrop && inferredHwnd != nullptr);
        if (useDxgiCrop) {
            HMONITOR mon = MonitorFromWindow(inferredHwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFOEXW mi{sizeof(mi)};
            if (!mon || !GetMonitorInfoW(mon, &mi)) {
                Log::Error("Window select (DXGI crop): failed to query monitor info; falling back to WGC");
            } else {
                Log::Info("Window select: using DXGI monitor capture + crop fallback for '" + WindowTargeting::WideToUtf8(pickedName) + "'");

                if (!g_capture.Init(mi.szDevice)) {
                    Log::Error("Window select (DXGI crop): failed to initialize DXGI monitor capture; falling back to WGC");
                } else {
                    // We only used WGC for the picker UI; tear it down now that DXGI is ready.
                    g_captureWgc.Cleanup();

                    g_captureMode = CaptureMode::Monitor;
                    g_windowSelectDxgiCropActive = true;
                    g_windowSelectDxgiCropTarget = inferredHwnd;
                    g_windowSelectDxgiCropMonitorRect = mi.rcMonitor;
                    g_dxgiCaptureDeviceName = mi.szDevice;

                    LogExcludeFromCaptureState("Window select finalize: DXGI crop");

                // Window Select: follow target foreground for topmost behavior (Alt+Tab away/back).
                g_windowSelectFollowTopmost = true;
                g_windowSelectTargetRoot = WindowTargeting::GetRootWindowOrSelf(inferredHwnd);
                g_windowSelectTargetPid = WindowTargeting::GetWindowProcessIdSafe(inferredHwnd);
                g_windowSelectTitleHint = pickedName;
                SetActiveWindowForegroundHookEnabled(true);

                // Create windowed preview render window using the picked item size (not the monitor size).
                const UINT width = (pickedW > 0) ? pickedW : 1280;
                const UINT height = (pickedH > 0) ? pickedH : 720;
                g_windowSelectExpectedW = width;
                g_windowSelectExpectedH = height;
                const DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;

                // Default output target: primary monitor
                std::wstring primaryDisplayName;
                RECT primaryRect = {0, 0, 0, 0};
                Monitors::GetPrimaryMonitorInfo(primaryDisplayName, primaryRect);
                auto monitors = Monitors::EnumerateMonitors();
                g_outputMonIndex = 0;
                for (int i = 0; i < (int)monitors.size(); ++i) {
                    if (monitors[i].primary) { g_outputMonIndex = i; break; }
                }

                ChooseOutputMonitorAvoidingDxgiRecursion(monitors);

                WNDCLASS rwc = {};
                rwc.lpfnWndProc = RenderWndProc;
                rwc.hInstance = GetModuleHandle(nullptr);
                rwc.lpszClassName = TEXT("ArinCaptureRenderClass");
                rwc.hCursor = LoadCursor(nullptr, IDC_ARROW);
                RegisterClass(&rwc);

                const UINT previewW = (width > 0 && width < 1280) ? width : 1280;
                const UINT previewH = (height > 0 && height < 720) ? height : 720;
                RECT wr = { 0, 0, (LONG)previewW, (LONG)previewH };
                AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
                g_outputFullscreen = false;

                g_windowPickFinalizeStage.store(4, std::memory_order_relaxed);
                // Window Select: do not steal activation, but keep the window reachable.
                const DWORD rwEx = WS_EX_APPWINDOW;
                g_renderWnd = CreateWindowEx(rwEx, rwc.lpszClassName, TEXT("ArinCapture Output"),
                    WS_OVERLAPPEDWINDOW,
                    0, 0, (wr.right - wr.left), (wr.bottom - wr.top),
                    nullptr, nullptr, rwc.hInstance, nullptr);

                if (!g_renderWnd) {
                    Log::Error("Failed to create render window.");
                    g_capture.Cleanup();
                    KillTimer(hWnd, kTimerWindowPickFinalizeWatchdog);
                    g_windowPickFinalizeStage.store(0, std::memory_order_relaxed);
                    break;
                }

                g_outputWindowedStyle = GetWindowLongPtr(g_renderWnd, GWL_STYLE);
                g_outputWindowedExStyle = GetWindowLongPtr(g_renderWnd, GWL_EXSTYLE);
                GetWindowRect(g_renderWnd, &g_outputWindowedRect);

                if (!monitors.empty() && g_outputMonIndex >= 0 && g_outputMonIndex < (int)monitors.size()) {
                    Monitors::MoveWindowToMonitor(g_renderWnd, monitors[g_outputMonIndex], g_outputFullscreen);
                } else {
                    SetWindowPos(g_renderWnd, HWND_NOTOPMOST, primaryRect.left + 50, primaryRect.top + 50,
                        (wr.right - wr.left), (wr.bottom - wr.top), SWP_SHOWWINDOW | SWP_NOACTIVATE);
                }
                ApplyRenderWindowClickThrough(g_renderWnd, g_clickThrough);
                ApplyRenderWindowExcludeFromCapture(g_renderWnd, GetEffectiveExcludeFromCapture());
                    EnsureRenderWindowShowsInTaskbar(g_renderWnd);
                ShowWindow(g_renderWnd, SW_SHOWNOACTIVATE);

                // Initialize renderer using DXGI device/context
                g_windowPickFinalizeStage.store(5, std::memory_order_relaxed);
                ID3D11Device* dev = g_capture.GetDevice();
                ID3D11DeviceContext* ctx = g_capture.GetContext();
                if (g_renderer.Init(g_renderWnd, width, height, format, dev, ctx)) {
                    BeginHighResTimers();
                    g_capturing = true;
                    g_renderer.SetRenderResolutionIndex(g_renderResPresetIndex);
                    g_renderer.SetStereoEnabled(g_stereoEnabled);
                    g_renderer.SetStereoDepthLevel(g_stereoDepthLevel);
                    g_renderer.SetVSyncEnabled(g_vsyncEnabled);
                    tray.SetStereoEnabled(g_stereoEnabled);
                    tray.SetStereoDepthLevel(g_stereoDepthLevel);
                    tray.SetOverlayPositionIndex(g_overlayPosIndex);
                    g_renderer.SetOverlayPosition((Renderer::OverlayPosition)g_overlayPosIndex);
                    tray.SetClickThroughEnabled(g_clickThrough);
                    tray.SetCursorOverlayEnabled(g_cursorOverlay);
                    tray.SetVSyncEnabled(g_vsyncEnabled);
                    ApplyRenderWindowClickThrough(g_renderWnd, g_clickThrough);
                    ApplyRenderWindowExcludeFromCapture(g_renderWnd, GetEffectiveExcludeFromCapture());
                    g_renderWndNoActivate = (g_captureMode == CaptureMode::Window);

                    if (g_defaultOutputFullscreen) {
                        UpdateOutputMonitorIndexFromWindow(g_renderWnd);
                        ApplyOutputFullscreen(true);
                    }

                    // Restore focus to the picked app so game input works.
                    BeginForegroundRestoreAttempts(hWnd, inferredHwnd, pickedName, width, height);

                    Log::Info("Window select (DXGI crop) started successfully.");
                } else {
                    Log::Error("Window select (DXGI crop) start failed.");
                    g_capture.Cleanup();
                }

                    KillTimer(hWnd, kTimerWindowPickFinalizeWatchdog);
                    g_windowPickFinalizeStage.store(0, std::memory_order_relaxed);
                    break;
                }
            }
        }

        // Fallback: WGC literal picker-selected item.
        LogExcludeFromCaptureState("Window select finalize: WGC fallback");
        const ULONGLONG pickStartMs = GetTickCount64();
        if (!g_captureWgc.StartCaptureFromPickedItem()) {
            Log::Error("Window capture could not be started after selection (StartCaptureFromPickedItem failed). ");
            g_captureWgc.Cleanup();
            KillTimer(hWnd, kTimerWindowPickFinalizeWatchdog);
            g_windowPickFinalizeStage.store(0, std::memory_order_relaxed);
            break;
        }
        Log::Info("Window select: StartCaptureFromPickedItem took " + std::to_string((unsigned long long)(GetTickCount64() - pickStartMs)) + "ms");

        g_windowPickFinalizeStage.store(3, std::memory_order_relaxed);

        // Default output target: primary monitor
        std::wstring primaryDisplayName;
        RECT primaryRect = {0, 0, 0, 0};
        Monitors::GetPrimaryMonitorInfo(primaryDisplayName, primaryRect);
        auto monitors = Monitors::EnumerateMonitors();
        g_outputMonIndex = 0;
        for (int i = 0; i < (int)monitors.size(); ++i) {
            if (monitors[i].primary) { g_outputMonIndex = i; break; }
        }

        if (inferredTargetHwnd) {
            BestEffortRestoreTargetWindow(WindowTargeting::GetRootWindowOrSelf(inferredTargetHwnd));
        }

        // Get capture size for swap chain (avoid blocking waits for a first frame in this handler).
        UINT width = 1280, height = 720;
        DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
        {
            UINT w = 0, h = 0;
            if (g_captureWgc.GetCaptureItemSize(&w, &h) && w > 0 && h > 0) {
                width = w;
                height = h;
            } else {
                Log::Error("Window select: capture item size unavailable; using fallback 1280x720 for init");
            }
        }

        // Create windowed preview render window
        WNDCLASS rwc = {};
        rwc.lpfnWndProc = RenderWndProc;
        rwc.hInstance = GetModuleHandle(nullptr);
        rwc.lpszClassName = TEXT("ArinCaptureRenderClass");
        rwc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClass(&rwc);

        const UINT previewW = (width > 0 && width < 1280) ? width : 1280;
        const UINT previewH = (height > 0 && height < 720) ? height : 720;
        RECT wr = { 0, 0, (LONG)previewW, (LONG)previewH };
        AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
        g_outputFullscreen = false;

        g_windowPickFinalizeStage.store(4, std::memory_order_relaxed);
        // Window Select: do not steal activation, but keep the window reachable.
        const DWORD rwEx = WS_EX_APPWINDOW;
        g_renderWnd = CreateWindowEx(rwEx, rwc.lpszClassName, TEXT("ArinCapture Output"),
            WS_OVERLAPPEDWINDOW,
            0, 0, (wr.right - wr.left), (wr.bottom - wr.top),
            nullptr, nullptr, rwc.hInstance, nullptr);

        if (!g_renderWnd) {
            Log::Error("Failed to create render window.");
            g_captureWgc.Cleanup();
            KillTimer(hWnd, kTimerWindowPickFinalizeWatchdog);
            g_windowPickFinalizeStage.store(0, std::memory_order_relaxed);
            break;
        }

        g_outputWindowedStyle = GetWindowLongPtr(g_renderWnd, GWL_STYLE);
        g_outputWindowedExStyle = GetWindowLongPtr(g_renderWnd, GWL_EXSTYLE);
        GetWindowRect(g_renderWnd, &g_outputWindowedRect);

        if (!monitors.empty() && g_outputMonIndex >= 0 && g_outputMonIndex < (int)monitors.size()) {
            Monitors::MoveWindowToMonitor(g_renderWnd, monitors[g_outputMonIndex], g_outputFullscreen);
        } else {
            SetWindowPos(g_renderWnd, HWND_NOTOPMOST, primaryRect.left + 50, primaryRect.top + 50,
                (wr.right - wr.left), (wr.bottom - wr.top), SWP_SHOWWINDOW | SWP_NOACTIVATE);
        }
        ApplyRenderWindowClickThrough(g_renderWnd, g_clickThrough);
        ApplyRenderWindowExcludeFromCapture(g_renderWnd, GetEffectiveExcludeFromCapture());
                EnsureRenderWindowShowsInTaskbar(g_renderWnd);
        ShowWindow(g_renderWnd, SW_SHOWNOACTIVATE);

        // Initialize renderer using WGC device/context
        g_windowPickFinalizeStage.store(5, std::memory_order_relaxed);
        const ULONGLONG wndStartMs = GetTickCount64();
        ID3D11Device* dev = g_captureWgc.GetDevice();
        ID3D11DeviceContext* ctx = g_captureWgc.GetContext();
        if (g_renderer.Init(g_renderWnd, width, height, format, dev, ctx)) {
            Log::Info("Window select: CreateWindow+Init renderer took " + std::to_string((unsigned long long)(GetTickCount64() - wndStartMs)) + "ms");
            BeginHighResTimers();
            g_captureMode = CaptureMode::Window;
            g_capturing = true;
            g_renderer.SetRenderResolutionIndex(g_renderResPresetIndex);
            g_renderer.SetStereoEnabled(g_stereoEnabled);
            g_renderer.SetStereoDepthLevel(g_stereoDepthLevel);
            g_renderer.SetVSyncEnabled(g_vsyncEnabled);
            tray.SetStereoEnabled(g_stereoEnabled);
            tray.SetStereoDepthLevel(g_stereoDepthLevel);
            tray.SetOverlayPositionIndex(g_overlayPosIndex);
            g_renderer.SetOverlayPosition((Renderer::OverlayPosition)g_overlayPosIndex);
            tray.SetClickThroughEnabled(g_clickThrough);
            tray.SetCursorOverlayEnabled(g_cursorOverlay);
            tray.SetVSyncEnabled(g_vsyncEnabled);
            ApplyRenderWindowClickThrough(g_renderWnd, g_clickThrough);
            ApplyRenderWindowExcludeFromCapture(g_renderWnd, GetEffectiveExcludeFromCapture());
            EnsureRenderWindowShowsInTaskbar(g_renderWnd);
            g_renderWndNoActivate = (g_captureMode == CaptureMode::Window);

            // Default: open output window as borderless fullscreen.
            if (g_defaultOutputFullscreen) {
                UpdateOutputMonitorIndexFromWindow(g_renderWnd);
                ApplyOutputFullscreen(true);
            }

            UpdateActiveWindowOverlayTopmost(GetForegroundWindow());

            // Window Select: restore focus to the picked app so game input works.
            // We don't have a reliable HWND from GraphicsCaptureItem, so use the picker display name as
            // a title hint and retry briefly via the existing foreground-restore timer.
            BeginForegroundRestoreAttempts(hWnd, nullptr, g_captureWgc.GetPickedItemDisplayName(), width, height);

            // Window Select: follow target foreground for topmost behavior (Alt+Tab away/back).
            g_windowSelectFollowTopmost = true;
            g_windowSelectTargetRoot = nullptr; // discovered by foreground restore
            g_windowSelectTargetPid = 0;
            g_windowSelectTitleHint = g_captureWgc.GetPickedItemDisplayName();
            g_windowSelectExpectedW = width;
            g_windowSelectExpectedH = height;
            g_windowSelectPickCompleteMs = GetTickCount64();
            SetActiveWindowForegroundHookEnabled(true);

            Log::Info("Window capture started successfully.");
        } else {
            Log::Error("Window capture start failed.");
            g_captureWgc.Cleanup();
        }

        KillTimer(hWnd, kTimerWindowPickFinalizeWatchdog);
        g_windowPickFinalizeStage.store(0, std::memory_order_relaxed);
        break;
    }
    case WM_APP + 3:
        // Output monitor control.
        // wParam == 0 -> cycle; wParam == (index+1) -> direct select.
        {
            auto monitors = Monitors::EnumerateMonitors();
            if (!monitors.empty()) {
                if (g_outputMonIndex < 0 || g_outputMonIndex >= (int)monitors.size()) g_outputMonIndex = 0;

                if (wParam == 0) {
                    g_outputMonIndex = (g_outputMonIndex + 1) % (int)monitors.size();
                } else {
                    const int requested = (int)wParam - 1;
                    if (requested >= 0 && requested < (int)monitors.size()) {
                        g_outputMonIndex = requested;
                    }
                }

                // If we're actively doing direct monitor capture without exclusion, never allow output to land
                // on the captured monitor (recursion). Only enforce this while capture is active.
                if (g_capturing && g_directMonitorCapture && !g_excludeFromCapture && !g_directMonitorCaptureDeviceName.empty() && monitors.size() > 1) {
                    if (monitors[g_outputMonIndex].name == g_directMonitorCaptureDeviceName) {
                        for (int i = 0; i < (int)monitors.size(); ++i) {
                            if (monitors[i].name != g_directMonitorCaptureDeviceName) {
                                g_outputMonIndex = i;
                                Log::Info("Monitor capture: output monitor matched captured monitor; moving output to avoid recursion");
                                break;
                            }
                        }
                    }
                }

                if (g_capturing && g_renderWnd) {
                    Monitors::MoveWindowToMonitor(g_renderWnd, monitors[g_outputMonIndex], g_outputFullscreen);

                    // Output moved: re-apply capture exclusion based on the new monitor.
                    ApplyRenderWindowExcludeFromCapture(g_renderWnd, GetEffectiveExcludeFromCapture());

                    // Some virtual displays (e.g., Virtual Desktop) can cause the swapchain to go black after a move.
                    // Refresh swapchain buffers on the new output.
                    if (!g_renderer.RefreshSwapChainForCurrentWindow()) {
                        Log::Error("TrayWndProc: swapchain refresh failed after monitor move");
                    }

                    if (g_outputFullscreen) {
                        // In fullscreen mode, follow the monitor size.
                        const UINT outW = (UINT)(monitors[g_outputMonIndex].rect.right - monitors[g_outputMonIndex].rect.left);
                        const UINT outH = (UINT)(monitors[g_outputMonIndex].rect.bottom - monitors[g_outputMonIndex].rect.top);
                        g_renderer.Resize(outW, outH);
                    }
                }
            }
        }
        break;

    case WM_APP + 4:
        // Toggle borderless fullscreen for output window.
        if (g_capturing && g_renderWnd) {
            UpdateOutputMonitorIndexFromWindow(g_renderWnd);
            ApplyOutputFullscreen(!g_outputFullscreen);
        } else {
            g_defaultOutputFullscreen = !g_defaultOutputFullscreen;
            Log::Info(std::string("Default output fullscreen ") + (g_defaultOutputFullscreen ? "ON" : "OFF"));
        }
        break;
    case WM_DESTROY:
        // Best-effort persist of the last in-memory state.
        SaveSettingsFromState(tray);
        tray.Cleanup();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK RenderWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static int paintCount = 0;
    static std::string lastFrameInfo;
    static int repeatCount = 0;
    switch (msg) {
    case WM_WINDOWPOSCHANGED: {
        // If the user drags the output window between monitors, the recursion/headset-capture behavior
        // can change. Re-apply display affinity when the monitor changes.
        if (hWnd == g_renderWnd) {
            HMONITOR now = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
            if (now && now != g_renderWndLastMonitorForAffinity) {
                g_renderWndLastMonitorForAffinity = now;
                ApplyRenderWindowExcludeFromCapture(g_renderWnd, GetEffectiveExcludeFromCapture());
            }
        }
        break;
    }
    case WM_MOUSEACTIVATE:
        // Window Select: keep the captured app as the active/foreground window.
        // Without this, a click on the overlay can steal activation and many games drop gamepad input.
        if (g_renderWndNoActivate) {
            // Allow interaction (move/resize/system buttons) but never activate this window.
            return MA_NOACTIVATE;
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    case WM_NCLBUTTONDOWN:
        // For non-activating overlay windows, Windows often requires activation before it will
        // process non-client interactions (caption buttons, move/size). Handle them manually.
        if (g_renderWndNoActivate) {
            const WPARAM hit = wParam;
            if (hit == HTCAPTION) {
                SendMessage(hWnd, WM_SYSCOMMAND, SC_MOVE + HTCAPTION, lParam);
                return 0;
            }

            int wmsz = -1;
            switch (hit) {
            case HTLEFT: wmsz = WMSZ_LEFT; break;
            case HTRIGHT: wmsz = WMSZ_RIGHT; break;
            case HTTOP: wmsz = WMSZ_TOP; break;
            case HTBOTTOM: wmsz = WMSZ_BOTTOM; break;
            case HTTOPLEFT: wmsz = WMSZ_TOPLEFT; break;
            case HTTOPRIGHT: wmsz = WMSZ_TOPRIGHT; break;
            case HTBOTTOMLEFT: wmsz = WMSZ_BOTTOMLEFT; break;
            case HTBOTTOMRIGHT: wmsz = WMSZ_BOTTOMRIGHT; break;
            default: break;
            }
            if (wmsz >= 0) {
                SendMessage(hWnd, WM_SYSCOMMAND, SC_SIZE + (WPARAM)wmsz, lParam);
                return 0;
            }
        }
        break;
    case WM_NCLBUTTONUP:
        if (g_renderWndNoActivate) {
            const WPARAM hit = wParam;
            if (hit == HTCLOSE) {
                PostMessage(hWnd, WM_CLOSE, 0, 0);
                return 0;
            }
            if (hit == HTMINBUTTON) {
                ShowWindow(hWnd, SW_MINIMIZE);
                return 0;
            }
            if (hit == HTMAXBUTTON) {
                ShowWindow(hWnd, IsZoomed(hWnd) ? SW_RESTORE : SW_MAXIMIZE);
                return 0;
            }
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    case WM_SETCURSOR:
        // Ensure we always set a cursor. If the window class has no cursor (or the underlying window
        // isn't receiving mouse messages due to this window being topmost), Windows can leave a
        // "busy" cursor stuck over the overlay.
        // Also: when we draw a software cursor into the output, hide the OS cursor over the output window
        // client area to avoid seeing two cursors.
        if (hWnd == g_renderWnd && g_capturing && g_cursorOverlay && g_captureMode == CaptureMode::Window) {
            if (LOWORD(lParam) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
        }
        if (!g_clickThrough) {
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
            return TRUE;
        }
        return DefWindowProc(hWnd, msg, wParam, lParam);
    case WM_CREATE:
        // Rendering is driven by the main message loop pacing (QPC) so it continues even when
        // the output window isn't foreground, without relying on coalesced WM_TIMER cadence.
        return 0;
    case WM_APP_UPDATE_RENDER_TIMER:
        // No-op: the main loop queries Renderer::GetFrameInterval() directly.
        return 0;
    case WM_SIZE:
        if (g_capturing && hWnd == g_renderWnd && wParam != SIZE_MINIMIZED) {
            const UINT w = (UINT)LOWORD(lParam);
            const UINT h = (UINT)HIWORD(lParam);
            // Only resize swapchain buffers in fullscreen mode.
            // In windowed mode, keep swapchain at capture/native size for consistent scaling/shader output.
            if (g_outputFullscreen && w > 0 && h > 0) {
                g_renderer.Resize(w, h);
            }
            UpdateRenderWindowAntiOcclusionRegion(hWnd);
        }
        break;
    case WM_EXITSIZEMOVE:
        if (!g_outputFullscreen && hWnd == g_renderWnd) {
            GetWindowRect(hWnd, &g_outputWindowedRect);
            UpdateOutputMonitorIndexFromWindow(hWnd);
        }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (g_trayWnd) {
                PostMessage(g_trayWnd, WM_APP + 2, 0, 0);
            } else {
                DestroyWindow(hWnd);
            }
        }
        break;
    case WM_ERASEBKGND:
        // We fully paint via the D3D swapchain; prevent GDI background erases that cause white flashes/blocks.
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
        break;
    }
    case WM_CLOSE:
        if (g_trayWnd) {
            PostMessage(g_trayWnd, WM_APP + 2, 0, 0);
        } else {
            DestroyWindow(hWnd);
        }
        break;
    case WM_DESTROY:
        break;
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    struct ScopedHandle {
        HANDLE h = nullptr;
        ~ScopedHandle() {
            if (h) CloseHandle(h);
        }
    } singleInstanceMutex;

    // Must be called before any UI is created.
    EnableDpiAwareness();

    // Dev/escape hatch: allow closing an existing tray-only instance even if the tray menu is broken.
    // Usage: ArinCaptureSBS.exe --shutdown
    {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        bool shutdown = false;
        if (argv) {
            for (int i = 1; i < argc; ++i) {
                if (argv[i] && lstrcmpiW(argv[i], L"--shutdown") == 0) {
                    shutdown = true;
                    break;
                }
            }
            LocalFree(argv);
        }

        if (shutdown) {
            HWND existing = FindWindowW(L"ArinCaptureTrayClass", L"ArinCapture");
            if (existing) {
                PostMessage(existing, WM_CLOSE, 0, 0);
            }
            return 0;
        }
    }

    singleInstanceMutex.h = CreateMutexW(nullptr, TRUE, L"Local\\ArinCaptureSBS_SingleInstance");
    if (singleInstanceMutex.h && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"ArinCapture is already running (check the system tray).", L"ArinCapture", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    winrt::init_apartment(winrt::apartment_type::single_threaded);
    g_uiThreadId = GetCurrentThreadId();
    Log::Info("WinMain UI thread id: " + std::to_string((int)g_uiThreadId));
    Log::Info("WinMain entered");
    Log::Info(BuildIdString());

    // Load persisted user settings (if present) before creating any UI.
    {
        const AppSettings s = AppSettings::Load();
        g_stereoEnabled = s.stereoEnabled;
        g_stereoDepthLevel = s.stereoDepthLevel;
        g_stereoParallaxStrengthPercent = s.stereoParallaxStrengthPercent;

        g_vsyncEnabled = s.vsyncEnabled;
        g_clickThrough = s.clickThrough;
        g_cursorOverlay = s.cursorOverlay;
        g_excludeFromCapture = s.excludeFromCapture;
        g_overlayPosIndex = s.overlayPosIndex;
        g_renderResPresetIndex = s.renderResPresetIndex;

        // Pre-seed renderer state so it behaves consistently even before capture starts.
        g_renderer.SetDiagnosticsOverlay(s.diagnosticsOverlay);
        g_renderer.SetDiagnosticsOverlaySizeIndex(s.diagnosticsOverlaySizeIndex);
        g_renderer.SetDiagnosticsOverlayCompact(s.diagnosticsOverlayCompact);
        g_renderer.SetOverlayPosition((Renderer::OverlayPosition)s.overlayPosIndex);
        g_renderer.SetFramerateIndex(s.framerateIndex);
        g_renderer.SetRenderResolutionIndex(s.renderResPresetIndex);
        g_renderer.SetVSyncEnabled(s.vsyncEnabled);
        g_renderer.SetStereoEnabled(s.stereoEnabled);
        g_renderer.SetStereoDepthLevel(s.stereoDepthLevel);
        g_renderer.SetStereoParallaxStrengthPercent(s.stereoParallaxStrengthPercent);
        g_renderer.SetStereoShaderMode(Renderer::StereoShaderMode::Depth3Pass);

        Log::Info(
            std::string("Settings summary:") +
            " stereoEnabled=" + std::to_string((int)s.stereoEnabled) +
            " depthLevel=" + std::to_string(s.stereoDepthLevel) +
            " parallaxStrengthPercent=" + std::to_string(s.stereoParallaxStrengthPercent) +
            " vsync=" + std::to_string((int)s.vsyncEnabled) +
            " cursorOverlay=" + std::to_string((int)s.cursorOverlay) +
            " renderResPresetIndex=" + std::to_string(s.renderResPresetIndex)
        );
    }
    // Register tray window class ONCE
    WNDCLASS wc = {};
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = TEXT("ArinCaptureTrayClass");
    RegisterClass(&wc);

    Log::Info("Calling CreateWindow for tray window");
    HWND hWnd = CreateWindow(TEXT("ArinCaptureTrayClass"), TEXT("ArinCapture"), 0,
        0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) { Log::Error("Failed to create tray window"); return 1; }
    Log::Info("Tray window created successfully");

    g_trayWnd = hWnd;

    // Message loop with frame pacing
    MSG msg;
    Log::Info("Entering message loop");

    LARGE_INTEGER qpf;
    QueryPerformanceFrequency(&qpf);
    LONGLONG nextFrameQpc = 0;

    for (;;) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                return 0;
            }

            if (g_stereoSettingsDlgHwnd && IsWindow(g_stereoSettingsDlgHwnd) && IsDialogMessage(g_stereoSettingsDlgHwnd, &msg)) {
                continue;
            }

            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (!(g_capturing && g_renderWnd)) {
            nextFrameQpc = 0;
            WaitMessage();
            continue;
        }

        const double intervalSec = g_renderer.GetFrameInterval();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        if (intervalSec <= 0.0) {
            // Unlimited: render as fast as possible, but still yield to the message queue.
            RenderOneFrame(g_renderWnd);
            MsgWaitForMultipleObjectsEx(0, nullptr, 0, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }

        const LONGLONG intervalTicks = (LONGLONG)(intervalSec * (double)qpf.QuadPart);
        if (intervalTicks <= 0) {
            RenderOneFrame(g_renderWnd);
            MsgWaitForMultipleObjectsEx(0, nullptr, 0, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }

        if (nextFrameQpc == 0) {
            nextFrameQpc = now.QuadPart + intervalTicks;
        }

        if (now.QuadPart >= nextFrameQpc) {
            RenderOneFrame(g_renderWnd);
            nextFrameQpc += intervalTicks;
            // If we fell far behind, resync instead of trying to "catch up".
            if (now.QuadPart - nextFrameQpc > intervalTicks * 4) {
                nextFrameQpc = now.QuadPart + intervalTicks;
            }
            continue;
        }

        const LONGLONG remaining = nextFrameQpc - now.QuadPart;
        DWORD timeoutMs = (DWORD)((remaining * 1000LL) / qpf.QuadPart);
        // Avoid a long oversleep; wake at least every 1ms.
        if (timeoutMs > 1) timeoutMs -= 1;
        MsgWaitForMultipleObjectsEx(0, nullptr, timeoutMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    return 0;
}
