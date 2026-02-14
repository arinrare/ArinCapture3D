#pragma once

#include <d3d11.h>
#include <memory>
#include <string>
#include <windows.h>

class CaptureWGC {
public:
    CaptureWGC();
    ~CaptureWGC();

    // Shows the system window picker and starts capturing the selected window.
    // Requires the target game to run in windowed/borderless-windowed mode.
    // Note: selection is asynchronous; the picker completion is reported back to the parent window via WM_APP+5.
    bool InitPicker(HWND parentWindow);

    // Finalizes the picker selection by creating the frame pool + capture session and starting capture.
    // IMPORTANT: Call this on the same thread/apartment that will call GetFrame (typically the main/UI thread).
    bool StartCaptureFromPickedItem();

    // Starts capturing a specific HWND (useful when you want to be able to resize the target window).
    // IMPORTANT: Call on the UI thread.
    bool StartCaptureFromWindow(HWND targetWindow);

    // True if this capture was started from a known HWND target.
    bool HasTargetWindow() const;

    // Attempts to resize the target window's client area to the requested dimensions.
    // Only works if HasTargetWindow() is true.
    // If clientWidth/clientHeight are 0, restores the original client size captured at StartCaptureFromWindow().
    bool ResizeTargetWindowClient(UINT clientWidth, UINT clientHeight);

    // Returns true and sets outTex and outTimestamp (QPC units) if a frame is available
    bool GetFrame(ID3D11Texture2D** outTex, INT64* outTimestamp = nullptr);
    void ReleaseFrame();
    void Cleanup();

    ID3D11Device* GetDevice() const;
    ID3D11DeviceContext* GetContext() const;

    // Returns the HWND of the captured window if available, or nullptr otherwise.
    HWND GetCapturedWindow() const;

    // Returns the display name of the last picker-selected item (if any).
    // Note: Windows doesn't provide a reliable way to map this back to an HWND in all cases,
    // but it can be useful as a best-effort hint.
    std::wstring GetPickedItemDisplayName() const;

    // Returns the most recent capture item's content size (best effort).
    // For picker selection, this is populated immediately when the user picks an item.
    bool GetCaptureItemSize(UINT* outWidth, UINT* outHeight) const;

    // Diagnostics: capture delivery counters.
    // FrameArrivedCount increments on FrameArrived; FrameConsumedCount increments when GetFrame returns a frame.
    unsigned long long GetFrameArrivedCount() const;
    // FrameProducedCount increments for each actual frame pulled from the frame pool (draining in FrameArrived).
    // This is the best proxy for "frames available to the app".
    unsigned long long GetFrameProducedCount() const;
    unsigned long long GetFrameConsumedCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
