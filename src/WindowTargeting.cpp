#include "WindowTargeting.h"

#include <cwctype>

namespace WindowTargeting {

HWND GetRootWindowOrSelf(HWND hwnd) {
    if (!hwnd) return nullptr;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    return root ? root : hwnd;
}

DWORD GetWindowProcessIdSafe(HWND hwnd) {
    DWORD pid = 0;
    if (hwnd && IsWindow(hwnd)) {
        GetWindowThreadProcessId(hwnd, &pid);
    }
    return pid;
}

bool GetClientSizeSafe(HWND hwnd, UINT* outW, UINT* outH) {
    if (outW) *outW = 0;
    if (outH) *outH = 0;
    if (!hwnd || !IsWindow(hwnd)) return false;
    RECT cr{};
    if (!GetClientRect(hwnd, &cr)) return false;
    const int w = cr.right - cr.left;
    const int h = cr.bottom - cr.top;
    if (w <= 0 || h <= 0) return false;
    if (outW) *outW = (UINT)w;
    if (outH) *outH = (UINT)h;
    return true;
}

std::wstring GetWindowTitleSafe(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return L"";
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return L"";
    std::wstring title;
    title.resize((size_t)len + 1);
    GetWindowTextW(hwnd, title.data(), len + 1);
    title.resize(wcsnlen(title.c_str(), (size_t)len + 1));
    return title;
}

std::wstring ToLowerCopy(std::wstring s) {
    for (auto& ch : s) {
        ch = (wchar_t)towlower(ch);
    }
    return s;
}

static std::wstring GetWindowClassNameSafe(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return L"";
    wchar_t buf[256] = {};
    const int n = GetClassNameW(hwnd, buf, (int)ARRAYSIZE(buf));
    if (n <= 0) return L"";
    return std::wstring(buf, buf + n);
}

static std::wstring GetProcessExeNameLower(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return L"";

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";

    wchar_t path[MAX_PATH] = {};
    DWORD size = (DWORD)ARRAYSIZE(path);
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, path, &size) && size > 0) {
        std::wstring full(path, path + size);
        size_t slash = full.find_last_of(L"\\/");
        out = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
        out = ToLowerCopy(out);
    }
    CloseHandle(h);
    return out;
}

bool IsCandidateCapturedTargetWindow(HWND hwnd, HWND excludedA, HWND excludedB) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    HWND root = GetRootWindowOrSelf(hwnd);
    if (!root || !IsWindow(root)) return false;
    if (excludedA && root == excludedA) return false;
    if (excludedB && root == excludedB) return false;
    if (root == GetDesktopWindow() || root == GetShellWindow()) return false;
    if (!IsWindowVisible(root)) return false;
    return true;
}

bool IsProbablyShellOrExplorerWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return true;
    HWND root = GetRootWindowOrSelf(hwnd);
    if (!root || !IsWindow(root)) return true;

    const std::wstring cls = ToLowerCopy(GetWindowClassNameSafe(root));
    if (cls == L"cabinetwclass" || cls == L"explorerwclass" || cls == L"shell_traywnd" || cls == L"progman" ||
        cls == L"workerw" || cls == L"applicationframewindow") {
        return true;
    }

    const std::wstring exe = GetProcessExeNameLower(root);
    if (exe == L"explorer.exe" || exe == L"searchhost.exe" || exe == L"startmenuexperiencehost.exe" ||
        exe == L"applicationframehost.exe" || exe == L"shellexperiencehost.exe") {
        return true;
    }

    return false;
}

bool HasAnyCandidateCapturedTargetWindow(HWND excludedA, HWND excludedB) {
    struct Ctx {
        HWND excludedA;
        HWND excludedB;
        bool found;
    } ctx{excludedA, excludedB, false};

    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* ctx = reinterpret_cast<Ctx*>(lParam);
            if (!ctx) return TRUE;
            HWND root = GetRootWindowOrSelf(hwnd);
            if (!IsCandidateCapturedTargetWindow(root, ctx->excludedA, ctx->excludedB)) return TRUE;
            ctx->found = true;
            return FALSE;
        },
        (LPARAM)&ctx);

    return ctx.found;
}

HWND FindTopLevelWindowByTitleExact(const std::wstring& title, HWND excludedA, HWND excludedB) {
    if (title.empty()) return nullptr;
    struct Ctx {
        const std::wstring* title;
        HWND excludedA;
        HWND excludedB;
        HWND result;
    } ctx{&title, excludedA, excludedB, nullptr};

    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* ctx = reinterpret_cast<Ctx*>(lParam);
            if (!ctx || !ctx->title) return TRUE;

            HWND root = GetRootWindowOrSelf(hwnd);
            if (!IsCandidateCapturedTargetWindow(root, ctx->excludedA, ctx->excludedB)) return TRUE;

            int len = GetWindowTextLengthW(root);
            if (len <= 0) return TRUE;
            std::wstring text;
            text.resize((size_t)len + 1);
            GetWindowTextW(root, text.data(), len + 1);
            text.resize(wcsnlen(text.c_str(), (size_t)len + 1));

            if (text == *ctx->title) {
                ctx->result = root;
                return FALSE;
            }
            return TRUE;
        },
        (LPARAM)&ctx);

    return ctx.result;
}

HWND FindBestTopLevelWindowForFocusHint(
    const std::wstring& titleHint,
    UINT expectedW,
    UINT expectedH,
    HWND excludedA,
    HWND excludedB) {
    struct Best {
        int score = -1;
        HWND hwnd = nullptr;
    } best;

    struct Ctx {
        const std::wstring* titleHint;
        UINT expectedW;
        UINT expectedH;
        HWND foregroundRoot;
        HWND excludedA;
        HWND excludedB;
        Best* best;
    } ctx{&titleHint, expectedW, expectedH, GetRootWindowOrSelf(GetForegroundWindow()), excludedA, excludedB, &best};

    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* ctx = reinterpret_cast<Ctx*>(lp);
            if (!ctx || !ctx->best) return TRUE;

            HWND root = GetRootWindowOrSelf(hwnd);
            if (!IsCandidateCapturedTargetWindow(root, ctx->excludedA, ctx->excludedB)) return TRUE;
            if (IsProbablyShellOrExplorerWindow(root)) return TRUE;

            int score = 0;

            // Title hint scoring (best-effort)
            if (ctx->titleHint && !ctx->titleHint->empty()) {
                const std::wstring title = GetWindowTitleSafe(root);
                if (!title.empty()) {
                    if (title == *ctx->titleHint) score += 100;
                    const std::wstring titleLower = ToLowerCopy(title);
                    const std::wstring hintLower = ToLowerCopy(*ctx->titleHint);
                    if (!hintLower.empty() && titleLower.find(hintLower) != std::wstring::npos) score += 50;
                }
            }

            // Size scoring
            if (ctx->expectedW > 0 && ctx->expectedH > 0) {
                UINT cw = 0, ch = 0;
                if (GetClientSizeSafe(root, &cw, &ch)) {
                    const int dw = (int)cw - (int)ctx->expectedW;
                    const int dh = (int)ch - (int)ctx->expectedH;
                    const int adw = dw < 0 ? -dw : dw;
                    const int adh = dh < 0 ? -dh : dh;
                    if (adw <= 8 && adh <= 8) score += 30;
                    else if (adw <= 32 && adh <= 32) score += 15;
                }
            }

            // Slightly prefer the current foreground window if it passes filters.
            if (root && ctx->foregroundRoot && root == ctx->foregroundRoot) score += 5;

            if (score > ctx->best->score) {
                ctx->best->score = score;
                ctx->best->hwnd = root;
            }
            return TRUE;
        },
        (LPARAM)&ctx);

    const bool hasHints = (!titleHint.empty()) || (expectedW > 0 && expectedH > 0);
    if (hasHints) {
        // Avoid selecting an arbitrary window (e.g., the currently focused one) unless we have a real match.
        if (best.score >= 15) return best.hwnd;
        return nullptr;
    }

    // No hints: still return a best candidate (foreground gets a small bump).
    if (best.score >= 0) return best.hwnd;
    return nullptr;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int bytesNeeded = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (bytesNeeded <= 0) return std::string();
    std::string out;
    out.resize((size_t)bytesNeeded);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), bytesNeeded, nullptr, nullptr);
    return out;
}

} // namespace WindowTargeting
