#include "DepthDialog.h"

#include <commctrl.h>
#include <vector>

#include "../res/resource.h"

#pragma comment(lib, "Comctl32.lib")

static constexpr int kStereoDepthDefault = 10;
static constexpr int kStereoParallaxDefaultPercent = 20;
static constexpr int kStereoParallaxMaxPercent = 50;

static void SetDepthValueText(HWND hDlg, int v) {
    TCHAR buf[32];
    wsprintf(buf, TEXT("%d"), v);
    SetDlgItemText(hDlg, IDC_DEPTH_VALUE, buf);
}

static void SetParallaxValueText(HWND hDlg, int percent) {
    if (percent < 0) percent = 0;
    if (percent > kStereoParallaxMaxPercent) percent = kStereoParallaxMaxPercent;
    TCHAR buf[32];
    wsprintf(buf, TEXT("%d%%"), percent);
    SetDlgItemText(hDlg, IDC_PARALLAX_VALUE, buf);
}

struct StereoDialogState {
    int* outDepth = nullptr;
    int* outParallaxStrengthPercent = nullptr;

    bool modeless = false;

    int originalDepth = kStereoDepthDefault;
    int originalParallaxStrengthPercent = kStereoParallaxDefaultPercent;
    int workingDepth = kStereoDepthDefault;
    int workingParallaxStrengthPercent = kStereoParallaxDefaultPercent;

    std::function<void(int, int)> onPreview;
    std::function<void(bool, int, int)> onDone;
};

static void MoveDialogToNextMonitor(HWND hDlg) {
    if (!hDlg) return;

    struct MonitorEntry {
        HMONITOR hMon;
        RECT work;
    };

    std::vector<MonitorEntry> monitors;
    monitors.reserve(8);

    auto enumProc = [](HMONITOR hMon, HDC, LPRECT, LPARAM lParam) -> BOOL {
        auto* list = reinterpret_cast<std::vector<MonitorEntry>*>(lParam);
        if (!list) return FALSE;
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfo(hMon, &mi)) return TRUE;
        list->push_back(MonitorEntry{ hMon, mi.rcWork });
        return TRUE;
    };

    EnumDisplayMonitors(nullptr, nullptr, enumProc, (LPARAM)&monitors);
    if (monitors.size() <= 1) return;

    HMONITOR current = MonitorFromWindow(hDlg, MONITOR_DEFAULTTONEAREST);
    size_t idx = 0;
    for (size_t i = 0; i < monitors.size(); ++i) {
        if (monitors[i].hMon == current) { idx = i; break; }
    }
    const size_t next = (idx + 1) % monitors.size();

    RECT wr{};
    if (!GetWindowRect(hDlg, &wr)) return;
    const int w = (int)(wr.right - wr.left);
    const int h = (int)(wr.bottom - wr.top);

    const RECT& work = monitors[next].work;
    int x = work.left + ((work.right - work.left) - w) / 2;
    int y = work.top + ((work.bottom - work.top) - h) / 2;

    // Clamp into work area.
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
    if (x + w > work.right) x = work.right - w;
    if (y + h > work.bottom) y = work.bottom - h;

    SetWindowPos(hDlg, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void ApplyWorkingFromSliders(HWND hDlg, StereoDialogState* state) {
    if (!state) return;

    HWND hDepth = GetDlgItem(hDlg, IDC_DEPTH_SLIDER);
    HWND hParallax = GetDlgItem(hDlg, IDC_PARALLAX_SLIDER);

    int depth = state->workingDepth;
    int parallaxStrength = state->workingParallaxStrengthPercent;
    if (hDepth) depth = (int)SendMessage(hDepth, TBM_GETPOS, 0, 0);
    if (hParallax) parallaxStrength = (int)SendMessage(hParallax, TBM_GETPOS, 0, 0);

    if (depth < 1) depth = 1;
    if (depth > 20) depth = 20;
    if (parallaxStrength < 0) parallaxStrength = 0;
    if (parallaxStrength > kStereoParallaxMaxPercent) parallaxStrength = kStereoParallaxMaxPercent;

    state->workingDepth = depth;
    state->workingParallaxStrengthPercent = parallaxStrength;

    SetDepthValueText(hDlg, depth);
    SetParallaxValueText(hDlg, parallaxStrength);

    if (state->onPreview) {
        state->onPreview(depth, parallaxStrength);
    }
}

static INT_PTR CALLBACK DepthDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        StereoDialogState* state = (StereoDialogState*)lParam;
        SetWindowLongPtr(hDlg, GWLP_USERDATA, (LONG_PTR)state);

        HWND hSlider = GetDlgItem(hDlg, IDC_DEPTH_SLIDER);
        if (hSlider) {
            SendMessage(hSlider, TBM_SETRANGE, TRUE, MAKELPARAM(1, 20));
            SendMessage(hSlider, TBM_SETTICFREQ, 1, 0);
            int v = kStereoDepthDefault;
            if (state) v = state->workingDepth;
            SendMessage(hSlider, TBM_SETPOS, TRUE, v);
            SetDepthValueText(hDlg, v);
        }

        HWND hParallaxSlider = GetDlgItem(hDlg, IDC_PARALLAX_SLIDER);
        if (hParallaxSlider) {
            SendMessage(hParallaxSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, kStereoParallaxMaxPercent));
            SendMessage(hParallaxSlider, TBM_SETTICFREQ, 10, 0);
            int v = kStereoParallaxDefaultPercent;
            if (state) {
                v = state->workingParallaxStrengthPercent;
            }
            if (v < 0) v = 0;
            if (v > kStereoParallaxMaxPercent) v = kStereoParallaxMaxPercent;
            SendMessage(hParallaxSlider, TBM_SETPOS, TRUE, v);
            SetParallaxValueText(hDlg, v);
        }
        return TRUE;
    }
    case WM_CLOSE:
        PostMessage(hDlg, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), 0);
        return TRUE;
    case WM_HSCROLL: {
        auto* state = (StereoDialogState*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
        ApplyWorkingFromSliders(hDlg, state);
        return TRUE;
    }
    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case IDC_STEREO_RESET_DEFAULTS: {
            auto* state = (StereoDialogState*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
            if (!state) return TRUE;

            state->workingDepth = kStereoDepthDefault;
            state->workingParallaxStrengthPercent = kStereoParallaxDefaultPercent;

            HWND hDepth = GetDlgItem(hDlg, IDC_DEPTH_SLIDER);
            HWND hParallax = GetDlgItem(hDlg, IDC_PARALLAX_SLIDER);
            if (hDepth) SendMessage(hDepth, TBM_SETPOS, TRUE, state->workingDepth);
            if (hParallax) SendMessage(hParallax, TBM_SETPOS, TRUE, state->workingParallaxStrengthPercent);

            ApplyWorkingFromSliders(hDlg, state);
            return TRUE;
        }
        case IDC_STEREO_NEXT_MONITOR:
            MoveDialogToNextMonitor(hDlg);
            return TRUE;
        case IDOK: {
            StereoDialogState* state = (StereoDialogState*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
            ApplyWorkingFromSliders(hDlg, state);

            if (state) {
                if (state->outDepth) *state->outDepth = state->workingDepth;
                if (state->outParallaxStrengthPercent) *state->outParallaxStrengthPercent = state->workingParallaxStrengthPercent;
                if (state->onDone) {
                    state->onDone(true, state->workingDepth, state->workingParallaxStrengthPercent);
                }
            }
            if (state && state->modeless) DestroyWindow(hDlg);
            else EndDialog(hDlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            if (auto* state = (StereoDialogState*)GetWindowLongPtr(hDlg, GWLP_USERDATA)) {
                if (state->onPreview) {
                    state->onPreview(state->originalDepth, state->originalParallaxStrengthPercent);
                }
                if (state->onDone) {
                    state->onDone(false, state->originalDepth, state->originalParallaxStrengthPercent);
                }
                if (state->modeless) {
                    DestroyWindow(hDlg);
                    return TRUE;
                }
            }
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        default: 
            break;
        }
        break;
    }
    case WM_NCDESTROY: {
        // If this was created modeless, we own and must free the heap state.
        auto* state = (StereoDialogState*)GetWindowLongPtr(hDlg, GWLP_USERDATA);
        if (state && state->modeless) {
            SetWindowLongPtr(hDlg, GWLP_USERDATA, 0);
            delete state;
        }
        break;
    }
    }
    return FALSE;
}

bool DepthDialog::Show(HWND hWndParent, int& depthLevel, int& parallaxStrengthPercent,
                       const std::function<void(int depthLevel, int parallaxStrengthPercent)>& onPreview) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    int localDepth = depthLevel;
    int localParallaxStrength = parallaxStrengthPercent;

    if (localDepth < 1) localDepth = 1;
    if (localDepth > 20) localDepth = 20;
    if (localParallaxStrength < 0) localParallaxStrength = 0;
    if (localParallaxStrength > kStereoParallaxMaxPercent) localParallaxStrength = kStereoParallaxMaxPercent;

    StereoDialogState state;
    state.outDepth = &localDepth;
    state.outParallaxStrengthPercent = &localParallaxStrength;
    state.modeless = false;
    state.originalDepth = localDepth;
    state.originalParallaxStrengthPercent = localParallaxStrength;
    state.workingDepth = localDepth;
    state.workingParallaxStrengthPercent = localParallaxStrength;
    state.onPreview = onPreview;

    INT_PTR r = DialogBoxParam(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDD_DEPTH_DIALOG), hWndParent, DepthDlgProc, (LPARAM)&state);
    if (r == IDOK) {
        depthLevel = localDepth;
        parallaxStrengthPercent = localParallaxStrength;
        return true;
    }
    return false;
}

HWND DepthDialog::ShowModeless(
    HWND hWndParent,
    int initialDepthLevel,
    int initialParallaxStrengthPercent,
    const std::function<void(int depthLevel, int parallaxStrengthPercent)>& onPreview,
    const std::function<void(bool accepted, int depthLevel, int parallaxStrengthPercent)>& onDone) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    int localDepth = initialDepthLevel;
    int localParallaxStrength = initialParallaxStrengthPercent;
    if (localDepth < 1) localDepth = 1;
    if (localDepth > 20) localDepth = 20;
    if (localParallaxStrength < 0) localParallaxStrength = 0;
    if (localParallaxStrength > kStereoParallaxMaxPercent) localParallaxStrength = kStereoParallaxMaxPercent;

    auto* state = new StereoDialogState();
    state->outDepth = nullptr;
    state->outParallaxStrengthPercent = nullptr;
    state->modeless = true;
    state->originalDepth = localDepth;
    state->originalParallaxStrengthPercent = localParallaxStrength;
    state->workingDepth = localDepth;
    state->workingParallaxStrengthPercent = localParallaxStrength;
    state->onPreview = onPreview;
    state->onDone = onDone;

    HWND hDlg = CreateDialogParam(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDD_DEPTH_DIALOG), hWndParent, DepthDlgProc, (LPARAM)state);
    if (!hDlg) {
        delete state;
        return nullptr;
    }
    ShowWindow(hDlg, SW_SHOW);
    return hDlg;
}
