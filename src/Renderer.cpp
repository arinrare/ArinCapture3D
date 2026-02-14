
#include "Renderer.h"
#include "Log.h"
#include <windows.h>
#include <dxgi1_2.h>
#include <winrt/base.h>
#include <d3dcompiler.h>
#include "3PassShader.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#ifndef AC_SRC_OVER
#define AC_SRC_OVER 0x00
#endif

static float Clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

void Renderer::SetMenuOverlayRectNormalized(float left, float top, float right, float bottom) {
    // Clamp + sanitize.
    left = Clamp01(left);
    top = Clamp01(top);
    right = Clamp01(right);
    bottom = Clamp01(bottom);
    if (right < left) std::swap(right, left);
    if (bottom < top) std::swap(bottom, top);
    menuLeft01_ = left;
    menuTop01_ = top;
    menuRight01_ = right;
    menuBottom01_ = bottom;
}

void Renderer::UpdateMenuOverlayImageBGRA(const void* bgra, UINT width, UINT height) {
    if (!device_ || !context_) return;
    if (!bgra || width == 0 || height == 0) {
        if (menuSrv_) { menuSrv_->Release(); menuSrv_ = nullptr; }
        if (menuTex_) { menuTex_->Release(); menuTex_ = nullptr; }
        menuW_ = menuH_ = 0;
        return;
    }

    if (!menuTex_ || !menuSrv_ || menuW_ != width || menuH_ != height) {
        if (menuSrv_) { menuSrv_->Release(); menuSrv_ = nullptr; }
        if (menuTex_) { menuTex_->Release(); menuTex_ = nullptr; }
        menuW_ = menuH_ = 0;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = device_->CreateTexture2D(&td, nullptr, &menuTex_);
        if (FAILED(hr) || !menuTex_) {
            Log::Error("Renderer: CreateTexture2D(menuTex) failed");
            return;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = td.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        hr = device_->CreateShaderResourceView(menuTex_, &sd, &menuSrv_);
        if (FAILED(hr) || !menuSrv_) {
            Log::Error("Renderer: CreateShaderResourceView(menuSrv) failed");
            if (menuTex_) { menuTex_->Release(); menuTex_ = nullptr; }
            return;
        }

        menuW_ = width;
        menuH_ = height;
    }

    context_->UpdateSubresource(menuTex_, 0, nullptr, bgra, width * 4, 0);
}

void Renderer::SetSourceCropNormalized(float left, float top, float right, float bottom) {
    // Sanitize and clamp.
    left = Clamp01(left);
    top = Clamp01(top);
    right = Clamp01(right);
    bottom = Clamp01(bottom);
    if (right < left) std::swap(left, right);
    if (bottom < top) std::swap(top, bottom);

    // Avoid degenerate rects.
    const float minSize = 1.0f / 4096.0f;
    if ((right - left) < minSize || (bottom - top) < minSize) {
        ClearSourceCrop();
        return;
    }

    cropEnabled_ = true;
    cropLeft_ = left;
    cropTop_ = top;
    cropRight_ = right;
    cropBottom_ = bottom;
}

void Renderer::ClearSourceCrop() {
    cropEnabled_ = false;
    cropLeft_ = 0.0f;
    cropTop_ = 0.0f;
    cropRight_ = 1.0f;
    cropBottom_ = 1.0f;
}

// Some toolchains/SDK setups don't ship msimg32.h by default.
// We runtime-load AlphaBlend from msimg32.dll and fall back to opaque fill if unavailable.
typedef struct _AC_BLENDFUNCTION {
    BYTE BlendOp;
    BYTE BlendFlags;
    BYTE SourceConstantAlpha;
    BYTE AlphaFormat;
} AC_BLENDFUNCTION;

static bool TryAlphaBlendRect(HDC dst, int x, int y, int w, int h, COLORREF color, BYTE alpha) {
    if (!dst || w <= 0 || h <= 0) return false;

    static HMODULE s_msimg32 = nullptr;
    static BOOL(WINAPI* s_alphaBlend)(HDC, int, int, int, int, HDC, int, int, int, int, AC_BLENDFUNCTION) = nullptr;
    static bool s_tried = false;
    if (!s_tried) {
        s_tried = true;
        s_msimg32 = LoadLibraryW(L"msimg32.dll");
        if (s_msimg32) {
            s_alphaBlend = (BOOL(WINAPI*)(HDC, int, int, int, int, HDC, int, int, int, int, AC_BLENDFUNCTION))GetProcAddress(s_msimg32, "AlphaBlend");
        }
    }
    if (!s_alphaBlend) return false;

    // Cache a 1x1 BGRA DIB section and scale it via AlphaBlend.
    // This avoids per-frame compatible bitmap/DC allocations (which are expensive).
    static HDC s_memDc = nullptr;
    static HBITMAP s_dib = nullptr;
    static void* s_bits = nullptr;
    if (!s_memDc) {
        s_memDc = CreateCompatibleDC(dst);
        if (!s_memDc) return false;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = 1;
        bmi.bmiHeader.biHeight = -1; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        s_dib = CreateDIBSection(s_memDc, &bmi, DIB_RGB_COLORS, &s_bits, nullptr, 0);
        if (!s_dib || !s_bits) {
            if (s_dib) { DeleteObject(s_dib); s_dib = nullptr; }
            DeleteDC(s_memDc);
            s_memDc = nullptr;
            return false;
        }
        SelectObject(s_memDc, s_dib);
    }

    // Write one BGRA pixel.
    const BYTE r = GetRValue(color);
    const BYTE g = GetGValue(color);
    const BYTE b = GetBValue(color);
    ((BYTE*)s_bits)[0] = b;
    ((BYTE*)s_bits)[1] = g;
    ((BYTE*)s_bits)[2] = r;
    ((BYTE*)s_bits)[3] = 0xFF;

    AC_BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = alpha;
    bf.AlphaFormat = 0;

    bool ok = (s_alphaBlend(dst, x, y, w, h, s_memDc, 0, 0, 1, 1, bf) != FALSE);
    return ok;
}

void Renderer::UpdateRepeat(INT64 frameTimestamp) {
    // WGC-only: estimate capture cadence from frame timestamps (SystemRelativeTime in 100ns units).
    // This provides an independent corroboration of the ev/prod/cons counters.
    if (captureStatsBackend_ == CaptureBackendStats::WGC && lastFrameTimestamp_ != 0 && frameTimestamp > lastFrameTimestamp_) {
        const double dtSec = (double)(frameTimestamp - lastFrameTimestamp_) * 1e-7;
        if (dtSec > 0.0 && dtSec < 1.0) {
            if (wgcCaptureDtEmaSec_ <= 0.0) {
                wgcCaptureDtEmaSec_ = dtSec;
            } else {
                // Light smoothing so the HUD is stable but still reacts.
                const double a = 0.10;
                wgcCaptureDtEmaSec_ = (1.0 - a) * wgcCaptureDtEmaSec_ + a * dtSec;
            }
            wgcCaptureFpsEstimate_ = (wgcCaptureDtEmaSec_ > 0.0) ? (1.0 / wgcCaptureDtEmaSec_) : 0.0;
        }
    }

    if (frameTimestamp == lastFrameTimestamp_) {
        repeatCount_++;
    } else {
        repeatCount_ = 0;
        lastFrameTimestamp_ = frameTimestamp;
    }
}
void Renderer::ResetRepeatStats() {
    lastFrameTimestamp_ = 0;
    repeatCount_ = 0;
    wgcCaptureDtEmaSec_ = 0.0;
    wgcCaptureFpsEstimate_ = 0.0;
}

void Renderer::SetCaptureStatsDXGI(unsigned long long producedFramesTotal, UINT lastAccumulatedFrames) {
    captureStatsBackend_ = CaptureBackendStats::DXGI;
    dxgiProducedTotal_ = producedFramesTotal;
    dxgiLastAccumulated_ = lastAccumulatedFrames;
}

void Renderer::SetCaptureStatsWGC(unsigned long long arrivedEventsTotal, unsigned long long producedFramesTotal, unsigned long long consumedFramesTotal) {
    captureStatsBackend_ = CaptureBackendStats::WGC;
    wgcArrivedTotal_ = arrivedEventsTotal;
    wgcProducedTotal_ = producedFramesTotal;
    wgcConsumedTotal_ = consumedFramesTotal;
}

void Renderer::UpdateRateStats(bool gotNewFrame) {
    if (rateQpf_.QuadPart == 0) {
        QueryPerformanceFrequency(&rateQpf_);
    }

    LARGE_INTEGER nowQpc{};
    QueryPerformanceCounter(&nowQpc);
    if (rateLastQpc_.QuadPart == 0) {
        rateLastQpc_ = nowQpc;
    }

    ratePresentCount_++;
    if (gotNewFrame) {
        rateNewFrameCount_++;
    }

    const double elapsed = double(nowQpc.QuadPart - rateLastQpc_.QuadPart) / double(rateQpf_.QuadPart);
    if (elapsed < 1.0) return;

    presentFps_ = (elapsed > 0.0) ? (ratePresentCount_ / elapsed) : 0.0;
    newFrameFps_ = (elapsed > 0.0) ? (rateNewFrameCount_ / elapsed) : 0.0;

    // Update capture delivery rates using monotonically increasing totals.
    if (captureStatsBackend_ == CaptureBackendStats::DXGI) {
        if (dxgiProducedTotal_ < rateLastDxgiProduced_) rateLastDxgiProduced_ = dxgiProducedTotal_;
        dxgiProducedFps_ = (elapsed > 0.0) ? (double)(dxgiProducedTotal_ - rateLastDxgiProduced_) / elapsed : 0.0;
        rateLastDxgiProduced_ = dxgiProducedTotal_;
    } else if (captureStatsBackend_ == CaptureBackendStats::WGC) {
        if (wgcArrivedTotal_ < rateLastWgcArrived_) rateLastWgcArrived_ = wgcArrivedTotal_;
        if (wgcProducedTotal_ < rateLastWgcProduced_) rateLastWgcProduced_ = wgcProducedTotal_;
        if (wgcConsumedTotal_ < rateLastWgcConsumed_) rateLastWgcConsumed_ = wgcConsumedTotal_;
        wgcArrivedFps_ = (elapsed > 0.0) ? (double)(wgcArrivedTotal_ - rateLastWgcArrived_) / elapsed : 0.0;
        wgcProducedFps_ = (elapsed > 0.0) ? (double)(wgcProducedTotal_ - rateLastWgcProduced_) / elapsed : 0.0;
        wgcConsumedFps_ = (elapsed > 0.0) ? (double)(wgcConsumedTotal_ - rateLastWgcConsumed_) / elapsed : 0.0;
        rateLastWgcArrived_ = wgcArrivedTotal_;
        rateLastWgcProduced_ = wgcProducedTotal_;
        rateLastWgcConsumed_ = wgcConsumedTotal_;
    }

    ratePresentCount_ = 0;
    rateNewFrameCount_ = 0;
    rateLastQpc_ = nowQpc;
}

void Renderer::EnsureOverlayFont(UINT dpi) {
    if (dpi == 0) dpi = 96;
    if (overlayFont_ && overlayDpi_ == dpi) return;

    if (overlayFont_) {
        DeleteObject(overlayFont_);
        overlayFont_ = nullptr;
    }

    // Diagnostics overlay sizing (0=Small, 1=Medium, 2=Large).
    // Keep point sizes deliberately small; on high-DPI screens a "normal" UI font becomes massive.
    int pointSize = 9; // Medium
    if (overlaySizeIndex_ == 0) pointSize = 7;
    else if (overlaySizeIndex_ == 2) pointSize = 11;
    const int heightPx = -MulDiv(pointSize, (int)dpi, 72);
    overlayFont_ = CreateFontW(
        heightPx,
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI"
    );
    overlayDpi_ = dpi;
}

void Renderer::EnsureDepthStereoResources(UINT outW, UINT outH) {
    if (!device_) return;
    if (outW == 0 || outH == 0) return;

    const bool okExisting = (
        depthRawTex_ && depthRawSrv_ && depthRawUav_ &&
        depthSmoothTex_ && depthSmoothSrv_ && depthSmoothUav_ &&
        depthPrevTex_[0] && depthPrevSrv_[0] && depthPrevUav_[0] &&
        depthPrevTex_[1] && depthPrevSrv_[1] && depthPrevUav_[1] &&
        stereoOutTex_ && stereoOutSrv_ && stereoOutUav_ &&
        depthOutW_ == outW && depthOutH_ == outH
    );
    if (okExisting) return;

    if (depthRawSrv_) { depthRawSrv_->Release(); depthRawSrv_ = nullptr; }
    if (depthRawUav_) { depthRawUav_->Release(); depthRawUav_ = nullptr; }
    if (depthRawTex_) { depthRawTex_->Release(); depthRawTex_ = nullptr; }

    if (depthSmoothSrv_) { depthSmoothSrv_->Release(); depthSmoothSrv_ = nullptr; }
    if (depthSmoothUav_) { depthSmoothUav_->Release(); depthSmoothUav_ = nullptr; }
    if (depthSmoothTex_) { depthSmoothTex_->Release(); depthSmoothTex_ = nullptr; }

    for (int i = 0; i < 2; ++i) {
        if (depthPrevSrv_[i]) { depthPrevSrv_[i]->Release(); depthPrevSrv_[i] = nullptr; }
        if (depthPrevUav_[i]) { depthPrevUav_[i]->Release(); depthPrevUav_[i] = nullptr; }
        if (depthPrevTex_[i]) { depthPrevTex_[i]->Release(); depthPrevTex_[i] = nullptr; }
    }
    depthPrevIndex_ = 0;
    depthFrame_ = 0.0f;

    if (stereoOutSrv_) { stereoOutSrv_->Release(); stereoOutSrv_ = nullptr; }
    if (stereoOutUav_) { stereoOutUav_->Release(); stereoOutUav_ = nullptr; }
    if (stereoOutTex_) { stereoOutTex_->Release(); stereoOutTex_ = nullptr; }
    depthOutW_ = depthOutH_ = 0;

    auto createDepthTex = [&](const char* name, ID3D11Texture2D** outTex, ID3D11ShaderResourceView** outSrv, ID3D11UnorderedAccessView** outUav) -> bool {
        if (!outTex || !outSrv || !outUav) return false;
        *outTex = nullptr;
        *outSrv = nullptr;
        *outUav = nullptr;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = outW;
        td.Height = outH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

        HRESULT hr = device_->CreateTexture2D(&td, nullptr, outTex);
        if (FAILED(hr) || !*outTex) {
            Log::Error(std::string("EnsureDepthStereoResources: CreateTexture2D(") + name + ") failed");
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = td.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        hr = device_->CreateShaderResourceView(*outTex, &sd, outSrv);
        if (FAILED(hr) || !*outSrv) {
            Log::Error(std::string("EnsureDepthStereoResources: CreateShaderResourceView(") + name + ") failed");
            return false;
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = td.Format;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        ud.Texture2D.MipSlice = 0;
        hr = device_->CreateUnorderedAccessView(*outTex, &ud, outUav);
        if (FAILED(hr) || !*outUav) {
            Log::Error(std::string("EnsureDepthStereoResources: CreateUnorderedAccessView(") + name + ") failed");
            return false;
        }
        return true;
    };

    if (!createDepthTex("depthRaw", &depthRawTex_, &depthRawSrv_, &depthRawUav_)) return;
    if (!createDepthTex("depthSmooth", &depthSmoothTex_, &depthSmoothSrv_, &depthSmoothUav_)) return;
    if (!createDepthTex("depthPrev0", &depthPrevTex_[0], &depthPrevSrv_[0], &depthPrevUav_[0])) return;
    if (!createDepthTex("depthPrev1", &depthPrevTex_[1], &depthPrevSrv_[1], &depthPrevUav_[1])) return;

    // Output SBS image (RGBA8) with UAV+SRV.
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = outW;
        td.Height = outH;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        HRESULT hr = device_->CreateTexture2D(&td, nullptr, &stereoOutTex_);
        if (FAILED(hr) || !stereoOutTex_) {
            Log::Error("EnsureDepthStereoResources: CreateTexture2D(stereoOut) failed");
            return;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = td.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sd.Texture2D.MipLevels = 1;
        hr = device_->CreateShaderResourceView(stereoOutTex_, &sd, &stereoOutSrv_);
        if (FAILED(hr) || !stereoOutSrv_) {
            Log::Error("EnsureDepthStereoResources: CreateShaderResourceView(stereoOut) failed");
            return;
        }

        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = td.Format;
        ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
        ud.Texture2D.MipSlice = 0;
        hr = device_->CreateUnorderedAccessView(stereoOutTex_, &ud, &stereoOutUav_);
        if (FAILED(hr) || !stereoOutUav_) {
            Log::Error("EnsureDepthStereoResources: CreateUnorderedAccessView(stereoOut) failed");
            return;
        }
    }

    depthOutW_ = outW;
    depthOutH_ = outH;

    // Initialize history to neutral.
    if (context_) {
        const float clearVal[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
        if (depthPrevUav_[0]) context_->ClearUnorderedAccessViewFloat(depthPrevUav_[0], clearVal);
        if (depthPrevUav_[1]) context_->ClearUnorderedAccessViewFloat(depthPrevUav_[1], clearVal);
    }
}

void Renderer::SetRenderResolutionIndex(int idx) {
    if (idx < 0) idx = 0;
    if (renderResIndex_ == idx) return;
    renderResIndex_ = idx;
    downDirty_ = true;
    // Drop the existing downscale resources; they will be recreated on next Render.
    if (downSrv_) { downSrv_->Release(); downSrv_ = nullptr; }
    if (downRtv_) { downRtv_->Release(); downRtv_ = nullptr; }
    if (downTex_) { downTex_->Release(); downTex_ = nullptr; }
    downW_ = downH_ = 0;
}

void Renderer::SetStereoDepthLevel(int level) {
    // Allow 0 as a true neutral depth setting.
    if (level < 0) level = 0;
    if (level > 20) level = 20;
    stereoDepthLevel_ = level;
}

#pragma comment(lib, "d3dcompiler.lib")

static std::string WideToUtf8(const wchar_t* wstr) {
    if (!wstr) return std::string();
    int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return std::string();
    std::string out;
    out.resize(static_cast<size_t>(needed - 1));
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

static void LogDeviceAdapter(ID3D11Device* device, const char* prefix) {
    if (!device) return;
    IDXGIDevice* dxgiDevice = nullptr;
    HRESULT hr = device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr) || !dxgiDevice) return;
    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr) || !adapter) return;

    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(adapter->GetDesc(&desc))) {
        Log::Info(std::string(prefix) + " adapter: " + WideToUtf8(desc.Description) +
                  " luid=" + std::to_string((unsigned int)desc.AdapterLuid.HighPart) + ":" + std::to_string((unsigned int)desc.AdapterLuid.LowPart));
    }
    adapter->Release();
}

static void LogSwapChainContainingOutput(IDXGISwapChain* swapChain, const char* prefix) {
    if (!swapChain) return;
    IDXGIOutput* output = nullptr;
    HRESULT hr = swapChain->GetContainingOutput(&output);
    if (FAILED(hr) || !output) {
        Log::Error(std::string(prefix) + " GetContainingOutput failed");
        return;
    }

    DXGI_OUTPUT_DESC outDesc{};
    if (SUCCEEDED(output->GetDesc(&outDesc))) {
        Log::Info(std::string(prefix) + " output: " + WideToUtf8(outDesc.DeviceName));
    }

    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(output->GetParent(__uuidof(IDXGIAdapter), (void**)&adapter)) && adapter) {
        DXGI_ADAPTER_DESC ad{};
        if (SUCCEEDED(adapter->GetDesc(&ad))) {
            Log::Info(std::string(prefix) + " output adapter: " + WideToUtf8(ad.Description) +
                      " luid=" + std::to_string((unsigned int)ad.AdapterLuid.HighPart) + ":" + std::to_string((unsigned int)ad.AdapterLuid.LowPart));
        }
        adapter->Release();
    }

    output->Release();
}

static bool CompileShader(const char* hlsl, const char* entry, const char* target, ID3DBlob** outBlob) {
    if (!outBlob) return false;
    *outBlob = nullptr;
    // IMPORTANT: Keep shader compilation flags identical across Debug/Release
    // so shader behavior matches between configurations.
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    ID3DBlob* shader = nullptr;
    ID3DBlob* errors = nullptr;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entry, target, flags, 0, &shader, &errors);
    if (FAILED(hr) || !shader) {
        if (errors) {
            std::string err((const char*)errors->GetBufferPointer(), errors->GetBufferSize());
            Log::Error("D3DCompile failed: " + err);
            errors->Release();
        } else {
            Log::Error("D3DCompile failed");
        }
        if (shader) shader->Release();
        return false;
    }
    if (errors) errors->Release();
    *outBlob = shader;
    return true;
}

// Single HLSL string for the fullscreen blit pass (scaling + stereo shift + cursor/menu overlays).
// Keeping this local lets us remove the old Shader.* module.
static const char* kBlitHlsl = R"HLSL(
struct VSIn {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(VSIn i) {
    VSOut o;
    o.pos = float4(i.pos, 0.0, 1.0);
    o.uv  = i.uv;
    return o;
}

Texture2D srcTex : register(t0);
Texture2D menuTex : register(t1);
SamplerState samp0 : register(s0);

cbuffer StereoCB : register(b0) {
    float uOffset;
    float eyeSign;
    float warpStrength;
    float popStrength;

    // Parallax strength multiplier.
    // 1.0 = neutral, 2.0 = double baseline shift + pop layering.
    float parallaxPercent;
    // Macro depth multiplier.
    // 1.0 = neutral, 2.0 = boost baseline shift on large-scale structure.
    float macroDepthPercent;
    float pad1;
    float pad2;
};

cbuffer CropCB : register(b1) {
    float2 cropOffset;
    float2 cropScale;
};

cbuffer CursorCB : register(b2) {
    // Cursor position normalized in [0,1] relative to a single-eye view.
    float cursorX01;
    float cursorY01;
    float cursorSizePx;
    float cursorEnabled;

    // If 1, fold output U (frac(u*2)) so cursor appears in both halves when presenting a pre-SBS texture.
    float cursorFoldU;
    float cursorPad1;
    float cursorPad2;
    float cursorPad3;
};

cbuffer MenuCB : register(b3) {
    // Normalized destination rect in output UV: (l, t, r, b)
    float4 menuRect;
    float menuEnabled;
    // If 1, fold output U (frac(u*2)) so overlay appears in both halves when presenting a pre-SBS texture.
    float menuFoldU;
    float menuPad2;
    float menuPad3;
};

float4 ApplySoftwareCursor(float4 baseColor, float2 uv) {
    if (cursorEnabled < 0.5) return baseColor;

    // Optional fold for full-screen SBS textures.
    if (cursorFoldU > 0.5) {
        uv.x = frac(uv.x * 2.0);
    }

    float2 c = float2(saturate(cursorX01), saturate(cursorY01));
    float2 d = abs(uv - c);

    // Approximate UV-per-pixel using derivatives.
    float2 uvPerPx = float2(abs(ddx(uv.x)), abs(ddy(uv.y)));
    uvPerPx = max(uvPerPx, float2(1e-6, 1e-6));

    float lenPx = max(6.0, cursorSizePx);
    float halfLenX = uvPerPx.x * lenPx;
    float halfLenY = uvPerPx.y * lenPx;

    // Outline + core thickness in UV.
    float thickCoreX = uvPerPx.x * 2.0;
    float thickCoreY = uvPerPx.y * 2.0;
    float thickOutX  = uvPerPx.x * 4.0;
    float thickOutY  = uvPerPx.y * 4.0;

    float horizOut = (d.y <= thickOutY && d.x <= halfLenX) ? 1.0 : 0.0;
    float vertOut  = (d.x <= thickOutX && d.y <= halfLenY) ? 1.0 : 0.0;
    float aOut = max(horizOut, vertOut);

    float horizIn = (d.y <= thickCoreY && d.x <= halfLenX) ? 1.0 : 0.0;
    float vertIn  = (d.x <= thickCoreX && d.y <= halfLenY) ? 1.0 : 0.0;
    float aIn = max(horizIn, vertIn);

    float4 col = baseColor;
    col = lerp(col, float4(0.0, 0.0, 0.0, 1.0), aOut);
    col = lerp(col, float4(1.0, 1.0, 1.0, 1.0), aIn);
    return col;
}

float4 ApplyMenuOverlay(float4 baseColor, float2 uv) {
    if (menuEnabled < 0.5) return baseColor;
    if (menuFoldU > 0.5) {
        uv.x = frac(uv.x * 2.0);
    }

    // Outside rect => no overlay.
    if (uv.x < menuRect.x || uv.x > menuRect.z || uv.y < menuRect.y || uv.y > menuRect.w) {
        return baseColor;
    }

    float2 rectMin = menuRect.xy;
    float2 rectMax = menuRect.zw;
    float2 rectSize = max(float2(1e-6, 1e-6), rectMax - rectMin);
    float2 tuv = (uv - rectMin) / rectSize;

    float4 m = menuTex.Sample(samp0, tuv);
    float a = saturate(m.a);
    // If capture source doesn't provide alpha, treat it as opaque.
    if (a < 1e-4) a = 1.0;
    return lerp(baseColor, m, a);
}

float Luma(float3 c) {
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

float EdgeMetricRadius(Texture2D tex, SamplerState samp, float2 uv, float radius) {
    uint w, h;
    tex.GetDimensions(w, h);
    float2 texel = 1.0 / float2(max(1u, w), max(1u, h));

    float2 dx = float2(texel.x, 0) * radius;
    float2 dy = float2(0, texel.y) * radius;

    float l0 = Luma(tex.Sample(samp, uv).rgb);
    float l1 = Luma(tex.Sample(samp, uv + dx).rgb);
    float l2 = Luma(tex.Sample(samp, uv - dx).rgb);
    float l3 = Luma(tex.Sample(samp, uv + dy).rgb);
    float l4 = Luma(tex.Sample(samp, uv - dy).rgb);

    // Simple gradient magnitude proxy (bigger means more edges/text).
    return abs(l0 - l1) + abs(l0 - l2) + abs(l0 - l3) + abs(l0 - l4);
}

float WarpWeight(float x, float warpStrength) {
    float x2 = x * x;
    float centerWeight = saturate(1.0 - x2);
    float edgeWeight = saturate(x2);
    return (warpStrength >= 0.0) ? centerWeight : edgeWeight;
}

bool ShrinkAndShiftOutputU(float u0, float uMin, float uMax, float margin, float delta, out float uOut) {
    // Pixel-perfect downscale + translation in *output space*:
    // - We leave black bars of size=margin (in the cropped UV range)
    // - The content shifts by the same amount as the sampling delta would have shifted it
    //   (note: sampling shift appears as an opposite-direction output shift).
    //
    // Using delta (sampling-space) directly:
    //   output shift = -delta
    // So the visible content interval in output UV becomes:
    //   [uMin + margin - delta, uMax - margin - delta]
    // and that interval maps back to the full [uMin, uMax] sampling range.
    float range = max(1e-6, uMax - uMin);
    margin = clamp(margin, 0.0, 0.49 * range);

    float innerMin = uMin + margin - delta;
    float innerMax = uMax - margin - delta;
    float innerRange = max(1e-6, innerMax - innerMin);

    if (u0 < innerMin || u0 > innerMax) {
        uOut = uMin;
        return false;
    }

    float t = (u0 - innerMin) / innerRange; // 0..1 within visible region
    uOut = uMin + t * range;                // remap to full source range
    return true;
}

float4 PSMain(VSOut i) : SV_Target {
    float2 uv0 = cropOffset + i.uv * cropScale;

    // Warp slider:
    //   warpStrength > 0 => center-weighted warp
    //   warpStrength < 0 => edge-weighted warp
    //   warpStrength = 0 => legacy behavior (uniform shift)
    float x = uv0.x * 2.0 - 1.0;
    float w = WarpWeight(x, warpStrength);
    float shiftMul = 1.0 + abs(warpStrength) * w;

    float2 uv = uv0;
    float parallaxStrength = clamp(parallaxPercent, 0.0, 2.0);
    float baseDelta = eyeSign * (uOffset * parallaxStrength * shiftMul);
    float uMin = cropOffset.x;
    float uMax = cropOffset.x + cropScale.x;
    float u;
    if (!ShrinkAndShiftOutputU(uv0.x, uMin, uMax, abs(baseDelta), baseDelta, u)) {
        return float4(0, 0, 0, 1);
    }
    uv.x = clamp(u, uMin, uMax);
    float4 c = srcTex.Sample(samp0, uv);
    c = ApplySoftwareCursor(c, i.uv);
    return ApplyMenuOverlay(c, i.uv);
}
)HLSL";

static bool CompileBlitVS(ID3DBlob** outVsBlob) {
    return CompileShader(kBlitHlsl, "VSMain", "vs_4_0", outVsBlob);
}

static bool CompileBlitPS(const char* entry, ID3DBlob** outPsBlob) {
    if (!entry || !*entry) return false;
    return CompileShader(kBlitHlsl, entry, "ps_4_0", outPsBlob);
}

static void UnbindPSResource(ID3D11DeviceContext* ctx, UINT slot) {
    if (!ctx) return;
    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    ctx->PSSetShaderResources(slot, 1, nullSrv);
}

static void UnbindCSResource(ID3D11DeviceContext* ctx, UINT slot) {
    if (!ctx) return;
    ID3D11ShaderResourceView* nullSrv[1] = { nullptr };
    ctx->CSSetShaderResources(slot, 1, nullSrv);
}

static void UnbindCSUav(ID3D11DeviceContext* ctx, UINT slot) {
    if (!ctx) return;
    ID3D11UnorderedAccessView* nullUav[1] = { nullptr };
    UINT initialCounts[1] = { 0 };
    ctx->CSSetUnorderedAccessViews(slot, 1, nullUav, initialCounts);
}

static UINT DivRoundUp(UINT a, UINT b) {
    return (a + b - 1) / b;
}

bool Renderer::Init(HWND hWnd, UINT width, UINT height, DXGI_FORMAT format, ID3D11Device* device, ID3D11DeviceContext* context) {

    Log::Info("Renderer::Init called");
    // Release previous resources
    Cleanup();

    hWnd_ = hWnd;

    if (!device || !context) {
        Log::Error("Renderer::Init: device or context is null");
        return false;
    }

    device_ = device;
    context_ = context;
    device_->AddRef();
    context_->AddRef();

    IDXGIDevice* dxgiDevice = nullptr;
    HRESULT hr = device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr) || !dxgiDevice) {
        Log::Error("Renderer::Init: QueryInterface(IDXGIDevice) failed");
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr) || !adapter) {
        Log::Error("Renderer::Init: IDXGIDevice::GetAdapter failed");
        return false;
    }

    IDXGIFactory* factory = nullptr;
    hr = adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory);
    adapter->Release();
    if (FAILED(hr) || !factory) {
        Log::Error("Renderer::Init: IDXGIAdapter::GetParent(IDXGIFactory) failed");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = format;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hWnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    // Enables drawing the diagnostics overlay into the swapchain backbuffer via IDXGISurface1::GetDC.
    swapChainFlags_ = DXGI_SWAP_CHAIN_FLAG_GDI_COMPATIBLE;
    scd.Flags = swapChainFlags_;

    Log::Info("Renderer::Init: Cleanup");
    hr = factory->CreateSwapChain(device_, &scd, &swapChain_);
    factory->Release();
    if (FAILED(hr) || !swapChain_) {
        Log::Error("Renderer::Init: IDXGIFactory::CreateSwapChain failed");
        return false;
    }

    // IMPORTANT: In practice, DXGI can size a windowed swapchain to the current HWND client size
    // even when BufferDesc.Width/Height are provided (especially under DPI virtualization / timing).
    // Force buffers to the requested dimensions so Debug/Release behave identically.
    {
        HRESULT rhr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swapChainFlags_);
        if (FAILED(rhr)) {
            Log::Error("Renderer::Init: ResizeBuffers(requested size) failed");
            return false;
        }
    }

    // Log which output this swap chain is actually presenting to (helps diagnose black output).
    IDXGIOutput* containingOutput = nullptr;
    hr = swapChain_->GetContainingOutput(&containingOutput);
    if (SUCCEEDED(hr) && containingOutput) {
        DXGI_OUTPUT_DESC outDesc = {};
        if (SUCCEEDED(containingOutput->GetDesc(&outDesc))) {
            Log::Info("Renderer::Init: SwapChain containing output: " + WideToUtf8(outDesc.DeviceName));
        }
        containingOutput->Release();
    } else {
        Log::Error("Renderer::Init: GetContainingOutput failed (window may not be on a display yet?)");
    }

    ID3D11Texture2D* backBuffer = nullptr;
    Log::Info("Renderer::Init: Setup swap chain desc");
    hr = swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr)) { Log::Error("swapChain_->GetBuffer failed"); return false; }

    // Log the actual backbuffer size we ended up with.
    {
        D3D11_TEXTURE2D_DESC bd{};
        backBuffer->GetDesc(&bd);
        Log::Info("Renderer::Init: Backbuffer actual " + std::to_string((int)bd.Width) + "x" + std::to_string((int)bd.Height) +
                  " (requested " + std::to_string((int)width) + "x" + std::to_string((int)height) + ")");
    }
    Log::Info("Renderer::Init: CreateRenderTargetView");
    hr = device_->CreateRenderTargetView(backBuffer, nullptr, &rtv_);
    backBuffer->Release();
    if (FAILED(hr)) { Log::Error("CreateRenderTargetView failed"); return false; }

    // Remember intended swapchain size so refresh operations don't implicitly resize to the window.
    swapW_ = width;
    swapH_ = height;

    // Build shader-based blit pipeline.
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psStandardBlob = nullptr;

    if (!CompileBlitVS(&vsBlob) ||
        !CompileBlitPS("PSMain", &psStandardBlob)) {
        if (vsBlob) vsBlob->Release();
        if (psStandardBlob) psStandardBlob->Release();
        Log::Error("Renderer::Init: shader compilation failed");
        return false;
    }

    hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_);
    if (FAILED(hr) || !vs_) {
        vsBlob->Release();
        psStandardBlob->Release();
        Log::Error("Renderer::Init: CreateVertexShader failed");
        return false;
    }

    hr = device_->CreatePixelShader(psStandardBlob->GetBufferPointer(), psStandardBlob->GetBufferSize(), nullptr, &psStandard_);
    if (FAILED(hr) || !psStandard_) {
        vsBlob->Release();
        psStandardBlob->Release();
        Log::Error("Renderer::Init: CreatePixelShader(standard) failed");
        return false;
    }

    // Optional depth-based stereo compute shaders (3-pass).
    {
        ID3DBlob* csBlob = nullptr;

        csBlob = nullptr;
        if (ThreePassShader::CompileDepthRawCS(&csBlob) && csBlob) {
            hr = device_->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &csDepthRaw_);
            csBlob->Release();
        }   

        csBlob = nullptr;
        if (ThreePassShader::CompileDepthSmoothCS(&csBlob) && csBlob) {
            hr = device_->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &csDepthSmooth_);
            csBlob->Release();
        }   

        csBlob = nullptr;
        if (ThreePassShader::CompileParallaxSbsCS(&csBlob) && csBlob) {
            hr = device_->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &csParallaxSbs_);
            csBlob->Release();
        }
        if (!csDepthRaw_ || !csDepthSmooth_ || !csParallaxSbs_) {
            Log::Info("Renderer::Init: Depth stereo compute shaders not available.");
        }
    }

    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    hr = device_->CreateInputLayout(il, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout_);
    vsBlob->Release();
    psStandardBlob->Release();
    if (FAILED(hr) || !inputLayout_) {
        Log::Error("Renderer::Init: CreateInputLayout failed");
        return false;
    }

    struct V { float px, py, u, v; };
    // Fullscreen triangle (covers entire screen with 3 verts)
    V verts[3] = {
        { -1.0f, -1.0f, 0.0f, 1.0f },
        { -1.0f,  3.0f, 0.0f, -1.0f },
        {  3.0f, -1.0f, 2.0f, 1.0f },
    };
    D3D11_BUFFER_DESC vb = {};
    vb.ByteWidth = sizeof(verts);
    vb.Usage = D3D11_USAGE_IMMUTABLE;
    vb.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbInit = {};
    vbInit.pSysMem = verts;
    hr = device_->CreateBuffer(&vb, &vbInit, &vertexBuffer_);
    if (FAILED(hr) || !vertexBuffer_) {
        Log::Error("Renderer::Init: CreateBuffer(vertex) failed");
        return false;
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    // When shifting UVs for stereo depth, sampling can go out-of-bounds.
    // CLAMP avoids black bars/seams (especially at the stereo split) at the cost of edge smear.
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.BorderColor[0] = 0.0f;
    sd.BorderColor[1] = 0.0f;
    sd.BorderColor[2] = 0.0f;
    sd.BorderColor[3] = 1.0f;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&sd, &sampler_);
    if (FAILED(hr) || !sampler_) {
        Log::Error("Renderer::Init: CreateSamplerState failed");
        return false;
    }

    // CS params constant buffer (dynamic, optional).
    {
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = 48; // must be multiple of 16
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT cshr = device_->CreateBuffer(&cbd, nullptr, &csParamsCb_);
        if (FAILED(cshr) || !csParamsCb_) {
            Log::Error("Renderer::Init: CreateBuffer(csParamsCb) failed");
        }
    }

    srcW_ = srcH_ = 0;
    srcFmt_ = DXGI_FORMAT_UNKNOWN;

    // Stereo constant buffer (dynamic).
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 32; // must be multiple of 16
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = device_->CreateBuffer(&cbd, nullptr, &stereoCb_);
    if (FAILED(hr) || !stereoCb_) {
        Log::Error("Renderer::Init: CreateBuffer(stereoCb) failed");
        return false;
    }

    // Crop constant buffer (dynamic).
    cbd.ByteWidth = 16; // 2 x float2
    hr = device_->CreateBuffer(&cbd, nullptr, &cropCb_);
    if (FAILED(hr) || !cropCb_) {
        Log::Error("Renderer::Init: CreateBuffer(cropCb) failed");
        return false;
    }

    // Software cursor constant buffer (dynamic).
    // Layout: float4(x01, y01, sizePx, enabled) + float4(foldU, pad, pad, pad)
    cbd.ByteWidth = 32; // must be multiple of 16
    hr = device_->CreateBuffer(&cbd, nullptr, &cursorCb_);
    if (FAILED(hr) || !cursorCb_) {
        Log::Error("Renderer::Init: CreateBuffer(cursorCb) failed");
        return false;
    }

    // Menu overlay constant buffer (dynamic).
    // Layout: float4(l,t,r,b) + float4(enabled, foldU, pad, pad)
    cbd.ByteWidth = 32;
    hr = device_->CreateBuffer(&cbd, nullptr, &menuCb_);
    if (FAILED(hr) || !menuCb_) {
        Log::Error("Renderer::Init: CreateBuffer(menuCb) failed");
        return false;
    }

    // Default crop is identity.
    ClearSourceCrop();

    // Create a staging texture for optional backbuffer readback diagnostics.
    D3D11_TEXTURE2D_DESC rb = {};
    rb.Width = width;
    rb.Height = height;
    rb.MipLevels = 1;
    rb.ArraySize = 1;
    rb.Format = format;
    rb.SampleDesc.Count = 1;
    rb.SampleDesc.Quality = 0;
    rb.Usage = D3D11_USAGE_STAGING;
    rb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    rb.BindFlags = 0;
    rb.MiscFlags = 0;
    hr = device_->CreateTexture2D(&rb, nullptr, &backbufferReadback_);
    if (FAILED(hr) || !backbufferReadback_) {
        Log::Error("Renderer::Init: failed to create backbuffer readback staging texture");
    }
    debugReadbackFrames_ = 0;

    Log::Info("Renderer initialized successfully.");
    return true;
}

bool Renderer::Resize(UINT width, UINT height) {
    if (!swapChain_ || !device_) return false;
    if (width == 0 || height == 0) return false;

    swapW_ = width;
    swapH_ = height;

    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    if (backbufferReadback_) { backbufferReadback_->Release(); backbufferReadback_ = nullptr; }

    HRESULT hr = swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, swapChainFlags_);
    if (FAILED(hr)) {
        Log::Error("Renderer::Resize: ResizeBuffers failed");
        return false;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    hr = swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr) || !backBuffer) {
        Log::Error("Renderer::Resize: GetBuffer failed");
        return false;
    }
    hr = device_->CreateRenderTargetView(backBuffer, nullptr, &rtv_);
    if (FAILED(hr) || !rtv_) {
        backBuffer->Release();
        Log::Error("Renderer::Resize: CreateRenderTargetView failed");
        return false;
    }

    D3D11_TEXTURE2D_DESC backDesc;
    backBuffer->GetDesc(&backDesc);

    D3D11_TEXTURE2D_DESC rb = {};
    rb.Width = backDesc.Width;
    rb.Height = backDesc.Height;
    rb.MipLevels = 1;
    rb.ArraySize = 1;
    rb.Format = backDesc.Format;
    rb.SampleDesc.Count = 1;
    rb.Usage = D3D11_USAGE_STAGING;
    rb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device_->CreateTexture2D(&rb, nullptr, &backbufferReadback_);
    backBuffer->Release();

    debugReadbackFrames_ = 0;
    return true;
}

bool Renderer::RefreshSwapChainForCurrentWindow() {
    if (!swapChain_ || !device_) return false;

    Log::Info("Renderer::RefreshSwapChainForCurrentWindow: begin");
    LogDeviceAdapter(device_, "Renderer::RefreshSwapChainForCurrentWindow");
    LogSwapChainContainingOutput(swapChain_, "Renderer::RefreshSwapChainForCurrentWindow: before");

    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    if (backbufferReadback_) { backbufferReadback_->Release(); backbufferReadback_ = nullptr; }

    // Preserve the intended backbuffer size.
    // Using 0,0 would resize to the window client size, which can differ across runs/configs.
    const UINT w = swapW_;
    const UINT h = swapH_;
    HRESULT hr = swapChain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swapChainFlags_);
    if (FAILED(hr)) {
        Log::Error("Renderer::RefreshSwapChainForCurrentWindow: ResizeBuffers failed");
        return false;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    hr = swapChain_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
    if (FAILED(hr) || !backBuffer) {
        Log::Error("Renderer::RefreshSwapChainForCurrentWindow: GetBuffer failed");
        return false;
    }

    hr = device_->CreateRenderTargetView(backBuffer, nullptr, &rtv_);
    if (FAILED(hr) || !rtv_) {
        backBuffer->Release();
        Log::Error("Renderer::RefreshSwapChainForCurrentWindow: CreateRenderTargetView failed");
        return false;
    }

    D3D11_TEXTURE2D_DESC backDesc{};
    backBuffer->GetDesc(&backDesc);

    D3D11_TEXTURE2D_DESC rb = {};
    rb.Width = backDesc.Width;
    rb.Height = backDesc.Height;
    rb.MipLevels = 1;
    rb.ArraySize = 1;
    rb.Format = backDesc.Format;
    rb.SampleDesc.Count = 1;
    rb.Usage = D3D11_USAGE_STAGING;
    rb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = device_->CreateTexture2D(&rb, nullptr, &backbufferReadback_);
    backBuffer->Release();

    Log::Info("Renderer::RefreshSwapChainForCurrentWindow: backbuffer " + std::to_string((int)backDesc.Width) + "x" + std::to_string((int)backDesc.Height) + " fmt " + std::to_string((int)backDesc.Format));
    LogSwapChainContainingOutput(swapChain_, "Renderer::RefreshSwapChainForCurrentWindow: after");
    debugReadbackFrames_ = 0;
    return true;
}

void Renderer::Render(ID3D11Texture2D* srcTex, float depth) {
    if (!context_ || !rtv_ || !swapChain_) {
        Log::Error("Renderer::Render: context_ or rtv_ is null");
        return;
    }

    const float clearBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    // --- Repeat frame detection: use timestamp-based method ---
    // The caller should call UpdateRepeat(frameTimestamp) before Render.
    // Log backbuffer and srcTex size/format (throttled)
    ID3D11Resource* dstRes = nullptr;
    rtv_->GetResource(&dstRes);
    ID3D11Texture2D* backBuffer = nullptr;
    dstRes->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&backBuffer);
    D3D11_TEXTURE2D_DESC backDesc = {};
    D3D11_TEXTURE2D_DESC srcDesc = {};
    backBuffer->GetDesc(&backDesc);
    if (srcTex) {
        srcTex->GetDesc(&srcDesc);
    }
    {
        static UINT lastBw = 0, lastBh = 0;
        static DXGI_FORMAT lastBf = DXGI_FORMAT_UNKNOWN;
        static UINT lastSw = 0, lastSh = 0;
        static DXGI_FORMAT lastSf = DXGI_FORMAT_UNKNOWN;
        if (backDesc.Width != lastBw || backDesc.Height != lastBh || backDesc.Format != lastBf ||
            (srcTex && (srcDesc.Width != lastSw || srcDesc.Height != lastSh || srcDesc.Format != lastSf))) {
            std::string info = "Renderer::Render: backbuffer " + std::to_string(backDesc.Width) + "x" + std::to_string(backDesc.Height) + " fmt " + std::to_string(backDesc.Format);
            if (srcTex) {
                info += ", src " + std::to_string(srcDesc.Width) + "x" + std::to_string(srcDesc.Height) + " fmt " + std::to_string(srcDesc.Format);
                lastSw = srcDesc.Width;
                lastSh = srcDesc.Height;
                lastSf = srcDesc.Format;
            }
            Log::Info(info);
            lastBw = backDesc.Width;
            lastBh = backDesc.Height;
            lastBf = backDesc.Format;
        }
    }

    context_->OMSetRenderTargets(1, &rtv_, nullptr);
    context_->ClearRenderTargetView(rtv_, clearBlack);

    // Ensure we have a shader-readable copy of the captured frame.
    // NOTE: Avoid per-frame CreateShaderResourceView(srcTex) on the capture texture; it's expensive.
    // Instead, keep a persistent cache texture with an SRV and copy the latest frame into it.
    bool gotNewFrame = false;
    if (srcTex) {
        gotNewFrame = true;
        downDirty_ = true;

        // Track the current source size/format even if we don't allocate srcCopy_.
        srcW_ = srcDesc.Width;
        srcH_ = srcDesc.Height;
        srcFmt_ = srcDesc.Format;

        bool needRecreate = (!srcCopy_ || !srcSrv_);
        if (!needRecreate && srcCopy_) {
            D3D11_TEXTURE2D_DESC cd{};
            srcCopy_->GetDesc(&cd);
            if (cd.Width != srcW_ || cd.Height != srcH_ || cd.Format != srcFmt_) {
                needRecreate = true;
            }
        }

        if (needRecreate) {
            if (srcSrv_) { srcSrv_->Release(); srcSrv_ = nullptr; }
            if (srcCopy_) { srcCopy_->Release(); srcCopy_ = nullptr; }

            D3D11_TEXTURE2D_DESC td = {};
            td.Width = srcW_;
            td.Height = srcH_;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = srcFmt_;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            HRESULT chr = device_->CreateTexture2D(&td, nullptr, &srcCopy_);
            if (FAILED(chr) || !srcCopy_) {
                Log::Error("Renderer::Render: failed to create srcCopy_");
            } else {
                D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
                sv.Format = srcFmt_;
                sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                sv.Texture2D.MipLevels = 1;
                chr = device_->CreateShaderResourceView(srcCopy_, &sv, &srcSrv_);
                if (FAILED(chr) || !srcSrv_) {
                    Log::Error("Renderer::Render: failed to create srcSrv_");
                }
            }
        }

        if (srcCopy_) {
            context_->CopyResource(srcCopy_, srcTex);
        }
    }

    // Update per-frame diagnostics rates once per presented frame.
    UpdateRateStats(gotNewFrame);

    auto updateStereoCb = [&](float uOffset, float eyeSign, float parallaxStrength) {
        if (!stereoCb_) return;
        struct StereoCB {
            float uOffset;
            float eyeSign;
            float warpStrength;
            float popStrength;

            float parallaxPercent;
            float macroDepthPercent;
            float pad1;
            float pad2;
        };
        StereoCB cb{};
        cb.uOffset = uOffset;
        cb.eyeSign = eyeSign;
        cb.warpStrength = 0.0f;
        cb.popStrength = 0.0f;
        cb.parallaxPercent = parallaxStrength;
        // Macro Depth is intentionally locked to neutral (100% / 1.0).
        cb.macroDepthPercent = 1.0f;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(stereoCb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) && mapped.pData) {
            memcpy(mapped.pData, &cb, sizeof(cb));
            context_->Unmap(stereoCb_, 0);
        }
    };

    auto updateCropCb = [&](bool enableCrop) {
        if (!cropCb_) return;
        struct CropCB { float cropOffset[2]; float cropScale[2]; };
        CropCB cb{};
        if (enableCrop && cropEnabled_) {
            const float l = cropLeft_;
            const float t = cropTop_;
            const float r = cropRight_;
            const float b = cropBottom_;
            cb.cropOffset[0] = l;
            cb.cropOffset[1] = t;
            cb.cropScale[0] = (r - l);
            cb.cropScale[1] = (b - t);
        } else {
            cb.cropOffset[0] = 0.0f;
            cb.cropOffset[1] = 0.0f;
            cb.cropScale[0] = 1.0f;
            cb.cropScale[1] = 1.0f;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(cropCb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) && mapped.pData) {
            memcpy(mapped.pData, &cb, sizeof(cb));
            context_->Unmap(cropCb_, 0);
        }
    };

    auto updateCursorCb = [&](bool foldU) {
        if (!cursorCb_) return;
        struct CursorCB {
            float x01;
            float y01;
            float sizePx;
            float enabled;
            float foldU;
            float pad1;
            float pad2;
            float pad3;
        };
        CursorCB cb{};
        cb.x01 = softwareCursorX01_;
        cb.y01 = softwareCursorY01_;
        cb.sizePx = 24.0f;
        cb.enabled = softwareCursorEnabled_ ? 1.0f : 0.0f;
        cb.foldU = foldU ? 1.0f : 0.0f;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(cursorCb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) && mapped.pData) {
            memcpy(mapped.pData, &cb, sizeof(cb));
            context_->Unmap(cursorCb_, 0);
        }
    };

    auto updateMenuCb = [&](bool foldU) {
        if (!menuCb_) return;
        struct MenuCB {
            float l;
            float t;
            float r;
            float b;
            float enabled;
            float foldU;
            float pad2;
            float pad3;
        };
        MenuCB cb{};
        cb.l = menuLeft01_;
        cb.t = menuTop01_;
        cb.r = menuRight01_;
        cb.b = menuBottom01_;
        cb.enabled = (menuOverlayEnabled_ && menuSrv_) ? 1.0f : 0.0f;
        cb.foldU = foldU ? 1.0f : 0.0f;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(menuCb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) && mapped.pData) {
            memcpy(mapped.pData, &cb, sizeof(cb));
            context_->Unmap(menuCb_, 0);
        }
    };

    auto computeDownscaleSize = [&](UINT srcW, UINT srcH, UINT boundW, UINT boundH, UINT* outW, UINT* outH) {
        if (!outW || !outH) return;
        *outW = *outH = 0;
        if (srcW == 0 || srcH == 0 || boundW == 0 || boundH == 0) return;

        const double sx = (double)boundW / (double)srcW;
        const double sy = (double)boundH / (double)srcH;
        double s = (std::min)(sx, sy);
        if (s > 1.0) s = 1.0;

        UINT w = (UINT)(std::max)(1.0, std::floor((double)srcW * s + 0.5));
        UINT h = (UINT)(std::max)(1.0, std::floor((double)srcH * s + 0.5));

        // Keep dimensions even to reduce chances of odd-size issues in some pipelines.
        if (w > 2) w &= ~1u;
        if (h > 2) h &= ~1u;

        *outW = w;
        *outH = h;
    };

    // Optional output-side downscale: render the captured frame into a smaller RT, then render that RT to backbuffer.
    // This keeps the output window/backbuffer size unchanged (e.g., 4K fullscreen) while reducing the texture work.
    ID3D11ShaderResourceView* srvToPresent = srcSrv_;
    if (renderResIndex_ > 0 && device_ && context_) {
        struct Preset { UINT w; UINT h; };
        static const Preset kPresets[] = {
            { 0, 0 },
            { 1280, 720 },
            { 1600, 900 },
            { 1920, 1080 },
            { 2560, 1440 },
            { 3840, 2160 },
        };

        const int idx = (renderResIndex_ >= 0 && renderResIndex_ < (int)(sizeof(kPresets) / sizeof(kPresets[0]))) ? renderResIndex_ : 0;
        const Preset p = kPresets[idx];

        UINT wantW = 0, wantH = 0;
        computeDownscaleSize(srcW_, srcH_, p.w, p.h, &wantW, &wantH);

        if (wantW > 0 && wantH > 0) {
            const bool needCreate = (!downTex_ || !downRtv_ || !downSrv_ || downW_ != wantW || downH_ != wantH || downDirty_);
            if (needCreate) {
                if (downSrv_) { downSrv_->Release(); downSrv_ = nullptr; }
                if (downRtv_) { downRtv_->Release(); downRtv_ = nullptr; }
                if (downTex_) { downTex_->Release(); downTex_ = nullptr; }

                D3D11_TEXTURE2D_DESC td = {};
                td.Width = wantW;
                td.Height = wantH;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = srcFmt_;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                HRESULT hr = device_->CreateTexture2D(&td, nullptr, &downTex_);
                if (SUCCEEDED(hr) && downTex_) {
                    hr = device_->CreateRenderTargetView(downTex_, nullptr, &downRtv_);
                }
                if (SUCCEEDED(hr) && downTex_) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
                    sv.Format = srcFmt_;
                    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                    sv.Texture2D.MipLevels = 1;
                    hr = device_->CreateShaderResourceView(downTex_, &sv, &downSrv_);
                }

                if (FAILED(hr) || !downTex_ || !downRtv_ || !downSrv_) {
                    if (downSrv_) { downSrv_->Release(); downSrv_ = nullptr; }
                    if (downRtv_) { downRtv_->Release(); downRtv_ = nullptr; }
                    if (downTex_) { downTex_->Release(); downTex_ = nullptr; }
                    downW_ = downH_ = 0;
                    downDirty_ = true;
                } else {
                    downW_ = wantW;
                    downH_ = wantH;
                    downDirty_ = true;
                }
            }

            // Use the persistent cache SRV as the downscale source.
            ID3D11ShaderResourceView* downSrcSrv = srcSrv_;

            if (downRtv_ && downSrv_ && downTex_ && downDirty_ && downSrcSrv) {
                context_->OMSetRenderTargets(1, &downRtv_, nullptr);
                context_->ClearRenderTargetView(downRtv_, clearBlack);

                D3D11_VIEWPORT dvp;
                dvp.TopLeftX = 0;
                dvp.TopLeftY = 0;
                dvp.Width = (FLOAT)downW_;
                dvp.Height = (FLOAT)downH_;
                dvp.MinDepth = 0.0f;
                dvp.MaxDepth = 1.0f;
                context_->RSSetViewports(1, &dvp);

                const UINT stride = 16;
                const UINT offset = 0;
                context_->IASetInputLayout(inputLayout_);
                context_->IASetVertexBuffers(0, 1, &vertexBuffer_, &stride, &offset);
                context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                context_->VSSetShader(vs_, nullptr, 0);
                // For the downscale pass, always use the standard blit shader.
                context_->PSSetShader(psStandard_, nullptr, 0);
                context_->PSSetSamplers(0, 1, &sampler_);

                // The pixel shader requires both StereoCB (b0) and CropCB (b1).
                // For the downscale pass, we always apply the source crop (if enabled) and disable stereo shift.
                if (stereoCb_) {
                    context_->PSSetConstantBuffers(0, 1, &stereoCb_);
                }
                if (cropCb_) {
                    context_->PSSetConstantBuffers(1, 1, &cropCb_);
                }
                if (cursorCb_) {
                    context_->PSSetConstantBuffers(2, 1, &cursorCb_);
                }
                // Ensure menu overlay is disabled for the downscale pass.
                {
                    ID3D11Buffer* nullCb = nullptr;
                    context_->PSSetConstantBuffers(3, 1, &nullCb);
                    ID3D11ShaderResourceView* nullSrv = nullptr;
                    context_->PSSetShaderResources(1, 1, &nullSrv);
                }
                updateCropCb(true);
                updateStereoCb(0.0f, 0.0f, 0.0f);
                updateCursorCb(false);

                context_->PSSetShaderResources(0, 1, &downSrcSrv);
                context_->Draw(3, 0);
                UnbindPSResource(context_, 0);

                downDirty_ = false;
            }

            if (downSrv_) {
                srvToPresent = downSrv_;
            }
        }
    }

    // If downscaling is enabled and we have a downscaled texture, prefer it.
    // If downscaling is enabled but we couldn't produce a downscaled texture, fall back to srcSrv_.

    bool presentingDownscaled = (srvToPresent && downSrv_ && (srvToPresent == downSrv_));

    bool depthStereoPresented = false;
    const bool wantDepthCompute = (stereoShaderMode_ == StereoShaderMode::Depth3Pass);

    // Diagnostic: log the stereo shader path once so Debug/Release behavior can be compared via logs.
    static bool s_loggedStereoPathOnce = false;
    if (!s_loggedStereoPathOnce) {
        Log::Info(std::string("Renderer::Render stereo mode=") + std::to_string((int)stereoShaderMode_) +
                  " wantDepthCompute=" + std::to_string((int)wantDepthCompute) +
                  " stereoEnabled=" + std::to_string((int)stereoEnabled_));
        s_loggedStereoPathOnce = true;
    }

    ID3D11ComputeShader* csDepthRawActive = csDepthRaw_;
    ID3D11ComputeShader* csDepthSmoothActive = csDepthSmooth_;
    ID3D11ComputeShader* csParallaxActive = csParallaxSbs_;

    if (stereoEnabled_ && wantDepthCompute && srvToPresent && csDepthRawActive && csDepthSmoothActive && csParallaxActive && csParamsCb_ && sampler_) {
        // IMPORTANT: The compute-based depth stereo pipeline should operate at the resolution of the
        // texture being processed (native capture or downscaled), not at the swapchain backbuffer size.
        // This avoids Debug/Release mismatches when the swapchain is sized to the window.
        UINT computeW = 0;
        UINT computeH = 0;
        if (presentingDownscaled && downW_ > 0 && downH_ > 0) {
            computeW = downW_;
            computeH = downH_;
        } else if (srcW_ > 0 && srcH_ > 0) {
            computeW = srcW_;
            computeH = srcH_;
        } else {
            // Fallback if we don't yet know the source size (e.g. first frame).
            computeW = backDesc.Width;
            computeH = backDesc.Height;
        }

        static bool s_loggedDepthComputeDimsOnce = false;
        if (!s_loggedDepthComputeDimsOnce) {
            Log::Info(
                std::string("Renderer::Render depth compute dims=") +
                std::to_string((int)computeW) + "x" + std::to_string((int)computeH) +
                " (backbuffer=" + std::to_string((int)backDesc.Width) + "x" + std::to_string((int)backDesc.Height) +
                ", src=" + std::to_string((int)srcW_) + "x" + std::to_string((int)srcH_) +
                ", down=" + std::to_string((int)downW_) + "x" + std::to_string((int)downH_) +
                ")"
            );
            s_loggedDepthComputeDimsOnce = true;
        }

        EnsureDepthStereoResources(computeW, computeH);

        if (depthRawUav_ && depthRawSrv_ && depthSmoothUav_ && depthSmoothSrv_ && depthPrevSrv_[0] && depthPrevSrv_[1] && depthPrevUav_[0] && depthPrevUav_[1] && stereoOutUav_ && stereoOutSrv_) {
            // Update CS parameters.
            struct CSParams {
                UINT outWidth;
                UINT outHeight;
                UINT mode3d;
                INT zoomLevel;

                float parallaxPx;
                float frame;
                float pad0[2];

                float cropOffset[2];
                float cropScale[2];
            };

            CSParams cb{};
            cb.outWidth = computeW;
            cb.outHeight = computeH;
            cb.mode3d = 2; // SBS
            cb.zoomLevel = 0;

            const float t = (float)stereoDepthLevel_ / 20.0f;
            const float maxShiftPx = 60.0f;
            const float parallaxStrength = (float)stereoParallaxStrengthPercent_ / 100.0f;
            cb.parallaxPx = t * maxShiftPx * parallaxStrength;
            cb.frame = depthFrame_;
            depthFrame_ += 1.0f;

            // Crop mapping is applied inside the compute path when sampling the source.
            // If we're already using the downscaled RT as input, the crop has already been applied.
            const bool needCrop = !presentingDownscaled;
            if (needCrop && cropEnabled_) {
                cb.cropOffset[0] = cropLeft_;
                cb.cropOffset[1] = cropTop_;
                cb.cropScale[0] = (cropRight_ - cropLeft_);
                cb.cropScale[1] = (cropBottom_ - cropTop_);
            } else {
                cb.cropOffset[0] = 0.0f;
                cb.cropOffset[1] = 0.0f;
                cb.cropScale[0] = 1.0f;
                cb.cropScale[1] = 1.0f;
            }

            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context_->Map(csParamsCb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) && mapped.pData) {
                memcpy(mapped.pData, &cb, sizeof(cb));
                context_->Unmap(csParamsCb_, 0);
            }

            const UINT gx = DivRoundUp(computeW, 16);
            const UINT gy = DivRoundUp(computeH, 16);

            // Pass 1: depth raw (writes u0).
            {
                context_->CSSetShader(csDepthRawActive, nullptr, 0);
                context_->CSSetSamplers(0, 1, &sampler_);
                context_->CSSetConstantBuffers(0, 1, &csParamsCb_);

                context_->CSSetShaderResources(0, 1, &srvToPresent);
                context_->CSSetUnorderedAccessViews(0, 1, &depthRawUav_, nullptr);
                context_->Dispatch(gx, gy, 1);

                UnbindCSUav(context_, 0);
                UnbindCSResource(context_, 0);
            }

            // Pass 2: depth smooth with history ping-pong (reads t1=depthRaw, t2=depthPrev; writes u1=depthPrevNext, u2=depthSmooth).
            {
                const int prevIdx = depthPrevIndex_ & 1;
                const int nextIdx = (depthPrevIndex_ ^ 1) & 1;

                context_->CSSetShader(csDepthSmoothActive, nullptr, 0);
                context_->CSSetSamplers(0, 1, &sampler_);
                context_->CSSetConstantBuffers(0, 1, &csParamsCb_);

                ID3D11ShaderResourceView* srvs[3] = { nullptr, depthRawSrv_, depthPrevSrv_[prevIdx] };
                context_->CSSetShaderResources(0, 3, srvs);

                ID3D11UnorderedAccessView* uavs[2] = { depthPrevUav_[nextIdx], depthSmoothUav_ };
                context_->CSSetUnorderedAccessViews(1, 2, uavs, nullptr);

                context_->Dispatch(gx, gy, 1);

                UnbindCSUav(context_, 1);
                UnbindCSUav(context_, 2);
                UnbindCSResource(context_, 1);
                UnbindCSResource(context_, 2);

                depthPrevIndex_ = nextIdx;
            }

            // Pass 3: parallax SBS (reads t0=src, t1=depthSmooth; writes u3=stereoOut).
            {
                context_->CSSetShader(csParallaxActive, nullptr, 0);
                context_->CSSetSamplers(0, 1, &sampler_);
                context_->CSSetConstantBuffers(0, 1, &csParamsCb_);

                ID3D11ShaderResourceView* srvs[2] = { srvToPresent, depthSmoothSrv_ };
                context_->CSSetShaderResources(0, 2, srvs);
                context_->CSSetUnorderedAccessViews(3, 1, &stereoOutUav_, nullptr);
                context_->Dispatch(gx, gy, 1);

                UnbindCSUav(context_, 3);
                UnbindCSResource(context_, 0);
                UnbindCSResource(context_, 1);
            }

            context_->CSSetShader(nullptr, nullptr, 0);

            // Present the computed SBS image.
            srvToPresent = stereoOutSrv_;
            presentingDownscaled = true;
            depthStereoPresented = true;
        }
    }

    static bool s_loggedStereoPresentedOnce = false;
    if (!s_loggedStereoPresentedOnce) {
        Log::Info(std::string("Renderer::Render depthStereoPresented=") + std::to_string((int)depthStereoPresented) +
                  " (0 means fallback to standard PS path)");
        s_loggedStereoPresentedOnce = true;
    }

    // Final present pass: draw fullscreen triangle sampling srvToPresent into the backbuffer.
    // NOTE: The optional downscale pass binds a different RTV; always rebind the swapchain backbuffer RTV here.
    context_->OMSetRenderTargets(1, &rtv_, nullptr);

    const UINT stride = 16;
    const UINT offset = 0;
    context_->IASetInputLayout(inputLayout_);
    context_->IASetVertexBuffers(0, 1, &vertexBuffer_, &stride, &offset);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vs_, nullptr, 0);

    context_->PSSetShader(psStandard_, nullptr, 0);
    context_->PSSetSamplers(0, 1, &sampler_);

    // Bind stereo CB (PS b0). We'll update it per-eye.
    if (stereoCb_) {
        context_->PSSetConstantBuffers(0, 1, &stereoCb_);
    }

    // Bind crop CB (PS b1). We'll update it per-pass.
    if (cropCb_) {
        context_->PSSetConstantBuffers(1, 1, &cropCb_);
    }

    // Bind cursor CB (PS b2).
    if (cursorCb_) {
        context_->PSSetConstantBuffers(2, 1, &cursorCb_);
    }

    // Bind menu CB (PS b3).
    if (menuCb_) {
        context_->PSSetConstantBuffers(3, 1, &menuCb_);
    }


    const float parallaxStrength = (float)stereoParallaxStrengthPercent_ / 100.0f;

    // Base per-eye disparity magnitude for the standard PS stereo shift (in UV units of the presented texture).
    float uOffset = 0.0f;
    if (stereoEnabled_ && srvToPresent) {
        const float t = (float)stereoDepthLevel_ / 20.0f;
        const float maxShiftPx = 60.0f;
        const float shiftPx = t * maxShiftPx;
        const float texW = (float)(presentingDownscaled ? downW_ : srcW_);
        if (texW > 1.0f) {
            uOffset = shiftPx / texW;
        }
    }

    if (srvToPresent) {
        context_->PSSetShaderResources(0, 1, &srvToPresent);

        if (menuOverlayEnabled_ && menuSrv_) {
            context_->PSSetShaderResources(1, 1, &menuSrv_);
        } else {
            ID3D11ShaderResourceView* nullSrv = nullptr;
            context_->PSSetShaderResources(1, 1, &nullSrv);
        }

        if (stereoEnabled_ && !depthStereoPresented) {
            // Split the backbuffer into two viewports.
            // Use an integer split that guarantees full coverage even for odd widths.
            const UINT leftWpx = backDesc.Width / 2;
            const UINT rightWpx = backDesc.Width - leftWpx;
            const FLOAT leftW = (FLOAT)leftWpx;
            const FLOAT rightW = (FLOAT)rightWpx;
            const FLOAT fullH = (FLOAT)backDesc.Height;

            // Left eye
            {
                D3D11_VIEWPORT vp{};
                vp.TopLeftX = 0.0f;
                vp.TopLeftY = 0.0f;
                vp.Width = leftW;
                vp.Height = fullH;
                vp.MinDepth = 0.0f;
                vp.MaxDepth = 1.0f;
                context_->RSSetViewports(1, &vp);
                // Apply crop only when sampling the original source. If we're presenting the downscaled RT,
                // the crop has already been applied in the downscale pass.
                updateCropCb(!presentingDownscaled);
                // Per-eye disparity.
                updateStereoCb(uOffset, -1.0f, parallaxStrength);
                updateCursorCb(false);
                updateMenuCb(false);
                context_->Draw(3, 0);
            }

            // Right eye
            {
                D3D11_VIEWPORT vp{};
                vp.TopLeftX = leftW;
                vp.TopLeftY = 0.0f;
                vp.Width = rightW;
                vp.Height = fullH;
                vp.MinDepth = 0.0f;
                vp.MaxDepth = 1.0f;
                context_->RSSetViewports(1, &vp);
                updateCropCb(!presentingDownscaled);
                updateStereoCb(uOffset, +1.0f, parallaxStrength);
                updateCursorCb(false);
                updateMenuCb(false);
                context_->Draw(3, 0);
            }
        } else {
            D3D11_VIEWPORT vp{};
            vp.TopLeftX = 0.0f;
            vp.TopLeftY = 0.0f;
            vp.Width = (FLOAT)backDesc.Width;
            vp.Height = (FLOAT)backDesc.Height;
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            context_->RSSetViewports(1, &vp);
            updateCropCb(!presentingDownscaled);
            updateStereoCb(0.0f, 0.0f, 0.0f);
            // When the presented texture already contains SBS (depth compute path), fold U so the cursor draws in both halves.
            updateCursorCb(stereoEnabled_ && depthStereoPresented);
            updateMenuCb(stereoEnabled_ && depthStereoPresented);
            context_->Draw(3, 0);
        }

        UnbindPSResource(context_, 0);
        UnbindPSResource(context_, 1);
    }

    // Note: Diagnostics overlay is drawn as part of the presented frame when possible.

    // Diagnostic: read back a center pixel from the swapchain buffer for a few frames.
    if (backbufferReadback_ && debugReadbackFrames_ < 6) {
        context_->CopyResource(backbufferReadback_, backBuffer);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT mhr = context_->Map(backbufferReadback_, 0, D3D11_MAP_READ, 0, &mapped);
        if (SUCCEEDED(mhr) && mapped.pData) {
            const UINT cx = backDesc.Width / 2;
            const UINT cy = backDesc.Height / 2;
            uint8_t* px = (uint8_t*)mapped.pData + (size_t)cy * mapped.RowPitch + (size_t)cx * 4;
            Log::Info(
                "Backbuffer sample (" + std::to_string(cx) + "," + std::to_string(cy) + "): "
                "[B:" + std::to_string(px[0]) + ",G:" + std::to_string(px[1]) + ",R:" + std::to_string(px[2]) + ",A:" + std::to_string(px[3]) + "]"
            );
            context_->Unmap(backbufferReadback_, 0);
        } else {
            Log::Error("Backbuffer readback: Map failed");
        }
        ++debugReadbackFrames_;
    }

    auto buildOverlayText = [&](wchar_t* outBuf, size_t outCch, UINT dpi, bool /*gotNewFrameThisTick*/) {
        const double presentFps = presentFps_;
        const double newFrameFps = newFrameFps_;

        int winW = 0, winH = 0;
        if (hWnd_) {
            RECT cr{};
            if (GetClientRect(hWnd_, &cr)) {
                winW = (int)(cr.right - cr.left);
                winH = (int)(cr.bottom - cr.top);
            }
        }

        // srcW_/srcH_ are the most recent captured frame dimensions.
        // backDesc is the swapchain backbuffer dimensions.
        const double perEyeFps = presentFps;
        const double viewsPerSecond = presentFps * (stereoEnabled_ ? 2.0 : 1.0);
        const double newPerEyeFps = newFrameFps;
        const double newViewsPerSecond = newFrameFps * (stereoEnabled_ ? 2.0 : 1.0);

        // "Target" is the app's present cadence selection (tray framerate), not the game's frame production rate.
        wchar_t targetBuf[16] = L"";
        double targetFps = 0.0;
        if (framerateIndex_ >= 0 && framerateIndex_ <= 3) {
            static const int kTargets[] = { 60, 72, 90, 120 };
            targetFps = (double)kTargets[framerateIndex_];
            swprintf(targetBuf, _countof(targetBuf), L"%d", kTargets[framerateIndex_]);
        } else {
            swprintf(targetBuf, _countof(targetBuf), L"Unlim");
        }

        const wchar_t* matchLabel = L"";
        if (targetFps > 0.0) {
            const double tol = max(1.0, targetFps * 0.05); // within 5% or 1fps
            if (presentFps < targetFps - tol) matchLabel = L"LOW";
            else if (presentFps > targetFps + tol) matchLabel = L"HIGH";
            else matchLabel = L"OK";
        }

        // Best-effort estimate of the source/game cadence.
        // - WGC: prefer produced frame rate (frames drained from the pool). FrameArrived callbacks can batch
        //        multiple frames and under-report cadence if you look at event frequency.
        // - DXGI: use produced rate (best proxy for desktop cadence; includes AccumulatedFrames when falling behind).
        double capFps = 0.0;
        const wchar_t* capLabel = L"";
        if (captureStatsBackend_ == CaptureBackendStats::WGC) {
            if (wgcProducedFps_ > 0.0) {
                capFps = wgcProducedFps_;
                capLabel = L"prod";
            } else if (wgcCaptureFpsEstimate_ > 0.0) {
                capFps = wgcCaptureFpsEstimate_;
                capLabel = L"ts";
            } else if (wgcArrivedFps_ > 0.0) {
                capFps = wgcArrivedFps_;
                capLabel = L"ev";
            }
        } else if (captureStatsBackend_ == CaptureBackendStats::DXGI) {
            capFps = dxgiProducedFps_;
            capLabel = L"prod";
        }

        // Extra context for interpreting Cap.
        wchar_t capExtra[96] = L"";
        if (captureStatsBackend_ == CaptureBackendStats::WGC) {
            // FrameArrived callbacks can batch multiple frames; show both.
            swprintf(capExtra, _countof(capExtra), L"(ev %.0f prod %.0f cons %.0f)", wgcArrivedFps_, wgcProducedFps_, wgcConsumedFps_);
        } else if (captureStatsBackend_ == CaptureBackendStats::DXGI) {
            // AccumulatedFrames > 1 implies source/desktop cadence is higher than our sampling.
            swprintf(capExtra, _countof(capExtra), L"(acc %u)", (unsigned)dxgiLastAccumulated_);
        }

        if (overlayCompact_) {
            // Extra stat: capture delivery vs consumption.
            wchar_t capLine[160] = L"";
            if (captureStatsBackend_ == CaptureBackendStats::DXGI) {
                swprintf(capLine, _countof(capLine), L"Cap: DXGI prod %.1f (acc %u)", dxgiProducedFps_, (unsigned)dxgiLastAccumulated_);
            } else if (captureStatsBackend_ == CaptureBackendStats::WGC) {
                const long long backlog = (long long)wgcProducedTotal_ - (long long)wgcConsumedTotal_;
                if (wgcCaptureFpsEstimate_ > 0.0) {
                    swprintf(capLine, _countof(capLine), L"Cap: WGC ev %.1f prod %.1f cons %.1f ts %.1f (q %lld)",
                        wgcArrivedFps_, wgcProducedFps_, wgcConsumedFps_, wgcCaptureFpsEstimate_, backlog);
                } else {
                    swprintf(capLine, _countof(capLine), L"Cap: WGC ev %.1f prod %.1f cons %.1f (q %lld)",
                        wgcArrivedFps_, wgcProducedFps_, wgcConsumedFps_, backlog);
                }
            }

            const UINT rendW = (downW_ ? downW_ : srcW_);
            const UINT rendH = (downH_ ? downH_ : srcH_);

            if (srcW_ > 0 && srcH_ > 0) {
                swprintf(outBuf, outCch,
                    L"Out: %.1f/%s %s (new %.1f)  Cap: %.1f %s %s\nSrc: %ux%u  Rend: %ux%u  Out: %ux%u\nStereo: %s (%d)  VSync: %s\n%s",
                    presentFps,
                    targetBuf,
                    matchLabel,
                    newFrameFps,
                    capFps,
                    capLabel,
                    capExtra,
                    (unsigned)srcW_, (unsigned)srcH_,
                    (unsigned)rendW, (unsigned)rendH,
                    (unsigned)backDesc.Width, (unsigned)backDesc.Height,
                    stereoEnabled_ ? L"Half-SBS" : L"Off",
                    stereoDepthLevel_,
                    vsyncEnabled_ ? L"On" : L"Off",
                    capLine);
            } else {
                swprintf(outBuf, outCch,
                    L"Out: %.1f/%s %s (new %.1f)  Cap: %.1f %s %s\nOut: %ux%u\nStereo: %s (%d)  VSync: %s\n%s",
                    presentFps,
                    targetBuf,
                    matchLabel,
                    newFrameFps,
                    capFps,
                    capLabel,
                    capExtra,
                    (unsigned)backDesc.Width, (unsigned)backDesc.Height,
                    stereoEnabled_ ? L"Half-SBS" : L"Off",
                    stereoDepthLevel_,
                    vsyncEnabled_ ? L"On" : L"Off",
                    capLine);
            }
            return;
        }

        wchar_t capStatsBuf[160] = L"(none)";
        if (captureStatsBackend_ == CaptureBackendStats::DXGI) {
            swprintf(capStatsBuf, _countof(capStatsBuf), L"DXGI prod %.1f/s acc %u", dxgiProducedFps_, (unsigned)dxgiLastAccumulated_);
        } else if (captureStatsBackend_ == CaptureBackendStats::WGC) {
            const long long backlog = (long long)wgcProducedTotal_ - (long long)wgcConsumedTotal_;
            swprintf(capStatsBuf, _countof(capStatsBuf), L"WGC ev %.1f/s prod %.1f/s cons %.1f/s q %lld", wgcArrivedFps_, wgcProducedFps_, wgcConsumedFps_, backlog);
        }

        if (srcW_ > 0 && srcH_ > 0) {
            swprintf(outBuf, outCch,
            L"Output Present: %.1f fps\nOutput New: %.1f fps\nSource Cap: %.1f %s\nPer-eye: %.1f fps\nViews: %.1f /s\nNew Views: %.1f /s\nRepeat: %d\nDPI: %u\nVSync: %s\nCapture: %ux%u\nRender: %ux%u\nStereo: %s (Depth %d)\nOutput: %ux%u\nWindow: %dx%d\nCapStats: %s",
                presentFps,
                newFrameFps,
                capFps,
                capLabel,
            perEyeFps,
            viewsPerSecond,
            newViewsPerSecond,
                repeatCount_,
                dpi,
                vsyncEnabled_ ? L"On" : L"Off",
                (unsigned)srcW_, (unsigned)srcH_,
                (unsigned)(downW_ ? downW_ : srcW_), (unsigned)(downH_ ? downH_ : srcH_),
                stereoEnabled_ ? L"Half-SBS" : L"Off",
                stereoDepthLevel_,
                (unsigned)backDesc.Width, (unsigned)backDesc.Height,
                winW, winH,
                capStatsBuf
            );
        } else {
            swprintf(outBuf, outCch,
            L"Output Present: %.1f fps\nOutput New: %.1f fps\nSource Cap: %.1f %s\nPer-eye: %.1f fps\nViews: %.1f /s\nNew Views: %.1f /s\nRepeat: %d\nDPI: %u\nVSync: %s\nCapture: (none)\nRender: (n/a)\nStereo: %s (Depth %d)\nOutput: %ux%u\nWindow: %dx%d\nCapStats: %s",
                presentFps,
                newFrameFps,
                capFps,
                capLabel,
            perEyeFps,
            viewsPerSecond,
            newViewsPerSecond,
                repeatCount_,
                dpi,
                vsyncEnabled_ ? L"On" : L"Off",
                stereoEnabled_ ? L"Half-SBS" : L"Off",
                stereoDepthLevel_,
                (unsigned)backDesc.Width, (unsigned)backDesc.Height,
                winW, winH,
                capStatsBuf
            );
        }
    };

    auto drawOverlayToHdc = [&](HDC hdc, UINT dpi, const RECT* clipRc) {
        if (!hdc) return;
        EnsureOverlayFont(dpi);

        const int marginBase = (overlaySizeIndex_ == 0) ? 4 : 6;
        const int padXBase = (overlaySizeIndex_ == 0) ? 4 : 6;
        const int padYBase = (overlaySizeIndex_ == 0) ? 3 : 5;
        const int margin = MulDiv(marginBase, (int)dpi, 96);
        const int padX = MulDiv(padXBase, (int)dpi, 96);
        const int padY = MulDiv(padYBase, (int)dpi, 96);

        RECT bounds{};
        if (clipRc) {
            bounds = *clipRc;
        } else {
            bounds.left = 0;
            bounds.top = 0;
            bounds.right = (LONG)backDesc.Width;
            bounds.bottom = (LONG)backDesc.Height;
        }

        const int boundsW = (int)(bounds.right - bounds.left);
        const int boundsH = (int)(bounds.bottom - bounds.top);

        HGDIOBJ oldFont = nullptr;
        if (overlayFont_) {
            oldFont = SelectObject(hdc, overlayFont_);
        }

        const bool gotNewFrameThisTick = (srcTex != nullptr);

        wchar_t buf[768];
        buildOverlayText(buf, _countof(buf), dpi, gotNewFrameThisTick);

        // Measure text to avoid huge backgrounds. Constrain width so it wraps nicely.
        const int maxWidthBase = (overlaySizeIndex_ == 0) ? 260 : 320;
        const int maxTextW = (boundsW > 0)
            ? max(80, min(MulDiv(maxWidthBase, (int)dpi, 96), boundsW - (margin * 2) - (padX * 2)))
            : MulDiv(maxWidthBase, (int)dpi, 96);

        RECT textCalc{ 0, 0, maxTextW, 0 };
        DrawTextW(hdc, buf, -1, &textCalc, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_CALCRECT);

        const int textW = max(1, (int)(textCalc.right - textCalc.left));
        const int textH = max(1, (int)(textCalc.bottom - textCalc.top));
        const int boxW = textW + (padX * 2);
        const int boxH = textH + (padY * 2);

        int left = bounds.left + margin;
        int top = bounds.top + margin;
        switch (overlayPosition_) {
        case Renderer::OverlayPosition::TopLeft:
            left = bounds.left + margin;
            top = bounds.top + margin;
            break;
        case Renderer::OverlayPosition::TopRight:
            left = bounds.right - margin - boxW;
            top = bounds.top + margin;
            break;
        case Renderer::OverlayPosition::BottomLeft:
            left = bounds.left + margin;
            top = bounds.bottom - margin - boxH;
            break;
        case Renderer::OverlayPosition::BottomRight:
            left = bounds.right - margin - boxW;
            top = bounds.bottom - margin - boxH;
            break;
        case Renderer::OverlayPosition::Center:
            left = bounds.left + (boundsW - boxW) / 2;
            top = bounds.top + (boundsH - boxH) / 2;
            break;
        default:
            break;
        }

        // Safety clamp within bounds.
        left = max((int)bounds.left, min(left, (int)bounds.right - boxW));
        top = max((int)bounds.top, min(top, (int)bounds.bottom - boxH));

        RECT bgRc = { left, top, left + boxW, top + boxH };

        int saved = SaveDC(hdc);
        if (clipRc) {
            HRGN clip = CreateRectRgn(clipRc->left, clipRc->top, clipRc->right, clipRc->bottom);
            SelectClipRgn(hdc, clip);
            DeleteObject(clip);
        }

        // Charcoal, slightly translucent background behind the overlay.
        // Prefer runtime-loaded AlphaBlend; fall back to opaque fill if unavailable.
        const COLORREF bg = RGB(28, 28, 28);
        const BYTE bgAlpha = 200; // 0..255
        bool bgDrawn = TryAlphaBlendRect(hdc, bgRc.left, bgRc.top, boxW, boxH, bg, bgAlpha);
        if (!bgDrawn) {
            HBRUSH b = CreateSolidBrush(bg);
            FillRect(hdc, &bgRc, b);
            DeleteObject(b);
        }

        RECT textRc{ bgRc.left + padX, bgRc.top + padY, bgRc.right - padX, bgRc.bottom - padY };
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 232, 140));
        DrawTextW(hdc, buf, -1, &textRc, DT_LEFT | DT_TOP | DT_WORDBREAK);

        if (oldFont) {
            SelectObject(hdc, oldFont);
        }

        if (saved) {
            RestoreDC(hdc, saved);
        }
    };

    bool overlayDrawnInBackbuffer = false;

    // Diagnostics overlay: preferred path draws into the swapchain backbuffer (pre-Present).
    // NOTE: GDI-on-swapchain can be a GPU/CPU sync point under load; users can toggle it off.
    if (diagnosticsOverlay_ && hWnd_ && swapChain_) {
        const UINT dpi = GetDpiForWindow(hWnd_);

        winrt::com_ptr<IDXGISurface1> surface;
        HRESULT shr = swapChain_->GetBuffer(0, __uuidof(IDXGISurface1), surface.put_void());
        if (SUCCEEDED(shr) && surface) {
            // Make sure D3D isn't holding the backbuffer as an RTV while GDI touches it.
            context_->OMSetRenderTargets(0, nullptr, nullptr);

            HDC hdc = nullptr;
            shr = surface->GetDC(FALSE, &hdc);
            if (SUCCEEDED(shr) && hdc) {
                if (stereoEnabled_) {
                    const int halfW = (int)(backDesc.Width / 2);
                    RECT leftClip{ 0, 0, halfW, (LONG)backDesc.Height };
                    RECT rightClip{ halfW, 0, (LONG)backDesc.Width, (LONG)backDesc.Height };
                    drawOverlayToHdc(hdc, dpi, &leftClip);
                    drawOverlayToHdc(hdc, dpi, &rightClip);
                } else {
                    drawOverlayToHdc(hdc, dpi, nullptr);
                }
                surface->ReleaseDC(nullptr);
                overlayDrawnInBackbuffer = true;
            } else {
                static int logged = 0;
                if (logged++ < 3) {
                    Log::Error("Renderer overlay: IDXGISurface1::GetDC failed hr=" + std::to_string((long)shr));
                }
            }
        } else {
            static int logged = 0;
            if (logged++ < 3) {
                Log::Error("Renderer overlay: swapChain GetBuffer(IDXGISurface1) failed hr=" + std::to_string((long)shr));
            }
        }
    }

    dstRes->Release();
    backBuffer->Release();
    const UINT syncInterval = vsyncEnabled_ ? 1u : 0u;
    HRESULT phr = swapChain_->Present(syncInterval, 0);
    if (FAILED(phr)) {
        Log::Error("Renderer::Render: Present failed: hr=" + std::to_string((long)phr));
        if (device_) {
            HRESULT rr = device_->GetDeviceRemovedReason();
            Log::Error("Renderer::Render: DeviceRemovedReason hr=" + std::to_string((long)rr));
        }
    }

    // Fallback overlay path: draw after Present onto the window DC.
    // This can flicker on some systems, but is better than no overlay at all.
    if (diagnosticsOverlay_ && hWnd_ && !overlayDrawnInBackbuffer) {
        const UINT dpi = GetDpiForWindow(hWnd_);
        HDC hdc = GetDC(hWnd_);
        if (hdc) {
            if (stereoEnabled_) {
                const int halfW = (int)(backDesc.Width / 2);
                RECT leftClip{ 0, 0, halfW, (LONG)backDesc.Height };
                RECT rightClip{ halfW, 0, (LONG)backDesc.Width, (LONG)backDesc.Height };
                drawOverlayToHdc(hdc, dpi, &leftClip);
                drawOverlayToHdc(hdc, dpi, &rightClip);
            } else {
                drawOverlayToHdc(hdc, dpi, nullptr);
            }
            ReleaseDC(hWnd_, hdc);
        }
    }
}

void Renderer::Cleanup() {
    if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
    if (swapChain_) { swapChain_->Release(); swapChain_ = nullptr; }
    if (backbufferReadback_) { backbufferReadback_->Release(); backbufferReadback_ = nullptr; }
    if (srcSrv_) { srcSrv_->Release(); srcSrv_ = nullptr; }
    if (srcCopy_) { srcCopy_->Release(); srcCopy_ = nullptr; }
    if (sampler_) { sampler_->Release(); sampler_ = nullptr; }
    if (vertexBuffer_) { vertexBuffer_->Release(); vertexBuffer_ = nullptr; }
    if (inputLayout_) { inputLayout_->Release(); inputLayout_ = nullptr; }
    if (psStandard_) { psStandard_->Release(); psStandard_ = nullptr; }
    if (csDepthRaw_) { csDepthRaw_->Release(); csDepthRaw_ = nullptr; }
    if (csDepthSmooth_) { csDepthSmooth_->Release(); csDepthSmooth_ = nullptr; }
    if (csParallaxSbs_) { csParallaxSbs_->Release(); csParallaxSbs_ = nullptr; }
    if (csParamsCb_) { csParamsCb_->Release(); csParamsCb_ = nullptr; }

    if (depthRawSrv_) { depthRawSrv_->Release(); depthRawSrv_ = nullptr; }
    if (depthRawUav_) { depthRawUav_->Release(); depthRawUav_ = nullptr; }
    if (depthRawTex_) { depthRawTex_->Release(); depthRawTex_ = nullptr; }

    if (depthSmoothSrv_) { depthSmoothSrv_->Release(); depthSmoothSrv_ = nullptr; }
    if (depthSmoothUav_) { depthSmoothUav_->Release(); depthSmoothUav_ = nullptr; }
    if (depthSmoothTex_) { depthSmoothTex_->Release(); depthSmoothTex_ = nullptr; }

    for (int i = 0; i < 2; ++i) {
        if (depthPrevSrv_[i]) { depthPrevSrv_[i]->Release(); depthPrevSrv_[i] = nullptr; }
        if (depthPrevUav_[i]) { depthPrevUav_[i]->Release(); depthPrevUav_[i] = nullptr; }
        if (depthPrevTex_[i]) { depthPrevTex_[i]->Release(); depthPrevTex_[i] = nullptr; }
    }
    depthPrevIndex_ = 0;
    depthFrame_ = 0.0f;

    if (stereoOutSrv_) { stereoOutSrv_->Release(); stereoOutSrv_ = nullptr; }
    if (stereoOutUav_) { stereoOutUav_->Release(); stereoOutUav_ = nullptr; }
    if (stereoOutTex_) { stereoOutTex_->Release(); stereoOutTex_ = nullptr; }
    depthOutW_ = depthOutH_ = 0;
    if (vs_) { vs_->Release(); vs_ = nullptr; }
    if (stereoCb_) { stereoCb_->Release(); stereoCb_ = nullptr; }
    if (cropCb_) { cropCb_->Release(); cropCb_ = nullptr; }
    if (cursorCb_) { cursorCb_->Release(); cursorCb_ = nullptr; }
    if (menuCb_) { menuCb_->Release(); menuCb_ = nullptr; }
    if (menuSrv_) { menuSrv_->Release(); menuSrv_ = nullptr; }
    if (menuTex_) { menuTex_->Release(); menuTex_ = nullptr; }
    menuW_ = menuH_ = 0;
    if (context_) { context_->Release(); context_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
    if (overlayFont_) { DeleteObject(overlayFont_); overlayFont_ = nullptr; }
    overlayDpi_ = 0;
    swapChainFlags_ = 0;

    if (downSrv_) { downSrv_->Release(); downSrv_ = nullptr; }
    if (downRtv_) { downRtv_->Release(); downRtv_ = nullptr; }
    if (downTex_) { downTex_->Release(); downTex_ = nullptr; }
    downW_ = downH_ = 0;
    downDirty_ = true;
    renderResIndex_ = 0;

    debugReadbackFrames_ = 0;
    srcW_ = srcH_ = 0;
    srcFmt_ = DXGI_FORMAT_UNKNOWN;

    captureStatsBackend_ = CaptureBackendStats::None;
    dxgiProducedTotal_ = 0;
    dxgiLastAccumulated_ = 0;
    wgcArrivedTotal_ = 0;
    wgcProducedTotal_ = 0;
    wgcConsumedTotal_ = 0;

    rateQpf_ = {};
    rateLastQpc_ = {};
    ratePresentCount_ = 0;
    rateNewFrameCount_ = 0;
    presentFps_ = 0.0;
    newFrameFps_ = 0.0;

    rateLastDxgiProduced_ = 0;
    rateLastWgcArrived_ = 0;
    rateLastWgcProduced_ = 0;
    rateLastWgcConsumed_ = 0;
    dxgiProducedFps_ = 0.0;
    wgcArrivedFps_ = 0.0;
    wgcProducedFps_ = 0.0;
    wgcConsumedFps_ = 0.0;
}
