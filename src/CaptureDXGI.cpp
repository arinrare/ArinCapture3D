#include <string>
#include <windows.h>

// Helper to convert wstring to string (UTF-8)
static std::string WStringToString(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &strTo[0], size_needed, nullptr, nullptr);
    return strTo;
}
#include "CaptureDXGI.h"
#include "Log.h"

bool CaptureDXGI::Init(const wchar_t* targetDeviceName) {
    Log::Info("CaptureDXGI::Init called");
    // Release any previous resources
    Cleanup();

    outputDeviceName_.clear();

    HRESULT hr;
    IDXGIFactory1* dxgiFactory = nullptr;
    Log::Info("CaptureDXGI::Init: Cleanup");
    hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&dxgiFactory);
    if (FAILED(hr)) { Log::Error("CreateDXGIFactory1 failed"); return false; }

    // Enumerate adapters and outputs, log device names, and select by device name
    IDXGIAdapter1* selectedAdapter = nullptr;
    IDXGIOutput* selectedOutput = nullptr;
    IDXGIOutput1* selectedOutput1 = nullptr;
    bool found = false;
    const bool hasTarget = (targetDeviceName && targetDeviceName[0] != L'\0');
    for (UINT adapterIdx = 0; !found && dxgiFactory->EnumAdapters1(adapterIdx, &selectedAdapter) != DXGI_ERROR_NOT_FOUND; ++adapterIdx) {
        for (UINT outputIdx = 0; selectedAdapter->EnumOutputs(outputIdx, &selectedOutput) != DXGI_ERROR_NOT_FOUND; ++outputIdx) {
            DXGI_OUTPUT_DESC desc;
            selectedOutput->GetDesc(&desc);
            std::wstring ws(desc.DeviceName);
            Log::Info("Found output " + std::to_string(outputIdx) + ": " + WStringToString(ws) + " (len=" + std::to_string(wcslen(desc.DeviceName)) + ")");
            if (hasTarget) {
                Log::Info("Target device name: " + WStringToString(targetDeviceName) + " (len=" + std::to_string(wcslen(targetDeviceName)) + ")");
            }

            const bool matches = !hasTarget || (wcsncmp(desc.DeviceName, targetDeviceName, wcslen(targetDeviceName)) == 0);
            if (matches) {
                HRESULT qhr = selectedOutput->QueryInterface(__uuidof(IDXGIOutput1), (void**)&selectedOutput1);
                if (SUCCEEDED(qhr)) {
                    adapter_ = selectedAdapter;
                    output1_ = selectedOutput1;
                    outputDeviceName_ = desc.DeviceName;
                    found = true;
                    Log::Info("Selected output for capture: " + WStringToString(ws) + " (len=" + std::to_string(wcslen(desc.DeviceName)) + ")");
                    break;
                }
            }
            selectedOutput->Release();
        }
        if (!found) selectedAdapter->Release();
    }
    if (!found) {
        Log::Error("Target monitor not found by device name");
        dxgiFactory->Release();
        return false;
    }

    // Create D3D11 device
    Log::Info("CaptureDXGI::Init: EnumAdapters1");
    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    hr = D3D11CreateDevice(adapter_, D3D_DRIVER_TYPE_UNKNOWN, nullptr, createFlags, nullptr, 0,
        D3D11_SDK_VERSION, &d3dDevice_, nullptr, &d3dContext_);
    if (FAILED(hr)) {
        Log::Error("D3D11CreateDevice failed with debug flags; retrying without D3D11_CREATE_DEVICE_DEBUG");
        createFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(adapter_, D3D_DRIVER_TYPE_UNKNOWN, nullptr, createFlags, nullptr, 0,
            D3D11_SDK_VERSION, &d3dDevice_, nullptr, &d3dContext_);
    }
    if (FAILED(hr)) { Log::Error("D3D11CreateDevice failed"); dxgiFactory->Release(); return false; }

    // Create duplication
    Log::Info("CaptureDXGI::Init: DuplicateOutput");
    hr = output1_->DuplicateOutput(d3dDevice_, &duplication_);
    if (FAILED(hr)) { Log::Error("DuplicateOutput failed"); dxgiFactory->Release(); return false; }

    duplication_->GetDesc(&duplDesc_);
    dxgiFactory->Release();
    Log::Info("DXGI capture initialized successfully.");
    return true;
}

bool CaptureDXGI::GetFrame(ID3D11Texture2D** outTex, INT64* outTimestamp) {
    if (!duplication_) {
        Log::Error("GetFrame: duplication_ is null");
        lastAcquireHr_ = E_FAIL;
        return false;
    }

    if (frameHeld_) {
        Log::Error("GetFrame called while a frame is still held; auto-releasing previous frame");
        duplication_->ReleaseFrame();
        frameHeld_ = false;
    }

    IDXGIResource* desktopResource = nullptr;
    DXGI_OUTDUPL_FRAME_INFO frameInfo = {};

    // Non-blocking acquire: the render loop is timer-driven, so we don't want to stall the UI thread.
    // If no new frame is available, AcquireNextFrame returns DXGI_ERROR_WAIT_TIMEOUT.
    // Small wait helps avoid phase-miss artifacts from purely non-blocking polling.
    // ~8ms keeps the UI responsive but greatly improves likelihood of grabbing a fresh frame at 60Hz.
    HRESULT hr = duplication_->AcquireNextFrame(8, &frameInfo, &desktopResource);
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            lastAcquireHr_ = hr;
            // Normal: no new frame ready yet.
#if defined(_DEBUG)
            static unsigned s_timeoutCount = 0;
            if ((++s_timeoutCount % 600u) == 0u) {
                Log::Info("CaptureDXGI: no new frame (WAIT_TIMEOUT) count=" + std::to_string(s_timeoutCount));
            }
#endif
            return false;
        }

        lastAcquireHr_ = hr;

        std::string hrMeaning;
        switch (hr) {
            case DXGI_ERROR_ACCESS_LOST: hrMeaning = "DXGI_ERROR_ACCESS_LOST"; break;
            case DXGI_ERROR_INVALID_CALL: hrMeaning = "DXGI_ERROR_INVALID_CALL"; break;
            case DXGI_ERROR_ACCESS_DENIED: hrMeaning = "DXGI_ERROR_ACCESS_DENIED"; break;
            default: hrMeaning = "Unknown"; break;
        }
        static unsigned s_otherFailCount = 0;
        if (s_otherFailCount++ < 5u || (s_otherFailCount % 120u) == 0u) {
            Log::Error("AcquireNextFrame failed, HRESULT: " + std::to_string(hr) + " (" + hrMeaning + ")");
        }
        return false;
    }
    lastAcquireHr_ = S_OK;
    if (outTimestamp) *outTimestamp = frameInfo.LastPresentTime.QuadPart;

    // Diagnostics: AccumulatedFrames tells us how many frames were produced since the last acquire.
    // In practice this is usually 1 when we're keeping up; >1 means we're falling behind.
    lastAccumulatedFrames_ = frameInfo.AccumulatedFrames;
    producedFramesTotal_ += (frameInfo.AccumulatedFrames > 0) ? (unsigned long long)frameInfo.AccumulatedFrames : 1ULL;
    // Avoid per-frame logging here: it is extremely expensive and can tank capture performance.
    // If you need to debug frame timing, enable a small, throttled log in debug builds.
#if defined(_DEBUG)
    static int s_loggedFrames = 0;
    if (s_loggedFrames++ < 2) {
        Log::Info("AcquireNextFrame ok. LastPresentTime=" + std::to_string(frameInfo.LastPresentTime.QuadPart) +
            ", AccumulatedFrames=" + std::to_string(frameInfo.AccumulatedFrames));
    }
#endif

    ID3D11Texture2D* frame = nullptr;
    hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&frame);
    desktopResource->Release();
    if (FAILED(hr)) {
        Log::Error("QueryInterface for ID3D11Texture2D failed, HRESULT: " + std::to_string(hr));
        return false;
    }

    // NOTE: Intentionally no per-frame staging readback / pixel logging.
    // Doing Map() on a staging texture every frame forces GPU/CPU synchronization and will destroy FPS.

    // IMPORTANT: The texture is only guaranteed valid until ReleaseFrame() is called.
    frameHeld_ = true;
    *outTex = frame; // caller must Release() AND call ReleaseFrame() once done with the frame.
    return true;
}

void CaptureDXGI::ReleaseFrame() {
    if (!duplication_) return;
    if (!frameHeld_) return;
    duplication_->ReleaseFrame();
    frameHeld_ = false;
}

void CaptureDXGI::Cleanup() {
    if (duplication_ && frameHeld_) {
        duplication_->ReleaseFrame();
        frameHeld_ = false;
    }
    if (duplication_) { duplication_->Release(); duplication_ = nullptr; }
    if (output1_) { output1_->Release(); output1_ = nullptr; }
    if (adapter_) { adapter_->Release(); adapter_ = nullptr; }
    if (d3dContext_) { d3dContext_->Release(); d3dContext_ = nullptr; }
    if (d3dDevice_) { d3dDevice_->Release(); d3dDevice_ = nullptr; }

    outputDeviceName_.clear();
}
