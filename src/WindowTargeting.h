#pragma once

#include <windows.h>

#include <string>

namespace WindowTargeting {

HWND GetRootWindowOrSelf(HWND hwnd);

DWORD GetWindowProcessIdSafe(HWND hwnd);

bool GetClientSizeSafe(HWND hwnd, UINT* outW, UINT* outH);

std::wstring GetWindowTitleSafe(HWND hwnd);
std::wstring ToLowerCopy(std::wstring s);

bool IsCandidateCapturedTargetWindow(HWND hwnd, HWND excludedA, HWND excludedB);
bool IsProbablyShellOrExplorerWindow(HWND hwnd);

// Returns true if there is at least one visible, non-desktop top-level window that could be captured.
// Intended for UI gating (e.g., disabling "Select Window" when there is nothing to select).
bool HasAnyCandidateCapturedTargetWindow(HWND excludedA, HWND excludedB);

HWND FindTopLevelWindowByTitleExact(const std::wstring& title, HWND excludedA, HWND excludedB);
HWND FindBestTopLevelWindowForFocusHint(
    const std::wstring& titleHint,
    UINT expectedW,
    UINT expectedH,
    HWND excludedA,
    HWND excludedB);

std::string WideToUtf8(const std::wstring& w);

} // namespace WindowTargeting
