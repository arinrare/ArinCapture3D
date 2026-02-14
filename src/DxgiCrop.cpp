#include "DxgiCrop.h"
#include "Renderer.h"

namespace DxgiCrop {

static bool GetWindowClientRectScreen(HWND hwnd, RECT* outRect) {
    if (!outRect) return false;
    *outRect = {0, 0, 0, 0};
    if (!hwnd || !IsWindow(hwnd)) return false;
    RECT cr{};
    if (!GetClientRect(hwnd, &cr)) return false;
    POINT tl{cr.left, cr.top};
    POINT br{cr.right, cr.bottom};
    if (!ClientToScreen(hwnd, &tl) || !ClientToScreen(hwnd, &br)) return false;
    outRect->left = tl.x;
    outRect->top = tl.y;
    outRect->right = br.x;
    outRect->bottom = br.y;
    return (outRect->right > outRect->left && outRect->bottom > outRect->top);
}

bool UpdateDxgiWindowCropForRenderer(Renderer& renderer, const CropState& activeWindow, const CropState& windowSelect) {
    HWND cropTarget = nullptr;
    RECT cropMonitorRect = {0, 0, 0, 0};

    if (activeWindow.active) {
        cropTarget = activeWindow.target;
        cropMonitorRect = activeWindow.monitorRect;
    } else if (windowSelect.active) {
        cropTarget = windowSelect.target;
        cropMonitorRect = windowSelect.monitorRect;
    }

    if (!cropTarget || !IsWindow(cropTarget)) {
        renderer.ClearSourceCrop();
        return false;
    }

    RECT clientScreen{};
    if (!GetWindowClientRectScreen(cropTarget, &clientScreen)) {
        renderer.ClearSourceCrop();
        return false;
    }

    RECT inter{};
    if (!IntersectRect(&inter, &clientScreen, &cropMonitorRect)) {
        renderer.ClearSourceCrop();
        return false;
    }

    const int monW = cropMonitorRect.right - cropMonitorRect.left;
    const int monH = cropMonitorRect.bottom - cropMonitorRect.top;
    if (monW <= 0 || monH <= 0) {
        renderer.ClearSourceCrop();
        return false;
    }

    const float l = (float)(inter.left - cropMonitorRect.left) / (float)monW;
    const float t = (float)(inter.top - cropMonitorRect.top) / (float)monH;
    const float r = (float)(inter.right - cropMonitorRect.left) / (float)monW;
    const float b = (float)(inter.bottom - cropMonitorRect.top) / (float)monH;
    renderer.SetSourceCropNormalized(l, t, r, b);
    return true;
}

} // namespace DxgiCrop
