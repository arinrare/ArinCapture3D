#include "Settings.h"

#include <windows.h>
#include <shlobj.h>

#include "Log.h"

#pragma comment(lib, "Shell32.lib")

namespace {

static std::string WideToUtf8Local(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string out;
    out.resize((size_t)len);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), len, nullptr, nullptr);
    return out;
}

static int ClampInt(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool EnsureDirExists(const std::wstring& dir) {
    if (dir.empty()) return false;
    const DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;

    // CreateDirectoryW only creates one segment; walk up.
    size_t pos = 0;
    while (true) {
        pos = dir.find(L'\\', pos);
        std::wstring part = (pos == std::wstring::npos) ? dir : dir.substr(0, pos);
        if (!part.empty() && part.back() != L':') {
            CreateDirectoryW(part.c_str(), nullptr);
        }
        if (pos == std::wstring::npos) break;
        ++pos;
    }

    const DWORD attr2 = GetFileAttributesW(dir.c_str());
    return (attr2 != INVALID_FILE_ATTRIBUTES && (attr2 & FILE_ATTRIBUTE_DIRECTORY));
}

static std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";

    std::wstring path(buf);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    return path.substr(0, slash);
}

static std::wstring GetAppDataDir() {
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p) {
        std::wstring base(p);
        CoTaskMemFree(p);
        return base + L"\\ArinCapture";
    }
    return L"";
}

static void WriteInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int v) {
    wchar_t buf[32];
    wsprintfW(buf, L"%d", v);
    WritePrivateProfileStringW(section, key, buf, path.c_str());
}

static void WriteBool(const std::wstring& path, const wchar_t* section, const wchar_t* key, bool v) {
    WritePrivateProfileStringW(section, key, v ? L"1" : L"0", path.c_str());
}

static bool TryReadInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int* outV) {
    if (!outV) return false;
    wchar_t buf[64] = {};
    const DWORD n = GetPrivateProfileStringW(section, key, L"", buf, (DWORD)(sizeof(buf) / sizeof(buf[0])), path.c_str());
    if (n == 0) return false;
    *outV = _wtoi(buf);
    return true;
}

static bool FileExists(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) return false;
    return true;
}

} // namespace

std::wstring AppSettings::GetSettingsPath() {
    std::wstring dir = GetAppDataDir();
    if (!dir.empty() && EnsureDirExists(dir)) {
        return dir + L"\\settings.ini";
    }

    // Fallback: next to executable.
    std::wstring exeDir = GetExeDir();
    if (!exeDir.empty()) {
        return exeDir + L"\\settings.ini";
    }

    return L"settings.ini";
}

AppSettings AppSettings::Load() {
    AppSettings s;
    const std::wstring path = GetSettingsPath();

    // First-run defaults (when there is no settings.ini yet).
    const bool firstRun = !FileExists(path);
    if (firstRun) {
        s.stereoDepthLevel = 10;
        s.stereoParallaxStrengthPercent = 20;
    }

    s.stereoEnabled = (GetPrivateProfileIntW(L"Stereo", L"Enabled", s.stereoEnabled ? 1 : 0, path.c_str()) != 0);
    s.stereoDepthLevel = ClampInt((int)GetPrivateProfileIntW(L"Stereo", L"DepthLevel", s.stereoDepthLevel, path.c_str()), 1, 20);
    {
        int v = s.stereoParallaxStrengthPercent;
        if (!TryReadInt(path, L"Stereo", L"ParallaxStrengthPercent", &v)) {
            // Backward-compat: older builds used ParallaxPercent for a convergence offset.
            // That behavior is intentionally removed; default to a neutral layering strength.
            if (!firstRun) {
                v = 50;
            }
        }
        s.stereoParallaxStrengthPercent = ClampInt(v, 0, 50);
    }

    s.vsyncEnabled = (GetPrivateProfileIntW(L"Output", L"VSyncEnabled", s.vsyncEnabled ? 1 : 0, path.c_str()) != 0);
    s.clickThrough = (GetPrivateProfileIntW(L"Output", L"ClickThrough", s.clickThrough ? 1 : 0, path.c_str()) != 0);
    s.cursorOverlay = (GetPrivateProfileIntW(L"Output", L"CursorOverlay", s.cursorOverlay ? 1 : 0, path.c_str()) != 0);
    s.excludeFromCapture = (GetPrivateProfileIntW(L"Output", L"ExcludeFromCapture", s.excludeFromCapture ? 1 : 0, path.c_str()) != 0);
    s.overlayPosIndex = ClampInt((int)GetPrivateProfileIntW(L"Output", L"OverlayPosIndex", s.overlayPosIndex, path.c_str()), 0, 4);

    s.diagnosticsOverlay = (GetPrivateProfileIntW(L"Diagnostics", L"OverlayEnabled", s.diagnosticsOverlay ? 1 : 0, path.c_str()) != 0);
    s.diagnosticsOverlaySizeIndex = ClampInt((int)GetPrivateProfileIntW(L"Diagnostics", L"OverlaySizeIndex", s.diagnosticsOverlaySizeIndex, path.c_str()), 0, 2);
    s.diagnosticsOverlayCompact = (GetPrivateProfileIntW(L"Diagnostics", L"OverlayCompact", s.diagnosticsOverlayCompact ? 1 : 0, path.c_str()) != 0);

    s.framerateIndex = ClampInt((int)GetPrivateProfileIntW(L"Performance", L"FramerateIndex", s.framerateIndex, path.c_str()), 0, 4);
    s.renderResPresetIndex = ClampInt((int)GetPrivateProfileIntW(L"Performance", L"RenderResPresetIndex", s.renderResPresetIndex, path.c_str()), 0, 10);

    Log::Info("Settings loaded from: " + WideToUtf8Local(path));
    return s;
}

void AppSettings::Save() const {
    const std::wstring path = GetSettingsPath();

    WriteBool(path, L"Stereo", L"Enabled", stereoEnabled);
    WriteInt(path, L"Stereo", L"DepthLevel", ClampInt(stereoDepthLevel, 1, 20));
    WriteInt(path, L"Stereo", L"ParallaxStrengthPercent", ClampInt(stereoParallaxStrengthPercent, 0, 50));

    WriteBool(path, L"Output", L"VSyncEnabled", vsyncEnabled);
    WriteBool(path, L"Output", L"ClickThrough", clickThrough);
    WriteBool(path, L"Output", L"CursorOverlay", cursorOverlay);
    WriteBool(path, L"Output", L"ExcludeFromCapture", excludeFromCapture);
    WriteInt(path, L"Output", L"OverlayPosIndex", ClampInt(overlayPosIndex, 0, 4));

    WriteBool(path, L"Diagnostics", L"OverlayEnabled", diagnosticsOverlay);
    WriteInt(path, L"Diagnostics", L"OverlaySizeIndex", ClampInt(diagnosticsOverlaySizeIndex, 0, 2));
    WriteBool(path, L"Diagnostics", L"OverlayCompact", diagnosticsOverlayCompact);

    WriteInt(path, L"Performance", L"FramerateIndex", ClampInt(framerateIndex, 0, 4));
    WriteInt(path, L"Performance", L"RenderResPresetIndex", ClampInt(renderResPresetIndex, 0, 10));
}
