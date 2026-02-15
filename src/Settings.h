#pragma once

#include <string>

struct AppSettings {
    // Stereo
    bool stereoEnabled = false;
    int stereoDepthLevel = 10;              // [1,20]
    int stereoParallaxStrengthPercent = 20; // [0,50]

    // Output / presentation
    bool vsyncEnabled = true;
    bool clickThrough = false;
    // When enabled, draw a software cursor overlay into the output frame.
    // This is useful for headset/streaming capture paths that don't capture the OS hardware cursor.
    // Cursor position is tracked from the OS cursor over the captured source (no input forwarding).
    bool cursorOverlay = false;
    bool excludeFromCapture = true;
    int overlayPosIndex = 0;              // 0..4

    // Diagnostics overlay
    bool diagnosticsOverlay = false;
    int diagnosticsOverlaySizeIndex = 0;  // 0..2
    bool diagnosticsOverlayCompact = true;

    // Performance
    int framerateIndex = 0;               // 0..4
    int renderResPresetIndex = 0;         // 0..N

    static std::wstring GetSettingsPath();
    static AppSettings Load();
    void Save() const;
};
