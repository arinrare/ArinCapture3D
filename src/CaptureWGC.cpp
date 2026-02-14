#include "CaptureWGC.h"
#include "Log.h"

#include <windows.h>

// C++/WinRT
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// Interop helpers
#include <Windows.Graphics.Capture.Interop.h>
#include <Windows.Graphics.DirectX.Direct3D11.Interop.h>
#include <shobjidl_core.h> // IInitializeWithWindow

#include <objbase.h> // CoGetApartmentType

#include <unknwn.h>

#include <atomic>
#include <mutex>

// For IAsyncInfo::ErrorCode
#include <winrt/Windows.Foundation.h>

// Some SDKs don't expose this interop interface cleanly to C++/WinRT consumers.
// It's the canonical way to unwrap an IDirect3DSurface to a DXGI/D3D11 interface.
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1")) IDirect3DDxgiInterfaceAccess : ::IUnknown {
    virtual HRESULT __stdcall GetInterface(REFIID iid, void** p) = 0;
};

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

// Deeper queue helps reduce frame drops during spikes and when the app is briefly busy.
// Note: Even though we drain each tick, a bit of buffering improves robustness.
static constexpr int kFramePoolBufferCount = 6;

static void LogCaptureItemDetails(const winrt::Windows::Graphics::Capture::GraphicsCaptureItem& item) {
    try {
        winrt::Windows::Graphics::SizeInt32 size{ 0, 0 };
        if (item) {
            size = item.Size();
        }
        std::string name;
        try {
            name = winrt::to_string(item.DisplayName());
        } catch (...) {
            name = "(display name unavailable)";
        }
        Log::Info("CaptureWGC: Selected item '" + name + "' size=" + std::to_string((int)size.Width) + "x" + std::to_string((int)size.Height));
    } catch (...) {
        Log::Error("CaptureWGC: Failed to query selected item details");
    }
}

struct CaptureWGC::Impl {
    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    IDirect3DDevice d3dDeviceWinrt{ nullptr };

    GraphicsCaptureItem item{ nullptr };
    std::wstring pickedDisplayName;
    GraphicsCapturePicker picker{ nullptr };
    winrt::Windows::Foundation::IAsyncOperation<GraphicsCaptureItem> pickOp{ nullptr };
    HWND notifyHwnd = nullptr;
    bool pickInProgress = false;
    Direct3D11CaptureFramePool framePool{ nullptr };
    GraphicsCaptureSession session{ nullptr };

    winrt::event_token frameArrivedToken{};
    std::atomic<unsigned long long> frameArrivedCount{ 0 };
    std::atomic<unsigned long long> frameProducedCount{ 0 };
    std::atomic<unsigned long long> frameConsumedCount{ 0 };
    std::mutex pendingMutex;
    Direct3D11CaptureFrame pendingFrame{ nullptr };

    HANDLE frameEvent = nullptr; // auto-reset event signaled when a new frame is buffered

    Direct3D11CaptureFrame currentFrame{ nullptr };
    bool frameHeld = false;

    HWND targetHwnd = nullptr;

    UINT originalClientW = 0;
    UINT originalClientH = 0;
    bool hasOriginalClientSize = false;

    int width = 0;
    int height = 0;

    bool EnsureD3DDevice() {
        if (d3dDevice && d3dContext && d3dDeviceWinrt) return true;

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &d3dDevice,
            nullptr,
            &d3dContext);

        if (FAILED(hr)) {
            Log::Error("CaptureWGC: D3D11CreateDevice failed with debug flags; retrying without debug");
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                flags,
                nullptr,
                0,
                D3D11_SDK_VERSION,
                &d3dDevice,
                nullptr,
                &d3dContext);
        }

        if (FAILED(hr) || !d3dDevice || !d3dContext) {
            Log::Error("CaptureWGC: D3D11CreateDevice failed");
            return false;
        }

        // Wrap as WinRT IDirect3DDevice
        com_ptr<IDXGIDevice> dxgiDevice;
        hr = d3dDevice->QueryInterface(__uuidof(IDXGIDevice), dxgiDevice.put_void());
        if (FAILED(hr) || !dxgiDevice) {
            Log::Error("CaptureWGC: QueryInterface(IDXGIDevice) failed");
            return false;
        }

        com_ptr<::IInspectable> inspectable;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put());
        if (FAILED(hr) || !inspectable) {
            Log::Error("CaptureWGC: CreateDirect3D11DeviceFromDXGIDevice failed");
            return false;
        }

        d3dDeviceWinrt = inspectable.as<IDirect3DDevice>();
        return (bool)d3dDeviceWinrt;
    }

    void StopSession() {
        // Best-effort cancel any in-flight picker operation.
        try {
            if (pickOp) {
                pickOp.Cancel();
            }
        } catch (...) {
        }
        pickOp = nullptr;
        picker = nullptr;
        notifyHwnd = nullptr;
        pickInProgress = false;
        if (frameHeld) {
            currentFrame = nullptr;
            frameHeld = false;
        }
        if (session) {
            session.Close();
            session = nullptr;
        }
        if (framePool) {
            try {
                if (frameArrivedToken.value) {
                    framePool.FrameArrived(frameArrivedToken);
                }
            } catch (...) {
            }
            framePool.Close();
            framePool = nullptr;
        }
        frameArrivedToken = {};
        frameArrivedCount.store(0, std::memory_order_relaxed);
        frameProducedCount.store(0, std::memory_order_relaxed);
        frameConsumedCount.store(0, std::memory_order_relaxed);
        if (frameEvent) {
            ResetEvent(frameEvent);
        }
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            pendingFrame = nullptr;
        }
        item = nullptr;
        pickedDisplayName.clear();
        targetHwnd = nullptr;
        originalClientW = 0;
        originalClientH = 0;
        hasOriginalClientSize = false;
        width = 0;
        height = 0;
    }
};

CaptureWGC::CaptureWGC() : impl_(std::make_unique<Impl>()) {
    if (impl_) {
        // Auto-reset, initially non-signaled.
        impl_->frameEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!impl_->frameEvent) {
            Log::Error("CaptureWGC: CreateEvent failed");
        }
    }
}
CaptureWGC::~CaptureWGC() { Cleanup(); }

bool CaptureWGC::InitPicker(HWND parentWindow) {
    if (!impl_) return false;

    if (!parentWindow || !IsWindow(parentWindow)) {
        Log::Error("CaptureWGC: InitPicker failed: invalid parent window");
        return false;
    }

    if (!GraphicsCaptureSession::IsSupported()) {
        Log::Error("CaptureWGC: Windows Graphics Capture is not supported on this OS");
        return false;
    }

    impl_->StopSession();

    if (!impl_->EnsureD3DDevice()) {
        return false;
    }

    impl_->notifyHwnd = parentWindow;

    {
        APTTYPE aptType = APTTYPE_CURRENT;
        APTTYPEQUALIFIER aptQualifier = APTTYPEQUALIFIER_NONE;
        const HRESULT hrApt = CoGetApartmentType(&aptType, &aptQualifier);
        if (SUCCEEDED(hrApt)) {
            Log::Info(
                "CaptureWGC: InitPicker apartment type=" + std::to_string((int)aptType) +
                " qualifier=" + std::to_string((int)aptQualifier)
            );
        }
    }

    try {
        impl_->picker = GraphicsCapturePicker();
        auto init = impl_->picker.as<IInitializeWithWindow>();
        const HRESULT hrInit = init->Initialize(parentWindow);
        if (FAILED(hrInit)) {
            char buf[64] = {};
            std::snprintf(buf, sizeof(buf), "0x%08X", (unsigned)hrInit);
            Log::Error(std::string("CaptureWGC: IInitializeWithWindow::Initialize failed hr=") + buf);
            impl_->picker = nullptr;
            return false;
        }

        Log::Info("CaptureWGC: Opening window picker...");
        Log::Info("CaptureWGC: InitPicker thread id: " + std::to_string((int)GetCurrentThreadId()));
        // IMPORTANT: don't call .get() on an STA thread; C++/WinRT asserts to avoid deadlocks.
        impl_->pickInProgress = true;
        impl_->pickOp = impl_->picker.PickSingleItemAsync();
    } catch (const winrt::hresult_error& e) {
        char buf[64] = {};
        std::snprintf(buf, sizeof(buf), "0x%08X", (unsigned)e.code().value);
        Log::Error(std::string("CaptureWGC: InitPicker threw hresult_error hr=") + buf + " msg='" + winrt::to_string(e.message()) + "'");
        impl_->pickInProgress = false;
        impl_->pickOp = nullptr;
        impl_->picker = nullptr;
        return false;
    } catch (...) {
        Log::Error("CaptureWGC: InitPicker threw unknown exception");
        impl_->pickInProgress = false;
        impl_->pickOp = nullptr;
        impl_->picker = nullptr;
        return false;
    }

    if (!impl_->pickOp) {
        Log::Error("CaptureWGC: PickSingleItemAsync returned null operation");
        impl_->pickInProgress = false;
        impl_->picker = nullptr;
        return false;
    }

    impl_->pickOp.Completed([this](auto const& op, winrt::Windows::Foundation::AsyncStatus status) {
        const DWORD completedTid = GetCurrentThreadId();
        // If the app canceled/cleaned up while the picker was open, do nothing.
        if (!impl_ || !impl_->pickInProgress) {
            return;
        }
        bool ok = false;

        // Log completion status and (when applicable) error code.
        try {
            Log::Info("CaptureWGC: Picker completed (status=" + std::to_string((int)status) + ")");
            if (status == winrt::Windows::Foundation::AsyncStatus::Error) {
                auto info = op.as<winrt::Windows::Foundation::IAsyncInfo>();
                const winrt::hresult ec = info.ErrorCode();
                char buf[64] = {};
                std::snprintf(buf, sizeof(buf), "0x%08X", (unsigned)ec.value);
                Log::Error(std::string("CaptureWGC: Picker async error code hr=") + buf);
            }
        } catch (...) {
            // Best-effort only; never let logging throw.
        }

        try {
            if (status == winrt::Windows::Foundation::AsyncStatus::Completed) {
                auto item = op.GetResults();
                if (item) {
                    impl_->item = item;
                    impl_->width = item.Size().Width;
                    impl_->height = item.Size().Height;

                    try {
                        impl_->pickedDisplayName = item.DisplayName().c_str();
                    } catch (...) {
                        impl_->pickedDisplayName.clear();
                    }

                    LogCaptureItemDetails(item);
                    Log::Info("CaptureWGC: Selected item size: " + std::to_string(impl_->width) + "x" + std::to_string(impl_->height));
                    Log::Info("CaptureWGC: Picker completion thread id: " + std::to_string((int)completedTid));

                    // IMPORTANT: Do not create the frame pool/session here.
                    // This completion callback can run on a threadpool thread, which would bind WinRT/COM
                    // objects to the wrong apartment. The app finalizes capture on the UI thread in response
                    // to WM_APP+5 by calling StartCaptureFromPickedItem().
                    ok = true;
                } else {
                    Log::Error("CaptureWGC: No window selected");
                }
            } else {
                Log::Error("CaptureWGC: Picker did not complete successfully (status=" + std::to_string((int)status) + ")");
            }
        } catch (const winrt::hresult_error& e) {
            char buf[64] = {};
            std::snprintf(buf, sizeof(buf), "0x%08X", (unsigned)e.code().value);
            Log::Error(std::string("CaptureWGC: Picker completion threw hresult_error hr=") + buf + " msg='" + winrt::to_string(e.message()) + "'");
        } catch (...) {
            Log::Error("CaptureWGC: Exception during picker completion");
        }

        impl_->pickInProgress = false;
        if (impl_->notifyHwnd) {
            // Notify app: WM_APP+5, wParam=1 success / 0 failure
            PostMessage(impl_->notifyHwnd, WM_APP + 5, ok ? 1 : 0, 0);
        }
    });

    return true;
}

std::wstring CaptureWGC::GetPickedItemDisplayName() const {
    if (!impl_) return L"";
    return impl_->pickedDisplayName;
}

bool CaptureWGC::GetCaptureItemSize(UINT* outWidth, UINT* outHeight) const {
    if (outWidth) *outWidth = 0;
    if (outHeight) *outHeight = 0;
    if (!impl_) return false;
    if (impl_->width <= 0 || impl_->height <= 0) return false;
    if (outWidth) *outWidth = (UINT)impl_->width;
    if (outHeight) *outHeight = (UINT)impl_->height;
    return true;
}

bool CaptureWGC::StartCaptureFromPickedItem() {
    if (!impl_) return false;
    if (!impl_->item) {
        Log::Error("CaptureWGC::StartCaptureFromPickedItem: no item selected");
        return false;
    }
    if (!impl_->EnsureD3DDevice()) {
        return false;
    }

    // Tear down any prior session/frame pool.
    if (impl_->frameHeld) {
        impl_->currentFrame = nullptr;
        impl_->frameHeld = false;
    }
    if (impl_->session) {
        impl_->session.Close();
        impl_->session = nullptr;
    }
    if (impl_->framePool) {
        impl_->framePool.Close();
        impl_->framePool = nullptr;
    }

    impl_->frameArrivedToken = {};
    impl_->frameArrivedCount.store(0, std::memory_order_relaxed);
    impl_->frameProducedCount.store(0, std::memory_order_relaxed);
    impl_->frameConsumedCount.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(impl_->pendingMutex);
        impl_->pendingFrame = nullptr;
    }

    try {
        Log::Info("CaptureWGC: StartCaptureFromPickedItem thread id: " + std::to_string((int)GetCurrentThreadId()));
        auto size = impl_->item.Size();
        impl_->width = size.Width;
        impl_->height = size.Height;

        LogCaptureItemDetails(impl_->item);

        // Prefer free-threaded frame pool when available.
        try {
            impl_->framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
                impl_->d3dDeviceWinrt,
                DirectXPixelFormat::B8G8R8A8UIntNormalized,
                kFramePoolBufferCount,
                size);
            Log::Info("CaptureWGC: Using CreateFreeThreaded frame pool");
        } catch (...) {
            impl_->framePool = Direct3D11CaptureFramePool::Create(
                impl_->d3dDeviceWinrt,
                DirectXPixelFormat::B8G8R8A8UIntNormalized,
                kFramePoolBufferCount,
                size);
            Log::Info("CaptureWGC: Using Create (apartment) frame pool");
        }

        impl_->frameArrivedToken = impl_->framePool.FrameArrived([this](auto const& /*pool*/, auto const&) {
            const unsigned long long n = impl_->frameArrivedCount.fetch_add(1ULL, std::memory_order_relaxed) + 1ULL;
            // IMPORTANT: do not drain the pool here.
            // FrameArrived callbacks can be coalesced/throttled depending on the apartment/threading model,
            // and draining in the callback can cause dropped frames when the buffer count is small.
            // We drain from GetFrame() on the render tick instead.
            if (impl_->frameEvent) {
                SetEvent(impl_->frameEvent);
            }
            if ((n % 600ULL) == 0ULL) {
                Log::Info("CaptureWGC: FrameArrived count=" + std::to_string(n));
            }
        });

        impl_->session = impl_->framePool.CreateCaptureSession(impl_->item);
        try {
            impl_->session.IsBorderRequired(false);
        } catch (...) {
        }
        // Cursor capture can add overhead; we don't need it for this app.
        try {
            impl_->session.IsCursorCaptureEnabled(false);
        } catch (...) {
        }
        impl_->session.StartCapture();
        Log::Info("CaptureWGC: Capture session started (UI thread)");
        return true;
    } catch (...) {
        Log::Error("CaptureWGC::StartCaptureFromPickedItem: exception while creating capture session");
        return false;
    }
}

bool CaptureWGC::StartCaptureFromWindow(HWND targetWindow) {
    if (!impl_) return false;
    if (!targetWindow || !IsWindow(targetWindow)) {
        Log::Error("CaptureWGC::StartCaptureFromWindow: invalid HWND");
        return false;
    }

    if (!GraphicsCaptureSession::IsSupported()) {
        Log::Error("CaptureWGC: Windows Graphics Capture is not supported on this OS");
        return false;
    }

    impl_->StopSession();

    if (!impl_->EnsureD3DDevice()) {
        return false;
    }

    try {
        auto interopFactory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        GraphicsCaptureItem item{ nullptr };
        HRESULT hr = interopFactory->CreateForWindow(targetWindow, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item));
        if (FAILED(hr) || !item) {
            Log::Error("CaptureWGC::StartCaptureFromWindow: CreateForWindow failed");
            return false;
        }

        impl_->item = item;
        impl_->targetHwnd = targetWindow;

        // Capture the original client size so we can restore it when selecting "Native".
        RECT cr{};
        if (GetClientRect(targetWindow, &cr)) {
            const UINT cw = (UINT)std::max<LONG>(0, cr.right - cr.left);
            const UINT ch = (UINT)std::max<LONG>(0, cr.bottom - cr.top);
            impl_->originalClientW = cw;
            impl_->originalClientH = ch;
            impl_->hasOriginalClientSize = (cw > 0 && ch > 0);
            Log::Info("CaptureWGC: Original target client size " + std::to_string((int)cw) + "x" + std::to_string((int)ch));
        } else {
            impl_->originalClientW = 0;
            impl_->originalClientH = 0;
            impl_->hasOriginalClientSize = false;
        }

        return StartCaptureFromPickedItem();
    } catch (...) {
        Log::Error("CaptureWGC::StartCaptureFromWindow: exception creating capture item");
        return false;
    }
}

bool CaptureWGC::HasTargetWindow() const {
    return impl_ && impl_->targetHwnd && IsWindow(impl_->targetHwnd);
}

bool CaptureWGC::ResizeTargetWindowClient(UINT clientWidth, UINT clientHeight) {
    if (!HasTargetWindow()) {
        Log::Error("CaptureWGC::ResizeTargetWindowClient: no target HWND available (use active-window capture)");
        return false;
    }

    HWND hwnd = impl_->targetHwnd;

    if (clientWidth == 0 || clientHeight == 0) {
        if (!impl_->hasOriginalClientSize) {
            Log::Error("CaptureWGC::ResizeTargetWindowClient: no original size recorded to restore");
            return false;
        }
        clientWidth = impl_->originalClientW;
        clientHeight = impl_->originalClientH;
        Log::Info("CaptureWGC: Restoring target client size to " + std::to_string((int)clientWidth) + "x" + std::to_string((int)clientHeight));
    }

    if (clientWidth == 0 || clientHeight == 0) return false;

    RECT cr{ 0, 0, (LONG)clientWidth, (LONG)clientHeight };
    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (!AdjustWindowRectEx(&cr, (DWORD)style, FALSE, (DWORD)exStyle)) {
        Log::Error("CaptureWGC::ResizeTargetWindowClient: AdjustWindowRectEx failed");
        return false;
    }

    const int w = cr.right - cr.left;
    const int h = cr.bottom - cr.top;
    if (w <= 0 || h <= 0) return false;

    BOOL ok = SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (!ok) {
        Log::Error("CaptureWGC::ResizeTargetWindowClient: SetWindowPos failed");
        return false;
    }

    RECT after{};
    int gotW = 0, gotH = 0;
    if (GetClientRect(hwnd, &after)) {
        gotW = (int)(after.right - after.left);
        gotH = (int)(after.bottom - after.top);
    }

    Log::Info(
        "CaptureWGC: Requested target client size " + std::to_string((int)clientWidth) + "x" + std::to_string((int)clientHeight) +
        ", now " + std::to_string(gotW) + "x" + std::to_string(gotH)
    );

    if (gotW > 0 && gotH > 0 && ((UINT)gotW != clientWidth || (UINT)gotH != clientHeight)) {
        Log::Error("CaptureWGC: Target window did not accept requested client size (game may be forcing its own size)");
    }
    return true;
}

bool CaptureWGC::GetFrame(ID3D11Texture2D** outTex, INT64* outTimestamp) {
    static int s_noFrameCount = 0;
    if (!outTex) return false;
    *outTex = nullptr;

    if (!impl_ || !impl_->framePool) {
        Log::Error("CaptureWGC::GetFrame: not initialized");
        return false;
    }

    if (impl_->frameHeld) {
        // Caller forgot to ReleaseFrame(). Don't allow frame queue to stall.
        impl_->currentFrame = nullptr;
        impl_->frameHeld = false;
        Log::Error("CaptureWGC::GetFrame: frame was still held; auto-released previous frame");
    }

    // Drain the pool and keep the most recent frame.
    // This keeps capture aligned with our render tick and avoids relying on FrameArrived callback frequency.
    Direct3D11CaptureFrame frame{ nullptr };
    unsigned drained = 0;
    try {
        for (;;) {
            auto f = impl_->framePool.TryGetNextFrame();
            if (!f) break;
            frame = f;
            drained++;
        }
    } catch (...) {
        Log::Error("CaptureWGC::GetFrame: exception from TryGetNextFrame (thread/apartment mismatch?)");
        return false;
    }

    if (drained > 0) {
        impl_->frameProducedCount.fetch_add((unsigned long long)drained, std::memory_order_relaxed);
    }

    // IMPORTANT: Do not block here.
    // GetFrame is called from the UI thread. Waiting, even briefly,
    // can stall message pumping and manifest as the "spinner" / no-input behavior.

    if (!frame) {
        s_noFrameCount++;
        if (s_noFrameCount == 1) {
            LogCaptureItemDetails(impl_->item);
        }
        if (s_noFrameCount == 1 || (s_noFrameCount % 120) == 0) {
            const unsigned long long arrived = impl_->frameArrivedCount.load(std::memory_order_relaxed);
            Log::Info("CaptureWGC::GetFrame: no new frame available (count=" + std::to_string(s_noFrameCount) + ", FrameArrived=" + std::to_string(arrived) + ")");
        }
        return false;
    }

    // If the captured content size changes (e.g., target window resized), the frame pool must be recreated.
    try {
        auto cs = frame.ContentSize();
        if (cs.Width > 0 && cs.Height > 0 && (cs.Width != impl_->width || cs.Height != impl_->height)) {
            Log::Info("CaptureWGC: ContentSize changed from " + std::to_string(impl_->width) + "x" + std::to_string(impl_->height) +
                " to " + std::to_string(cs.Width) + "x" + std::to_string(cs.Height) + " (recreating frame pool)");
            impl_->width = cs.Width;
            impl_->height = cs.Height;
            impl_->framePool.Recreate(
                impl_->d3dDeviceWinrt,
                DirectXPixelFormat::B8G8R8A8UIntNormalized,
                kFramePoolBufferCount,
                cs);
            // Drop any buffered pending frame from the old size.
            std::lock_guard<std::mutex> lock(impl_->pendingMutex);
            impl_->pendingFrame = nullptr;
        }
    } catch (...) {
    }

    // Reset streak when we finally get a frame.
    s_noFrameCount = 0;

    // We now drain directly from the pool each tick; ensure no stale pending frame lingers.
    {
        std::lock_guard<std::mutex> lock(impl_->pendingMutex);
        impl_->pendingFrame = nullptr;
    }

    // Handle resize
    auto cs = frame.ContentSize();
    if (cs.Width != impl_->width || cs.Height != impl_->height) {
        impl_->width = cs.Width;
        impl_->height = cs.Height;
        Log::Info("CaptureWGC: Content resized to " + std::to_string(impl_->width) + "x" + std::to_string(impl_->height) + ", recreating frame pool");
        impl_->framePool.Recreate(
            impl_->d3dDeviceWinrt,
            DirectXPixelFormat::B8G8R8A8UIntNormalized,
            kFramePoolBufferCount,
            cs);
    }

    // Convert surface to ID3D11Texture2D
    auto surface = frame.Surface();
    com_ptr<::IUnknown> surfaceUnknown = surface.as<::IUnknown>();
    com_ptr<IDirect3DDxgiInterfaceAccess> access;
    HRESULT hr = surfaceUnknown->QueryInterface(__uuidof(IDirect3DDxgiInterfaceAccess), access.put_void());
    if (FAILED(hr) || !access) {
        Log::Error("CaptureWGC::GetFrame: QueryInterface(IDirect3DDxgiInterfaceAccess) failed");
        return false;
    }

    com_ptr<ID3D11Texture2D> tex;
    hr = access->GetInterface(__uuidof(ID3D11Texture2D), tex.put_void());
    if (FAILED(hr) || !tex) {
        Log::Error("CaptureWGC::GetFrame: IDirect3DDxgiInterfaceAccess::GetInterface(ID3D11Texture2D) failed");
        return false;
    }

    if (outTimestamp) *outTimestamp = frame.SystemRelativeTime().count();
    impl_->currentFrame = frame;
    impl_->frameHeld = true;
    impl_->frameConsumedCount.fetch_add(1ULL, std::memory_order_relaxed);

    *outTex = tex.detach();
    return true;
}

unsigned long long CaptureWGC::GetFrameArrivedCount() const {
    if (!impl_) return 0ULL;
    return impl_->frameArrivedCount.load(std::memory_order_relaxed);
}

unsigned long long CaptureWGC::GetFrameProducedCount() const {
    if (!impl_) return 0ULL;
    return impl_->frameProducedCount.load(std::memory_order_relaxed);
}

unsigned long long CaptureWGC::GetFrameConsumedCount() const {
    if (!impl_) return 0ULL;
    return impl_->frameConsumedCount.load(std::memory_order_relaxed);
}

void CaptureWGC::ReleaseFrame() {
    if (!impl_) return;
    if (!impl_->frameHeld) return;
    impl_->currentFrame = nullptr;
    impl_->frameHeld = false;
}

void CaptureWGC::Cleanup() {
    if (!impl_) return;

    impl_->StopSession();

    if (impl_->frameEvent) {
        CloseHandle(impl_->frameEvent);
        impl_->frameEvent = nullptr;
    }

    if (impl_->d3dContext) {
        impl_->d3dContext->Release();
        impl_->d3dContext = nullptr;
    }
    if (impl_->d3dDevice) {
        impl_->d3dDevice->Release();
        impl_->d3dDevice = nullptr;
    }
    impl_->d3dDeviceWinrt = nullptr;
}

ID3D11Device* CaptureWGC::GetDevice() const {
    return impl_ ? impl_->d3dDevice : nullptr;
}

ID3D11DeviceContext* CaptureWGC::GetContext() const {
    return impl_ ? impl_->d3dContext : nullptr;
}

HWND CaptureWGC::GetCapturedWindow() const {
    return impl_ ? impl_->targetHwnd : nullptr;
}
