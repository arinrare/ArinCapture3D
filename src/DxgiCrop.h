#pragma once

#include <windows.h>

class Renderer;

namespace DxgiCrop {

struct CropState {
    bool active = false;
    HWND target = nullptr;
    RECT monitorRect = {0, 0, 0, 0};
};

// Applies the crop (normalized UVs) to `renderer` based on the first active crop state.
// Returns true if a crop was applied; false if crop was cleared / inactive.
bool UpdateDxgiWindowCropForRenderer(Renderer& renderer, const CropState& activeWindow, const CropState& windowSelect);

} // namespace DxgiCrop
