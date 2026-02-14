#pragma once
#include <d3d11.h>
#include <dxgi1_2.h>

#include <string>

class CaptureDXGI {
public:
    // If targetDeviceName is null/empty, captures the first enumerated output.
    bool Init(const wchar_t* targetDeviceName = nullptr);
    // Acquires the next desktop frame and returns a texture you can use until you call ReleaseFrame().
    // Returns true and sets outTex and outTimestamp (QPC units) if a frame is available
    bool GetFrame(ID3D11Texture2D** outTex, INT64* outTimestamp = nullptr);
    // Must be called exactly once after each successful GetFrame().
    void ReleaseFrame();
    void Cleanup();

    ID3D11Device* GetDevice() const { return d3dDevice_; }
    ID3D11DeviceContext* GetContext() const { return d3dContext_; }

    // Diagnostics: capture delivery stats.
    // - ProducedFramesTotal: sum of DXGI_OUTDUPL_FRAME_INFO::AccumulatedFrames (clamped to at least 1 per acquired frame).
    // - LastAccumulatedFrames: value reported by the most recent successful AcquireNextFrame.
    unsigned long long GetProducedFramesTotal() const { return producedFramesTotal_; }
    UINT GetLastAccumulatedFrames() const { return lastAccumulatedFrames_; }

    // Diagnostics: last HRESULT returned by AcquireNextFrame.
    // - S_OK: last acquire succeeded
    // - DXGI_ERROR_WAIT_TIMEOUT: normal "no new frame yet"
    // - other failures (e.g. DXGI_ERROR_ACCESS_LOST) indicate capture is broken and must be restarted.
    HRESULT GetLastAcquireNextFrameHr() const { return lastAcquireHr_; }

    // Device name of the captured output (e.g. "\\.\\DISPLAY1"). Empty if not initialized.
    const std::wstring& GetCapturedOutputDeviceName() const { return outputDeviceName_; }

private:
    ID3D11Device* d3dDevice_ = nullptr;
    ID3D11DeviceContext* d3dContext_ = nullptr;
    IDXGIOutputDuplication* duplication_ = nullptr;
    IDXGIAdapter1* adapter_ = nullptr;
    IDXGIOutput1* output1_ = nullptr;
    DXGI_OUTDUPL_DESC duplDesc_{};

    std::wstring outputDeviceName_;

    bool frameHeld_ = false;

    // Diagnostics
    unsigned long long producedFramesTotal_ = 0;
    UINT lastAccumulatedFrames_ = 0;
    HRESULT lastAcquireHr_ = S_OK;
};
