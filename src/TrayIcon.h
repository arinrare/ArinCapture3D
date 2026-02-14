#pragma once
#include <windows.h>

#include <string>
#include <vector>

class TrayIcon {
public:
    bool Init(HINSTANCE hInstance, HWND hWnd);
    // Optionally provide monitor names for an output-target submenu.
    void ShowMenu(const std::vector<std::wstring>& outputMonitorNames = {}, int currentOutputIndex = -1, bool isFullscreen = false);
    // Show the menu anchored at a specific screen point (useful for showing on the output/SBS monitor).
    void ShowMenuAt(POINT anchorPt, const std::vector<std::wstring>& outputMonitorNames = {}, int currentOutputIndex = -1, bool isFullscreen = false);
    void Cleanup();
    void SetCaptureActive(bool active);
    // User-facing notifications.
    // NOTE: We use a modal popup instead of tray balloons because balloon toasts are unreliable
    // when the taskbar is set to autohide.
    void ShowPopup(const TCHAR* title, const TCHAR* text, UINT flags = MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    // Back-compat shim: historically this used tray balloons.
    void ShowBalloon(const TCHAR* title, const TCHAR* text);
    bool IsCaptureActive() const { return captureActive_; }

    // Framerate selection
    void SetFramerateIndex(int idx) { framerateIndex_ = idx; }
    int GetFramerateIndex() const { return framerateIndex_; }

    // Diagnostics overlay
    void SetDiagnosticsOverlay(bool enabled) { diagnosticsOverlay_ = enabled; }
    bool GetDiagnosticsOverlay() const { return diagnosticsOverlay_; }
    // Diagnostics overlay size (0..2)
    void SetDiagnosticsOverlaySizeIndex(int idx) { diagnosticsOverlaySizeIndex_ = idx; }
    int GetDiagnosticsOverlaySizeIndex() const { return diagnosticsOverlaySizeIndex_; }
    // Diagnostics overlay content mode
    void SetDiagnosticsOverlayCompact(bool compact) { diagnosticsOverlayCompact_ = compact; }
    bool GetDiagnosticsOverlayCompact() const { return diagnosticsOverlayCompact_; }

    // Overlay position (0..4)
    void SetOverlayPositionIndex(int idx) { overlayPosIndex_ = idx; }
    int GetOverlayPositionIndex() const { return overlayPosIndex_; }

    // Render resolution preset (output-side downscale). 0=Native, 1..N presets.
    void SetRenderResolutionIndex(int idx) { renderResIndex_ = (idx < 0 ? 0 : (idx > 5 ? 5 : idx)); }
    int GetRenderResolutionIndex() const { return renderResIndex_; }

    // Input passthrough (click-through)
    void SetClickThroughEnabled(bool enabled) { clickThroughEnabled_ = enabled; }
    bool GetClickThroughEnabled() const { return clickThroughEnabled_; }

    // Software cursor overlay (tracks OS cursor over captured source; no input forwarding)
    void SetCursorOverlayEnabled(bool enabled) { cursorOverlayEnabled_ = enabled; }
    bool GetCursorOverlayEnabled() const { return cursorOverlayEnabled_; }

    // Exclude output window from screen capture/recording (WDA_EXCLUDEFROMCAPTURE).
    // Disabled by default so Virtual Desktop/other capture apps can see the output window.
    // Enabling can help avoid recursion when using monitor capture on the same display.
    void SetExcludeFromCaptureEnabled(bool enabled) { excludeFromCaptureEnabled_ = enabled; }
    bool GetExcludeFromCaptureEnabled() const { return excludeFromCaptureEnabled_; }

    // Stereo (Half-SBS)
    void SetStereoEnabled(bool enabled) { stereoEnabled_ = enabled; }
    bool GetStereoEnabled() const { return stereoEnabled_; }
    void SetStereoDepthLevel(int level) { stereoDepthLevel_ = level; }
    int GetStereoDepthLevel() const { return stereoDepthLevel_; }

    // Present / VSync
    void SetVSyncEnabled(bool enabled) { vsyncEnabled_ = enabled; }
    bool GetVSyncEnabled() const { return vsyncEnabled_; }

private:
    void UpdateMenu(const std::vector<std::wstring>& outputMonitorNames, int currentOutputIndex, bool isFullscreen);
    NOTIFYICONDATA nid_{};
    HWND hWnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    HMENU hMenu_ = nullptr;
    bool captureActive_ = false;
    int framerateIndex_ = 0; // 0=60, 1=72, 2=90, 3=120, 4=Unlimited
    bool diagnosticsOverlay_ = false;
    int diagnosticsOverlaySizeIndex_ = 0; // 0=Small, 1=Medium, 2=Large
    bool diagnosticsOverlayCompact_ = true;
    int renderResIndex_ = 0; // 0=Native, 1..N presets

    int overlayPosIndex_ = 0; // 0=TopLeft, 1=TopRight, 2=BottomLeft, 3=BottomRight, 4=Center

    bool clickThroughEnabled_ = false;

    bool cursorOverlayEnabled_ = false;

    bool excludeFromCaptureEnabled_ = false;

    bool stereoEnabled_ = false;
    int stereoDepthLevel_ = 12;

    bool vsyncEnabled_ = true;
};
