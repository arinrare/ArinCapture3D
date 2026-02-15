#pragma once
#include <windows.h>

#include <functional>

class DepthDialog {
public:
    // Shows a modal dialog to choose stereo settings.
    // - depthLevel: [1, 20]
    // - parallaxStrengthPercent: [0, 50]
    // - onPreview: called whenever the user drags a slider (and on Cancel to revert)
    // Returns true if the user pressed OK.
    bool Show(HWND hWndParent, int& depthLevel, int& parallaxStrengthPercent,
              const std::function<void(int depthLevel, int parallaxStrengthPercent)>& onPreview = {});

    // Shows a modeless dialog (non-blocking) so rendering can continue.
    // - onPreview: called whenever the user drags a slider (and on Cancel to revert)
    // - onDone: called when the dialog closes (OK/Cancel/close button)
    // Returns the dialog HWND, or nullptr on failure.
    HWND ShowModeless(
        HWND hWndParent,
        int initialDepthLevel,
        int initialParallaxStrengthPercent,
        const std::function<void(int depthLevel, int parallaxStrengthPercent)>& onPreview,
        const std::function<void(bool accepted, int depthLevel, int parallaxStrengthPercent)>& onDone);
};
