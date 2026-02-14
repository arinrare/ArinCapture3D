// Renderer.h
#pragma once
#include <d3d11.h>
#include <windows.h>
#include <winrt/base.h>

class Renderer {
public:
    enum class StereoShaderMode {
        Depth3Pass = 0,
    };
    enum class OverlayPosition {
        TopLeft = 0,
        TopRight = 1,
        BottomLeft = 2,
        BottomRight = 3,
        Center = 4,
    };
    // Now takes width, height, and format
    bool Init(HWND hWnd, UINT width, UINT height, DXGI_FORMAT format, ID3D11Device* device, ID3D11DeviceContext* context);
    bool Resize(UINT width, UINT height);

    // Call when the window is moved between monitors. This can help avoid black output on some
    // multi-monitor/virtual-display setups by recreating swapchain buffers on the new output.
    bool RefreshSwapChainForCurrentWindow();
    void Render(ID3D11Texture2D* srcTex, float depth);
    void Cleanup();

    // Software cursor overlay (drawn into the output frame so screen-capture apps can see it).
    // Cursor position is normalized in [0,1] relative to a single eye view.
    void SetSoftwareCursorEnabled(bool enabled) { softwareCursorEnabled_ = enabled; }
    bool GetSoftwareCursorEnabled() const { return softwareCursorEnabled_; }
    void SetSoftwareCursorPosNormalized(float x01, float y01) { softwareCursorX01_ = x01; softwareCursorY01_ = y01; }

    // Menu overlay (used to mirror the system tray popup menu into the output frame).
    // Rect is normalized in [0,1] relative to the output view.
    void SetMenuOverlayEnabled(bool enabled) { menuOverlayEnabled_ = enabled; }
    bool GetMenuOverlayEnabled() const { return menuOverlayEnabled_; }
    void SetMenuOverlayRectNormalized(float left, float top, float right, float bottom);
    void UpdateMenuOverlayImageBGRA(const void* bgra, UINT width, UINT height);

    // Optional source crop (normalized UV rect in the *current source texture*).
    // When set, the renderer samples only this sub-rect of the source.
    // left/top/right/bottom are in [0,1], where (0,0) is top-left of the source.
    void SetSourceCropNormalized(float left, float top, float right, float bottom);
    void ClearSourceCrop();

    // Repeat frame diagnostics
    void ResetRepeatStats();
    int GetRepeatCount() const { return repeatCount_; }
    void UpdateRepeat(INT64 frameTimestamp);

    // Capture backend diagnostics (used by the HUD)
    void SetCaptureStatsDXGI(unsigned long long producedFramesTotal, UINT lastAccumulatedFrames);
    void SetCaptureStatsWGC(unsigned long long arrivedEventsTotal, unsigned long long producedFramesTotal, unsigned long long consumedFramesTotal);

    // Diagnostics overlay
    void SetDiagnosticsOverlay(bool enabled) { diagnosticsOverlay_ = enabled; }
    bool GetDiagnosticsOverlay() const { return diagnosticsOverlay_; }
    // Diagnostics overlay sizing: 0=Small, 1=Medium, 2=Large
    void SetDiagnosticsOverlaySizeIndex(int idx) { overlaySizeIndex_ = (idx < 0 ? 0 : (idx > 2 ? 2 : idx)); overlayDpi_ = 0; }
    int GetDiagnosticsOverlaySizeIndex() const { return overlaySizeIndex_; }
    // Diagnostics overlay content: compact reduces the multi-line dump into a small HUD.
    void SetDiagnosticsOverlayCompact(bool compact) { overlayCompact_ = compact; }
    bool GetDiagnosticsOverlayCompact() const { return overlayCompact_; }
    void SetOverlayPosition(OverlayPosition pos) { overlayPosition_ = pos; }
    OverlayPosition GetOverlayPosition() const { return overlayPosition_; }

    // Framerate control
    void SetFramerateIndex(int idx) { framerateIndex_ = idx; }
    int GetFramerateIndex() const { return framerateIndex_; }

    // Present control (diagnostic): toggles swapchain Present sync interval.
    void SetVSyncEnabled(bool enabled) { vsyncEnabled_ = enabled; }
    bool GetVSyncEnabled() const { return vsyncEnabled_; }

    // Render resolution (output-side downscale). 0 = native (no downscale).
    void SetRenderResolutionIndex(int idx);
    int GetRenderResolutionIndex() const { return renderResIndex_; }

    // Stereoscopy (Half-SBS). When enabled, draws two views side-by-side.
    void SetStereoEnabled(bool enabled) { stereoEnabled_ = enabled; }
    bool GetStereoEnabled() const { return stereoEnabled_; }
    void SetStereoDepthLevel(int level);
    int GetStereoDepthLevel() const { return stereoDepthLevel_; }

    // Stereo parallax strength. Range [0,100] percent.
    void SetStereoParallaxStrengthPercent(int percent) { stereoParallaxStrengthPercent_ = (percent < 0 ? 0 : (percent > 100 ? 100 : percent)); }
    int GetStereoParallaxStrengthPercent() const { return stereoParallaxStrengthPercent_; }

    // Stereo shader selection.
    void SetStereoShaderMode(StereoShaderMode mode) { stereoShaderMode_ = mode; }
    StereoShaderMode GetStereoShaderMode() const { return stereoShaderMode_; }

    // Returns frame interval in seconds for current framerate
    double GetFrameInterval() const {
        static const double intervals[] = { 1.0/60.0, 1.0/72.0, 1.0/90.0, 1.0/120.0, 0.0 };
        if (framerateIndex_ >= 0 && framerateIndex_ < 5) return intervals[framerateIndex_];
        return 0.0;
    }

private:
    void UpdateRateStats(bool gotNewFrame);
    void EnsureOverlayFont(UINT dpi);
    void EnsureDepthStereoResources(UINT outW, UINT outH);

    HWND hWnd_ = nullptr;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGISwapChain* swapChain_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    UINT swapChainFlags_ = 0;

    // Intended swapchain buffer size. Used to keep RefreshSwapChainForCurrentWindow
    // from implicitly resizing to the current window client size.
    UINT swapW_ = 0;
    UINT swapH_ = 0;

    ID3D11Texture2D* backbufferReadback_ = nullptr;
    int debugReadbackFrames_ = 0;

    // Shader-based blit path (enables scaling + future stereo shaders)
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* psStandard_ = nullptr;
    // Depth-based stereo compute pipeline
    ID3D11ComputeShader* csDepthRaw_ = nullptr;
    ID3D11ComputeShader* csDepthSmooth_ = nullptr;
    ID3D11ComputeShader* csParallaxSbs_ = nullptr;

    ID3D11Buffer* csParamsCb_ = nullptr;

    ID3D11Texture2D* depthRawTex_ = nullptr;
    ID3D11ShaderResourceView* depthRawSrv_ = nullptr;
    ID3D11UnorderedAccessView* depthRawUav_ = nullptr;

    ID3D11Texture2D* depthSmoothTex_ = nullptr;
    ID3D11ShaderResourceView* depthSmoothSrv_ = nullptr;
    ID3D11UnorderedAccessView* depthSmoothUav_ = nullptr;

    ID3D11Texture2D* depthPrevTex_[2] = { nullptr, nullptr };
    ID3D11ShaderResourceView* depthPrevSrv_[2] = { nullptr, nullptr };
    ID3D11UnorderedAccessView* depthPrevUav_[2] = { nullptr, nullptr };
    int depthPrevIndex_ = 0;
    float depthFrame_ = 0.0f;

    ID3D11Texture2D* stereoOutTex_ = nullptr;
    ID3D11ShaderResourceView* stereoOutSrv_ = nullptr;
    ID3D11UnorderedAccessView* stereoOutUav_ = nullptr;

    UINT depthOutW_ = 0;
    UINT depthOutH_ = 0;
    ID3D11InputLayout* inputLayout_ = nullptr;
    ID3D11Buffer* vertexBuffer_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;

    // Stereo constant buffer (PS b0)
    ID3D11Buffer* stereoCb_ = nullptr;

    // Source crop constant buffer (PS b1)
    ID3D11Buffer* cropCb_ = nullptr;
    bool cropEnabled_ = false;
    float cropLeft_ = 0.0f;
    float cropTop_ = 0.0f;
    float cropRight_ = 1.0f;
    float cropBottom_ = 1.0f;

    // Software cursor constant buffer (PS b2)
    ID3D11Buffer* cursorCb_ = nullptr;
    bool softwareCursorEnabled_ = false;
    float softwareCursorX01_ = 0.5f;
    float softwareCursorY01_ = 0.5f;

    // Menu overlay (PS t1 + b3)
    ID3D11Texture2D* menuTex_ = nullptr;
    ID3D11ShaderResourceView* menuSrv_ = nullptr;
    UINT menuW_ = 0;
    UINT menuH_ = 0;
    ID3D11Buffer* menuCb_ = nullptr;
    bool menuOverlayEnabled_ = false;
    float menuLeft01_ = 0.0f;
    float menuTop01_ = 0.0f;
    float menuRight01_ = 0.0f;
    float menuBottom01_ = 0.0f;

    ID3D11Texture2D* srcCopy_ = nullptr;
    ID3D11ShaderResourceView* srcSrv_ = nullptr;
    UINT srcW_ = 0;
    UINT srcH_ = 0;
    DXGI_FORMAT srcFmt_ = DXGI_FORMAT_UNKNOWN;

    // Optional downscale render target (two-pass: src -> downscale -> backbuffer)
    int renderResIndex_ = 0;
    UINT downW_ = 0;
    UINT downH_ = 0;
    bool downDirty_ = true;
    ID3D11Texture2D* downTex_ = nullptr;
    ID3D11RenderTargetView* downRtv_ = nullptr;
    ID3D11ShaderResourceView* downSrv_ = nullptr;


    int framerateIndex_ = 0; // 0=60, 1=72, 2=90, 3=120, 4=Unlimited

    // Diagnostics rates (tracked per presented frame, independent of overlay draw passes).
    LARGE_INTEGER rateQpf_ = {};
    LARGE_INTEGER rateLastQpc_ = {};
    int ratePresentCount_ = 0;
    int rateNewFrameCount_ = 0;
    double presentFps_ = 0.0;
    double newFrameFps_ = 0.0;

    unsigned long long rateLastDxgiProduced_ = 0;
    unsigned long long rateLastWgcArrived_ = 0;
    unsigned long long rateLastWgcProduced_ = 0;
    unsigned long long rateLastWgcConsumed_ = 0;
    double dxgiProducedFps_ = 0.0;
    double wgcArrivedFps_ = 0.0;
    double wgcProducedFps_ = 0.0;
    double wgcConsumedFps_ = 0.0;
    bool diagnosticsOverlay_ = false;
    int overlaySizeIndex_ = 0; // 0=Small, 1=Medium, 2=Large
    bool overlayCompact_ = true;
    OverlayPosition overlayPosition_ = OverlayPosition::TopLeft;

    bool stereoEnabled_ = false;
    int stereoDepthLevel_ = 12; // [1,20]
    int stereoParallaxStrengthPercent_ = 50; // [0,100]

    StereoShaderMode stereoShaderMode_ = StereoShaderMode::Depth3Pass;

    bool vsyncEnabled_ = true;

    HFONT overlayFont_ = nullptr;
    UINT overlayDpi_ = 0;

    // Repeat frame detection
    INT64 lastFrameTimestamp_ = 0;
    int repeatCount_ = 0;

    // Capture timestamp pacing (WGC only). This is derived from SystemRelativeTime deltas.
    double wgcCaptureDtEmaSec_ = 0.0;
    double wgcCaptureFpsEstimate_ = 0.0;

    // Capture stats (fed by capture backends via main.cpp)
    enum class CaptureBackendStats {
        None,
        DXGI,
        WGC,
    };
    CaptureBackendStats captureStatsBackend_ = CaptureBackendStats::None;

    // DXGI
    unsigned long long dxgiProducedTotal_ = 0;
    UINT dxgiLastAccumulated_ = 0;

    // WGC
    unsigned long long wgcArrivedTotal_ = 0;
    unsigned long long wgcProducedTotal_ = 0;
    unsigned long long wgcConsumedTotal_ = 0;
};
