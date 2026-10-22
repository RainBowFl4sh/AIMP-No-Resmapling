// Restarting AIMP with a new output sample rate (EXPERIMENTAL).
//
// AIMP reads AIMPSoundOut\DeviceFreq only at start-up (or from its own settings page); there is no
// plugin interface to change it at runtime. Therefore:
//   1. The plugin starts "rundll32 PreventResampling.dll,RestartAimp ..." and closes AIMP.
//   2. Seamless mode: the helper first takes a picture of AIMP's visible windows and shows it at
//      exactly the same place and z-order ("freeze frame"), so AIMP seems to stay open.
//   3. The helper waits until AIMP has exited (AIMP still writes its settings while closing), writes
//      the new rate into AIMP.ini and starts AIMP again.
//   4. The plugin in the new AIMP gives its windows their state, normal position and size, z-order and
//      focus from before the restart back (WindowRestorer) and signals the helper; then the still
//      image fades out.
//   5. On start-up the plugin continues the track at the same position.
#pragma once
#include <shellapi.h>

#include "../BitPerfect.h"
#include "../Common.h"

namespace ar {
namespace win {

inline std::wstring ModulePath(HMODULE m) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(m, buf, MAX_PATH * 2);
    return std::wstring(buf, n);
}

inline HMODULE ThisModule() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&ThisModule, &m);
    return m;
}

// Named event: the helper signals it when the still image is on screen (or when there is none)
inline std::wstring ReadyEventName(DWORD pid) { return L"Local\\PreventResampling_Ready_" + std::to_wstring(pid); }

// Sets [section] key=value in an INI file (UTF-16 LE, UTF-8 with/without BOM or ANSI).
// Encoding, BOM and line endings are preserved.
inline bool PatchIni(const std::wstring& path, const std::wstring& section, const std::wstring& key,
                     const std::wstring& value) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    std::string raw;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) raw.append(buf, n);
    fclose(f);

    enum { U16, U8BOM, U8, ANSI } enc = U8;
    std::wstring text;
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        enc = U16;
        text.assign((const wchar_t*)(raw.data() + 2), (raw.size() - 2) / 2);
    } else {
        size_t start = 0;
        if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) {
            enc = U8BOM;
            start = 3;
        }
        int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data() + start, (int)(raw.size() - start), nullptr, 0);
        UINT cp = CP_UTF8;
        if (len <= 0 && raw.size() > start) {
            enc = ANSI;
            cp = CP_ACP;
            len = MultiByteToWideChar(cp, 0, raw.data() + start, (int)(raw.size() - start), nullptr, 0);
        }
        text.resize(len > 0 ? len : 0);
        if (len > 0) MultiByteToWideChar(cp, 0, raw.data() + start, (int)(raw.size() - start), &text[0], len);
    }

    std::wstring nl = text.find(L"\r\n") != std::wstring::npos ? L"\r\n" : L"\n";
    std::vector<std::wstring> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t e = text.find(L'\n', pos);
        std::wstring line = text.substr(pos, e == std::wstring::npos ? std::wstring::npos : e - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(line);
        if (e == std::wstring::npos) break;
        pos = e + 1;
    }

    std::wstring header = L"[" + Lower(section) + L"]", keyEq = Lower(key) + L"=";
    int secLine = -1, keyLine = -1, secEnd = (int)lines.size();
    for (int i = 0; i < (int)lines.size(); i++) {
        std::wstring t = Lower(Trim(lines[i]));
        if (!t.empty() && t[0] == L'[') {
            if (secLine >= 0) { secEnd = i; break; }
            if (t == header) secLine = i;
        } else if (secLine >= 0 && keyLine < 0) {
            std::wstring compact;
            for (wchar_t c : t) if (c != L' ' && c != L'\t') compact += c;
            if (compact.compare(0, keyEq.size(), keyEq) == 0) keyLine = i;
        }
    }
    std::wstring entry = key + L"=" + value;
    if (keyLine >= 0) lines[keyLine] = entry;
    else if (secLine >= 0) lines.insert(lines.begin() + secEnd, entry);
    else { lines.push_back(L"[" + section + L"]"); lines.push_back(entry); }

    std::wstring out;
    for (size_t i = 0; i < lines.size(); i++) out += lines[i] + (i + 1 < lines.size() ? nl : L"");

    std::string bytes;
    if (enc == U16) {
        bytes = "\xFF\xFE";
        bytes.append((const char*)out.data(), out.size() * 2);
    } else {
        UINT cp = enc == ANSI ? CP_ACP : CP_UTF8;
        int len = WideCharToMultiByte(cp, 0, out.data(), (int)out.size(), nullptr, 0, nullptr, nullptr);
        std::string s(len > 0 ? len : 0, '\0');
        if (len > 0) WideCharToMultiByte(cp, 0, out.data(), (int)out.size(), &s[0], len, nullptr, nullptr);
        if (enc == U8BOM) bytes = "\xEF\xBB\xBF";
        bytes += s;
    }
    std::wstring tmp = path + L".prtmp";
    f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

// ---------------------------------------------------------------------------------------------
// Waiting with a running message loop (the still-image windows must keep painting)

inline void PumpMessages() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

// true = handle signalled, false = timeout. Without handle: just keeps the windows alive for ms.
inline bool WaitPumping(HANDLE h, DWORD ms) {
    DWORD start = GetTickCount();
    for (;;) {
        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= ms) return false;
        DWORD r = h ? MsgWaitForMultipleObjects(1, &h, FALSE, ms - elapsed, QS_ALLINPUT)
                    : MsgWaitForMultipleObjects(0, nullptr, FALSE, ms - elapsed, QS_ALLINPUT);
        if (h && r == WAIT_OBJECT_0) return true;
        if (r == WAIT_OBJECT_0 + (h ? 1u : 0u)) { PumpMessages(); continue; }
        if (r == WAIT_TIMEOUT) return false;
        return false;
    }
}

inline std::wstring ClassOf(HWND h) {
    wchar_t cls[128] = {};
    GetClassNameW(h, cls, 128);
    return cls;
}

// ---------------------------------------------------------------------------------------------
// Still image ("freeze frame") of AIMP's windows

class StillImage {
    struct Shot {
        HWND source = nullptr, ghost = nullptr;
        HBITMAP bmp = nullptr;
        RECT rc = {};
        bool topmost = false;
    };
    std::vector<Shot> shots_;

    static LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_PAINT: {
                PAINTSTRUCT ps;
                HDC dc = BeginPaint(h, &ps);
                HBITMAP bmp = (HBITMAP)GetWindowLongPtrW(h, GWLP_USERDATA);
                if (bmp) {
                    BITMAP bm;
                    GetObjectW(bmp, sizeof(bm), &bm);
                    HDC mem = CreateCompatibleDC(dc);
                    HGDIOBJ old = SelectObject(mem, bmp);
                    BitBlt(dc, 0, 0, bm.bmWidth, bm.bmHeight, mem, 0, 0, SRCCOPY);
                    SelectObject(mem, old);
                    DeleteDC(mem);
                }
                EndPaint(h, &ps);
                return 0;
            }
            case WM_ERASEBKGND: return 1;
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    struct Search {
        DWORD pid;
        std::vector<HWND> found;
    };
    static BOOL CALLBACK Collect(HWND h, LPARAM lp) {
        Search* s = (Search*)lp;
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        RECT rc;
        if (pid == s->pid && IsWindowVisible(h) && !IsIconic(h) && GetWindowRect(h, &rc) && rc.right - rc.left >= 50 &&
            rc.bottom - rc.top >= 30)
            s->found.push_back(h);
        return TRUE;
    }

public:
    ~StillImage() { Remove(0); }

    static std::vector<HWND> VisibleWindows(DWORD pid) {
        Search s{pid, {}};
        EnumWindows(Collect, (LPARAM)&s);
        return s.found;  // top to bottom
    }

    // Takes pictures of all visible windows of the process and shows them in their place
    int Show(DWORD pid) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = Proc;
            wc.hInstance = ThisModule();
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            wc.lpszClassName = L"PreventResamplingStillImage";
            registered = RegisterClassW(&wc) != 0;
        }
        HDC screen = GetDC(nullptr);
        for (HWND src : VisibleWindows(pid)) {
            Shot s;
            s.source = src;
            GetWindowRect(src, &s.rc);
            s.topmost = (GetWindowLongW(src, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
            int w = s.rc.right - s.rc.left, hgt = s.rc.bottom - s.rc.top;
            HDC mem = CreateCompatibleDC(screen);
            s.bmp = CreateCompatibleBitmap(screen, w, hgt);
            HGDIOBJ old = SelectObject(mem, s.bmp);
            BitBlt(mem, 0, 0, w, hgt, screen, s.rc.left, s.rc.top, SRCCOPY | CAPTUREBLT);
            SelectObject(mem, old);
            DeleteDC(mem);
            shots_.push_back(s);
        }
        ReleaseDC(nullptr, screen);
        // Bottom-most first, each directly above its source window
        for (auto it = shots_.rbegin(); it != shots_.rend(); ++it) {
            Shot& s = *it;
            DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | (s.topmost ? WS_EX_TOPMOST : 0);
            // Named after the window's class: the new AIMP puts its window directly below it
            s.ghost = CreateWindowExW(ex, L"PreventResamplingStillImage", ClassOf(s.source).c_str(), WS_POPUP, s.rc.left, s.rc.top,
                                      s.rc.right - s.rc.left, s.rc.bottom - s.rc.top, nullptr, nullptr, ThisModule(), nullptr);
            if (!s.ghost) continue;
            SetWindowLongPtrW(s.ghost, GWLP_USERDATA, (LONG_PTR)s.bmp);
            SetLayeredWindowAttributes(s.ghost, 0, 255, LWA_ALPHA);
            HWND above = GetWindow(s.source, GW_HWNDPREV);
            HWND after = above ? above : (s.topmost ? HWND_TOPMOST : HWND_TOP);
            SetWindowPos(s.ghost, after, s.rc.left, s.rc.top, s.rc.right - s.rc.left, s.rc.bottom - s.rc.top,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            UpdateWindow(s.ghost);
        }
        PumpMessages();
        return (int)shots_.size();
    }

    bool Empty() const { return shots_.empty(); }

    // AIMP was in front: the still image takes the focus, so that Windows does not activate and raise
    // another window when the old AIMP closes
    bool TakeFocus() {
        if (shots_.empty() || !shots_.front().ghost) return false;
        HWND g = shots_.front().ghost;
        SetWindowLongW(g, GWL_EXSTYLE, GetWindowLongW(g, GWL_EXSTYLE) & ~WS_EX_NOACTIVATE);
        return SetForegroundWindow(g) != 0;
    }

    // ... and if Windows raises another window anyway, the still image goes back on top of it
    // (the new AIMP then appears directly below it)
    void KeepInFront() {
        for (auto it = shots_.rbegin(); it != shots_.rend(); ++it)
            if (it->ghost && !it->topmost)
                SetWindowPos(it->ghost, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    // Fades the still image out (fadeMs = 0: remove immediately)
    void Remove(int fadeMs) {
        if (shots_.empty()) return;
        const int steps = fadeMs > 0 ? 8 : 0;
        for (int i = steps - 1; i >= 0; i--) {
            for (auto& s : shots_)
                if (s.ghost) SetLayeredWindowAttributes(s.ghost, 0, (BYTE)(255 * i / steps), LWA_ALPHA);
            WaitPumping(nullptr, (DWORD)(fadeMs / steps));
        }
        for (auto& s : shots_) {
            if (s.ghost) DestroyWindow(s.ghost);
            if (s.bmp) DeleteObject(s.bmp);
        }
        shots_.clear();
        PumpMessages();
    }
};

inline bool OwnWindow(HWND h) {
    DWORD pid = 0;
    return h && GetWindowThreadProcessId(h, &pid) && pid == GetCurrentProcessId();
}

// Event the new AIMP (the plugin inside it) signals when its windows have their place back; the
// helper keeps the still image until then
inline std::wstring PlacedEventName(DWORD pid) { return L"Local\\PreventResampling_Placed_" + std::to_wstring(pid); }

// ---------------------------------------------------------------------------------------------
// AIMP's windows before a restart, given back by the plugin in the new AIMP process
//
// Everything is read and set inside AIMP's own process, on its main thread: there the coordinates
// mean the same before and after the restart (scaled displays, DPI modes), and AIMP may change the
// state and z-order of its own windows. Saved in the resume file as one line:
//   "<active>,<foreground window>,<tick>|<class>,<state>,<left>,<top>,<right>,<bottom>|..."
// active = AIMP was the foreground application; foreground window = the window that had the focus
// instead (given back if the new AIMP takes it); tick = GetTickCount() at the restart.
// state: 0 normal, 1 minimized, 2 maximized; the rectangle is the normal position and size
// (workspace coordinates, as GetWindowPlacement returns them).

struct SavedWindow {
    std::wstring cls;
    int state = 0;
    RECT normal = {};
};

struct SavedWindows {
    bool active = true;
    HWND foreground = nullptr;
    DWORD tick = 0;
    std::vector<SavedWindow> windows;  // top to bottom
};

// AIMP's windows that are on screen or minimized: top-level, visible (a minimized main window is
// hidden by AIMP) and at least 50 x 30 pixels
struct AimpWindowSearch {
    DWORD pid;
    bool withMinimized;
    std::vector<HWND> found;
};

inline BOOL CALLBACK CollectAimpWindow(HWND h, LPARAM lp) {
    AimpWindowSearch* s = (AimpWindowSearch*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != s->pid) return TRUE;
    RECT rc;
    if (IsIconic(h)) {
        WINDOWPLACEMENT p = {sizeof(p)};
        if (s->withMinimized && GetWindowPlacement(h, &p) && p.rcNormalPosition.right - p.rcNormalPosition.left >= 50 &&
            p.rcNormalPosition.bottom - p.rcNormalPosition.top >= 30 && ClassOf(h) != L"TApplication")
            s->found.push_back(h);
    } else if (IsWindowVisible(h) && GetWindowRect(h, &rc) && rc.right - rc.left >= 50 && rc.bottom - rc.top >= 30) {
        s->found.push_back(h);
    }
    return TRUE;
}

inline std::vector<HWND> AimpWindows(DWORD pid, bool withMinimized) {
    AimpWindowSearch s{pid, withMinimized, {}};
    EnumWindows(CollectAimpWindow, (LPARAM)&s);
    return s.found;  // top to bottom
}

// Lets this thread use the DPI mode of a window while it exists, so the window's coordinates are read
// and set in the window's own units. AIMP may mix DPI modes (its main thread and its main window can
// differ); without this a restart scaled the normal size, e.g. by 96/120 each time (Windows 10 1607+).
class SameDpiAs {
    typedef HANDLE(WINAPI* SetCtx)(HANDLE);
    SetCtx set_ = nullptr;
    HANDLE old_ = nullptr;

public:
    explicit SameDpiAs(HWND h) {
        typedef HANDLE(WINAPI* GetCtx)(HWND);
        HMODULE user = GetModuleHandleW(L"user32.dll");
        GetCtx get = user ? (GetCtx)GetProcAddress(user, "GetWindowDpiAwarenessContext") : nullptr;
        set_ = user ? (SetCtx)GetProcAddress(user, "SetThreadDpiAwarenessContext") : nullptr;
        if (get && set_) old_ = set_(get(h));
    }
    ~SameDpiAs() { if (set_ && old_) set_(old_); }
};

// Inside AIMP (main thread), right before the restart
inline std::wstring SaveWindowsLine() {
    HWND fg = GetForegroundWindow();
    bool active = fg && OwnWindow(fg);
    std::wstring r = std::wstring(active ? L"1" : L"0") + L"," + std::to_wstring(active ? 0ull : (unsigned long long)(UINT_PTR)fg) +
                     L"," + std::to_wstring(GetTickCount());
    for (HWND h : AimpWindows(GetCurrentProcessId(), true)) {
        WINDOWPLACEMENT p = {sizeof(p)};
        SameDpiAs dpi(h);
        if (!GetWindowPlacement(h, &p)) continue;
        int state = IsIconic(h) ? 1 : IsZoomed(h) ? 2 : 0;
        const RECT& n = p.rcNormalPosition;
        r += L"|" + ClassOf(h) + L"," + std::to_wstring(state) + L"," + std::to_wstring(n.left) + L"," + std::to_wstring(n.top) +
             L"," + std::to_wstring(n.right) + L"," + std::to_wstring(n.bottom);
    }
    return r;
}

inline bool ParseWindowsLine(const std::wstring& line, SavedWindows& out) {
    out = SavedWindows();
    std::vector<std::wstring> items = Split(line, L'|');
    if (items.empty()) return false;
    std::vector<std::wstring> head = Split(items[0], L',');
    if (head.size() < 3) return false;
    out.active = head[0] != L"0";
    out.foreground = (HWND)(UINT_PTR)_wcstoui64(head[1].c_str(), nullptr, 10);
    out.tick = (DWORD)_wcstoui64(head[2].c_str(), nullptr, 10);
    for (size_t i = 1; i < items.size(); i++) {
        const std::wstring& item = items[i];
        long v[5];
        size_t end = item.size();
        bool ok = true;
        for (int k = 4; k >= 0 && ok; k--) {  // state and the four numbers at the end (the class may contain commas)
            size_t comma = end ? item.find_last_of(L',', end - 1) : std::wstring::npos;
            ok = comma != std::wstring::npos && end > comma + 1;
            if (ok) v[k] = _wtol(item.substr(comma + 1, end - comma - 1).c_str()), end = comma;
        }
        if (!ok || end == 0 || v[0] < 0 || v[0] > 2 || v[3] <= v[1] || v[4] <= v[2]) continue;
        SavedWindow w;
        w.cls = item.substr(0, end);
        w.state = (int)v[0];
        w.normal = {v[1], v[2], v[3], v[4]};
        out.windows.push_back(w);
    }
    return true;
}

// The still image of a window (made by the helper, named after the window's class)
struct StillImageSearch {
    const std::wstring* cls;
    HWND found;
};

inline BOOL CALLBACK FindStillImageProc(HWND h, LPARAM lp) {
    StillImageSearch* s = (StillImageSearch*)lp;
    wchar_t title[128] = {};
    if (ClassOf(h) == L"PreventResamplingStillImage" && IsWindowVisible(h) && GetWindowTextW(h, title, 128) && *s->cls == title) {
        s->found = h;
        return FALSE;
    }
    return TRUE;
}

inline HWND FindStillImage(const std::wstring& cls) {
    StillImageSearch s{&cls, nullptr};
    EnumWindows(FindStillImageProc, (LPARAM)&s);
    return s.found;
}

// In the new AIMP process (main thread): gives AIMP's windows their state, normal position and
// place in the z-order back, keeps AIMP in the background if it was there (no activation, the
// window that had the focus keeps it) and tells the helper when the still image may fade out.
class WindowRestorer {
    SavedWindows saved_;
    std::vector<bool> done_;
    std::vector<HWND> normalShown_;  // maximized windows that were shown at their normal size once
    UINT_PTR timer_ = 0;
    HHOOK cbt_ = nullptr, posHook_ = nullptr;
    HWINEVENTHOOK showHook_ = nullptr;
    DWORD start_ = 0, firstDone_ = 0, placed_ = 0;
    bool userActivated_ = false, minimizedEarly_ = false, raised_ = false;
    DWORD lastMinimize_ = 0;
    std::vector<HWND> cloaked_;  // not drawn until they are in their place (Windows 8 and newer)
    int focusReturns_ = 0;

    static WindowRestorer*& Instance() {
        static WindowRestorer* p = nullptr;
        return p;
    }

    // While AIMP comes back in the background: no activation unless the user asks for it
    static LRESULT CALLBACK CbtProc(int code, WPARAM wp, LPARAM lp) {
        WindowRestorer* r = Instance();
        if (code == HCBT_ACTIVATE && r && !r->saved_.active && !r->userActivated_) {
            const CBTACTIVATESTRUCT* a = (const CBTACTIVATESTRUCT*)lp;
            if ((a && a->fMouse) || GetAsyncKeyState(VK_MENU) < 0) r->userActivated_ = true;  // click or Alt+Tab
            else {
                // AIMP asked for the foreground: Windows has already taken it from the other window.
                // Give it back at once (AIMP is busy starting, a timer would come too late).
                r->ReturnFocus();
                return 1;
            }
        }
        // AIMP's windows are invisible from the start until they are in their place (a minimized one
        // until AIMP has minimized it again): no window in its normal size for a moment
        if (code == HCBT_CREATEWND && r) r->Cloak((HWND)wp);
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    // DWM "cloaking" hides a window that is still shown as visible (AIMP draws its skin itself with
    // UpdateLayeredWindow, so a transparent layer would not help)
    static bool SetCloak(HWND h, BOOL on) {
        typedef HRESULT(WINAPI * SetAttr)(HWND, DWORD, LPCVOID, DWORD);
        static SetAttr set = []() -> SetAttr {
            HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
            return dwm ? (SetAttr)GetProcAddress(dwm, "DwmSetWindowAttribute") : nullptr;
        }();
        return set && SUCCEEDED(set(h, 13 /* DWMWA_CLOAK */, &on, sizeof(on)));
    }

    void Cloak(HWND h) {
        if (!SavedFor(h) || IsWindowVisible(h)) return;
        for (HWND c : cloaked_)
            if (c == h) return;
        if (SetCloak(h, TRUE)) cloaked_.push_back(h);
    }

    void Uncloak(HWND h) {
        for (size_t i = 0; i < cloaked_.size(); i++)
            if (cloaked_[i] == h) {
                if (IsWindow(h)) SetCloak(h, FALSE);
                cloaked_.erase(cloaked_.begin() + i);
                return;
            }
    }

    static BOOL CALLBACK CloakExistingProc(HWND h, LPARAM lp) {
        ((WindowRestorer*)lp)->Cloak(h);
        return TRUE;
    }

    // AIMP's windows appear directly below their still image; without one, while AIMP comes back in
    // the background, behind the window that has the focus (minimized ones at the very bottom)
    static LRESULT CALLBACK PosProc(int code, WPARAM wp, LPARAM lp) {
        WindowRestorer* r = Instance();
        const CWPSTRUCT* c = (const CWPSTRUCT*)lp;
        if (code == HC_ACTION && r && c && c->message == WM_WINDOWPOSCHANGING && c->lParam)
            r->KeepBehind(c->hwnd, (WINDOWPOS*)c->lParam);
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    const SavedWindow* SavedFor(HWND h) const {
        if (GetAncestor(h, GA_ROOT) != h) return nullptr;
        std::wstring cls = ClassOf(h);
        for (const SavedWindow& w : saved_.windows)
            if (w.cls == cls) return &w;
        return nullptr;
    }

    bool CanStayBehindForeground() const {
        HWND fg = saved_.foreground;
        return fg && IsWindow(fg) && IsWindowVisible(fg) && !OwnWindow(fg) && !(GetWindowLongW(fg, GWL_EXSTYLE) & WS_EX_TOPMOST);
    }

    void KeepBehind(HWND h, WINDOWPOS* p) {
        if ((p->flags & SWP_NOZORDER) && !(p->flags & SWP_SHOWWINDOW)) return;
        const SavedWindow* w = SavedFor(h);
        if (!w || (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOPMOST)) return;
        if (HWND still = FindStillImage(w->cls)) {
            p->hwndInsertAfter = still;
            p->flags &= ~SWP_NOZORDER;
            if (!saved_.active) p->flags |= SWP_NOACTIVATE;
            return;
        }
        if (saved_.active || userActivated_) return;
        p->flags |= SWP_NOACTIVATE;
        if (w->state == 1) {
            p->hwndInsertAfter = HWND_BOTTOM;
            p->flags &= ~SWP_NOZORDER;
            if ((p->flags & SWP_SHOWWINDOW) && !IsIconic(h)) {
                PostMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0);
                minimizedEarly_ = true;
                lastMinimize_ = GetTickCount() | 1;
            }
        } else if (CanStayBehindForeground() && p->hwndInsertAfter != saved_.foreground) {
            p->hwndInsertAfter = saved_.foreground;
            p->flags &= ~SWP_NOZORDER;
        }
    }

    void ReturnFocus() {
        if (focusReturns_ >= 5 || !saved_.foreground || !IsWindow(saved_.foreground) ||
            !IsWindowVisible(saved_.foreground) || OwnWindow(saved_.foreground))
            return;
        bool ok = SetForegroundWindow(saved_.foreground) != 0;
        if (focusReturns_++ == 0)
            Log(std::wstring(L"Restart: AIMP was in the background - ") +
                (ok ? L"focus given back to the window that had it" : L"could not give the focus back"));
    }

    // As soon as a window is shown: straight to its place directly below its still image
    static void CALLBACK ShowProc(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG child, DWORD, DWORD) {
        WindowRestorer* r = Instance();
        if (r && obj == OBJID_WINDOW && child == CHILDID_SELF && h && GetAncestor(h, GA_ROOT) == h) r->PlaceInZOrder(h);
    }

    static void CALLBACK TimerProc(HWND, UINT, UINT_PTR, DWORD) {
        if (WindowRestorer* r = Instance()) r->Step();
    }

    void PlaceInZOrder(HWND h) {
        if (IsIconic(h) || !IsWindowVisible(h)) return;
        HWND still = FindStillImage(ClassOf(h));
        if (still) {
            SetWindowPos(h, still, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            return;
        }
        // AIMP was in the background: stay behind the window that had the focus (unless that one is
        // "always on top" - placing behind it would make AIMP "always on top", too)
        HWND fg = saved_.foreground;
        if (saved_.active || userActivated_ || !CanStayBehindForeground() || (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOPMOST))
            return;
        const SavedWindow* w = SavedFor(h);
        if (w && w->state == 1) return;  // stays at the bottom until it is minimized
        for (HWND w = GetWindow(h, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV))
            if (w == fg) return;  // already behind it
        SetWindowPos(h, fg, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }

    // true = the window is where it belongs (a minimized one only once AIMP has minimized it)
    bool Apply(HWND h, const SavedWindow& w) {
        SameDpiAs dpi(h);
        WINDOWPLACEMENT p = {sizeof(p)};
        if (!GetWindowPlacement(h, &p)) return true;
        bool settled = true;
        const RECT& n = p.rcNormalPosition;
        bool sameRect = n.left == w.normal.left && n.top == w.normal.top && n.right == w.normal.right && n.bottom == w.normal.bottom;
        bool zoomed = IsZoomed(h) != 0, iconic = IsIconic(h) != 0;
        std::wstring what;
        if (w.state != 1 && std::find(normalShown_.begin(), normalShown_.end(), h) == normalShown_.end()) {
            // First set it once to its normal size (it is still cloaked): AIMP remembers the size for
            // "restore" itself and, after a start, takes it from its skin file without its own scaling
            // (e.g. x 96/120 with AIMP's interface at 125 %). Without this the window got smaller each time
            // it was restored with AIMP's button after a restart.
            // Two real size changes (1 pixel smaller, then the right size): AIMP only takes the size
            // over when the window really changes.
            normalShown_.push_back(h);
            p.rcNormalPosition = w.normal;
            p.rcNormalPosition.right--;
            p.rcNormalPosition.bottom--;
            p.flags = 0;
            p.showCmd = SW_SHOWNOACTIVATE;
            SetWindowPlacement(h, &p);
            SetWindowPos(h, nullptr, w.normal.left, w.normal.top, w.normal.right - w.normal.left, w.normal.bottom - w.normal.top,
                         SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            Log(L"Window " + w.cls + L": set to its normal size first (AIMP keeps it for restoring)");
            return false;  // maximized or placed in the next step, after AIMP has seen the normal size
        }
        if (w.state == 1) {
            // Minimized before: AIMP minimizes itself in its own way (taskbar or tray)
            bool visible = IsWindowVisible(h) != 0;
            if (!iconic && visible) {
                settled = false;
                DWORD now = GetTickCount();
                if (!lastMinimize_ || now - lastMinimize_ > 500) {
                    PostMessageW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0);
                    lastMinimize_ = now ? now : 1;
                    minimizedEarly_ = true;
                }
            } else if (!sameRect) {
                // Minimized (or hidden in the tray): only the normal place is set, it stays as it is
                p.rcNormalPosition = w.normal;
                p.flags = 0;
                p.showCmd = iconic ? SW_SHOWMINNOACTIVE : SW_HIDE;
                SetWindowPlacement(h, &p);
                what = L"minimized again";
            } else if (minimizedEarly_) {
                what = L"minimized again";
            }
            if (settled) minimizedEarly_ = false;
        } else if (w.state == 2) {
            if (!sameRect || !zoomed || iconic) {
                p.rcNormalPosition = w.normal;
                p.flags = 0;
                // already maximized: SW_SHOWNA keeps it maximized and only sets the normal rectangle
                p.showCmd = zoomed && !iconic ? SW_SHOWNA : SW_SHOWMAXIMIZED;
                SetWindowPlacement(h, &p);
                what = zoomed ? L"kept maximized" : L"maximized again";
            }
        } else if (!sameRect || zoomed || iconic) {
            p.rcNormalPosition = w.normal;
            p.flags = 0;
            p.showCmd = SW_SHOWNOACTIVATE;
            SetWindowPlacement(h, &p);
            what = L"placed again";
        }
        if (!what.empty())
            Log(L"Window " + w.cls + L": " + what + L", normal size " + std::to_wstring(w.normal.right - w.normal.left) + L"x" +
                std::to_wstring(w.normal.bottom - w.normal.top) + L" at " + std::to_wstring(w.normal.left) + L"," +
                std::to_wstring(w.normal.top));
        PlaceInZOrder(h);
        return settled;
    }



    bool UserInputSinceRestart() const {
        LASTINPUTINFO li = {sizeof(li)};
        return GetLastInputInfo(&li) && (LONG)(li.dwTime - saved_.tick) > 0;
    }

    void Step() {
        DWORD now = GetTickCount();
        // Windows that are back
        std::vector<HWND> mine = AimpWindows(GetCurrentProcessId(), true);
        std::vector<bool> used(mine.size(), false);
        for (size_t i = 0; i < saved_.windows.size(); i++) {
            if (done_[i]) continue;
            for (size_t k = 0; k < mine.size(); k++) {
                if (used[k] || ClassOf(mine[k]) != saved_.windows[i].cls) continue;
                used[k] = true;
                done_[i] = Apply(mine[k], saved_.windows[i]);
                if (done_[i]) Uncloak(mine[k]);  // in its place (a minimized one: minimized again)
                if (!firstDone_) firstDone_ = now;
                break;
            }
        }
        bool all = true;
        for (bool d : done_) all = all && d;
        if (!placed_ && (all || (firstDone_ && now - firstDone_ > 1500) || now - start_ > 20000)) {
            placed_ = now ? now : 1;
            HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, PlacedEventName(GetCurrentProcessId()).c_str());
            if (ev) { SetEvent(ev); CloseHandle(ev); }
            // AIMP was in front: be in front again (only possible if Windows allows it)
            HWND fg = GetForegroundWindow();
            if (saved_.active && !(fg && OwnWindow(fg)) && !UserInputSinceRestart())
                if (HWND main = FirstRestored(mine)) SetForegroundWindow(main);
        }
        // AIMP was in front: on top again once the still image is gone (it was kept directly below it)
        if (saved_.active && placed_ && !raised_) {
            HWND main = FirstRestored(mine), fg = GetForegroundWindow();
            if (main && !FindStillImage(ClassOf(main))) {
                raised_ = true;
                if (fg && OwnWindow(fg) && !UserInputSinceRestart())
                    SetWindowPos(main, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            }
        }
        // AIMP was in the background: if it took the focus anyway, give it back to the window that had it
        HWND fg = GetForegroundWindow();
        // (also when no window has the focus any more - then Windows allows it, too)
        if (!saved_.active && !userActivated_ && (!fg || OwnWindow(fg))) {
            ReturnFocus();
            for (HWND h : mine) PlaceInZOrder(h);
        }
        // Done a moment after the windows are back (longer if no window was on screen, e.g. in the
        // tray): check once more that AIMP did not change them afterwards
        if (placed_ && now - placed_ > 2500 && now - start_ > (saved_.windows.empty() ? 8000u : 0u)) {
            // ... unless the user has already used AIMP's windows (e.g. maximized it)
            // (a minimized one is still checked: it must not keep its place off the screen)
            LASTINPUTINFO li = {sizeof(li)};
            bool byUser = userActivated_ || (GetLastInputInfo(&li) && (LONG)(li.dwTime - placed_) > 0);
            mine = AimpWindows(GetCurrentProcessId(), true);
            used.assign(mine.size(), false);
            for (const SavedWindow& w : saved_.windows)
                for (size_t k = 0; k < mine.size(); k++)
                    if (!used[k] && ClassOf(mine[k]) == w.cls) {
                        used[k] = true;
                        bool onScreen = IsWindowVisible(mine[k]) && !IsIconic(mine[k]);
                        if (!byUser || (w.state == 1 && !onScreen)) Apply(mine[k], w);
                        break;
                    }
            End();
        }
    }

    // The first window that was not minimized (the main window), in the new process
    HWND FirstRestored(const std::vector<HWND>& mine) const {
        for (const SavedWindow& w : saved_.windows)
            if (w.state != 1)
                for (HWND h : mine)
                    if (ClassOf(h) == w.cls) return h;
        return nullptr;
    }

public:
    static WindowRestorer& Get() {
        static WindowRestorer r;
        return r;
    }

    // line = SavedWindows line from the resume file (see SaveWindowsLine)
    void Begin(const std::wstring& line) {
        End();
        if (!ParseWindowsLine(line, saved_)) return;
        done_.assign(saved_.windows.size(), false);
        normalShown_.clear();
        start_ = GetTickCount();
        firstDone_ = placed_ = 0;
        userActivated_ = minimizedEarly_ = raised_ = false;
        lastMinimize_ = 0;
        focusReturns_ = 0;
        Instance() = this;
        EnumThreadWindows(GetCurrentThreadId(), CloakExistingProc, (LPARAM)this);
        cbt_ = SetWindowsHookExW(WH_CBT, CbtProc, nullptr, GetCurrentThreadId());
        posHook_ = SetWindowsHookExW(WH_CALLWNDPROC, PosProc, nullptr, GetCurrentThreadId());
        showHook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, ShowProc, GetCurrentProcessId(), 0,
                                    WINEVENT_OUTOFCONTEXT);
        timer_ = SetTimer(nullptr, 0, 40, TimerProc);
        Log(std::wstring(L"Restart: AIMP was ") + (saved_.active ? L"in front" : L"in the background") + L", " +
            std::to_wstring(saved_.windows.size()) + L" window(s) to restore");
    }

    void End() {
        while (!cloaked_.empty()) Uncloak(cloaked_.back());
        if (timer_) KillTimer(nullptr, timer_);
        if (cbt_) UnhookWindowsHookEx(cbt_);
        if (posHook_) UnhookWindowsHookEx(posHook_);
        posHook_ = nullptr;
        if (showHook_) UnhookWinEvent(showHook_);
        timer_ = 0;
        cbt_ = nullptr;
        showHook_ = nullptr;
        if (Instance() == this) Instance() = nullptr;
    }
};

inline void EnableDpiAwareness() {
    // The picture must be taken in physical pixels, otherwise it is misplaced on scaled displays
    typedef BOOL(WINAPI * SetCtx)(HANDLE);
    HMODULE user = GetModuleHandleW(L"user32.dll");
    SetCtx set = user ? (SetCtx)GetProcAddress(user, "SetProcessDpiAwarenessContext") : nullptr;
    if (!set || !set((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) SetProcessDPIAware();
}

// ---------------------------------------------------------------------------------------------

// Starts the helper process (a 32-bit AIMP automatically gets the 32-bit rundll32)
// background = AIMP is not the foreground application (started without activation)
// hz = 0: the rate stays; patches: further AIMP.ini values for the bit-perfect mode (BpPatchText);
// exe = "-": only write the values once AIMP has closed, do not start it again
inline bool LaunchRestartHelper(uint32_t hz, const std::wstring& ini, const std::wstring& keyPath, bool seamless,
                                bool background, const std::wstring& patches = std::wstring(),
                                const std::wstring& exe = std::wstring()) {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\rundll32.exe\" \"" + ModulePath(ThisModule()) + L"\",RestartAimp " +
                       std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(hz) + L" \"" + ini + L"\" \"" +
                       (keyPath.empty() ? std::wstring(L"-") : keyPath) + L"\" \"" + (exe.empty() ? ModulePath(nullptr) : exe) +
                       L"\" " + (seamless ? L"seamless" : L"plain") + (background ? L" background" : L" front") + L" \"" +
                       (patches.empty() ? std::wstring(L"-") : patches) + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log(L"Restart helper could not be started (error " + std::to_wstring(GetLastError()) + L")");
        return false;
    }
    // AIMP is in front: the helper's still image may take the focus from it (see RunRestartHelper)
    if (!background) AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Body of the helper. Arguments: <pid> <hz> "<ini>" "<section\key>" "<aimp.exe>" [seamless|plain]
// [front|background] ["<patches>"]. "<aimp.exe>" = "-" does not start anything (tests, or only the
// bit-perfect values after AIMP closed). hz = 0: the rate is not written. patches: "Section\Key=n;..."
// (only the keys of the bit-perfect mode, see BpAllowedPatch).
inline void RunRestartHelper(const wchar_t* cmdLine) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine, &argc);
    if (!argv || argc < 5) { if (argv) LocalFree(argv); return; }
    DWORD pid = (DWORD)_wtoi(argv[0]);
    std::wstring hz = argv[1], ini = argv[2], keyPath = argv[3], exe = argv[4];
    bool seamless = argc >= 6 && std::wstring(argv[5]) == L"seamless";
    bool background = argc >= 7 && std::wstring(argv[6]) == L"background";
    std::wstring patchText = argc >= 8 ? argv[7] : L"-";
    LocalFree(argv);
    // The value goes into AIMP.ini as it is: only a plain number is accepted
    if (hz.empty() || hz.size() > 7 || hz.find_first_not_of(L"0123456789") != std::wstring::npos) return;
    std::vector<std::pair<std::wstring, std::wstring>> patches;  // "Section\Key", value
    for (size_t a = 0; patchText != L"-" && a < patchText.size();) {
        size_t b = patchText.find(L';', a);
        if (b == std::wstring::npos) b = patchText.size();
        std::wstring item = patchText.substr(a, b - a);
        size_t eq = item.find(L'=');
        if (eq != std::wstring::npos && BpAllowedPatch(item.substr(0, eq), item.substr(eq + 1)))
            patches.push_back({item.substr(0, eq), item.substr(eq + 1)});
        a = b + 1;
    }

    size_t slash = ini.find_last_of(L"\\/");
    if (slash != std::wstring::npos) Logger::Get().SetFile(ini.substr(0, slash + 1) + L"PreventResampling.log");

    // Physical pixels for the still image (scaled displays)
    EnableDpiAwareness();
    StillImage still;
    if (seamless && pid) {
        int n = still.Show(pid);
        Log(L"Restart helper: still image of " + std::to_wstring(n) + L" AIMP window(s)");
        if (n && !background && !still.TakeFocus()) Log(L"Restart helper: still image could not take the focus");
    }
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, ReadyEventName(pid).c_str());
    if (ready) { SetEvent(ready); CloseHandle(ready); }

    // Not any more once the user has chosen another window: only while the focus is with the still
    // image, an AIMP or the window Windows activated by itself right after the old AIMP closed
    DWORD exitTick = 0, newPid = 0;
    HWND autoFg = nullptr;
    bool userChose = false;
    auto keepFront = [&]() {
        if (background || userChose) return;
        HWND fg = GetForegroundWindow();
        DWORD fgPid = 0;
        if (fg) GetWindowThreadProcessId(fg, &fgPid);
        bool ours = !fg || fgPid == GetCurrentProcessId() || fgPid == pid || (newPid && fgPid == newPid) || fg == autoFg;
        if (!ours && !autoFg && exitTick && GetTickCount() - exitTick < 1000) ours = (autoFg = fg) != nullptr;
        if (ours) still.KeepInFront();
        else userChose = true;
    };
    HANDLE h = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    if (h) {
        bool exited = false;
        for (DWORD t = GetTickCount(); !exited && GetTickCount() - t < 30000;) {
            exited = WaitPumping(h, 30);
            if (exited) exitTick = GetTickCount() | 1;
            keepFront();
        }
        CloseHandle(h);
        if (!exited) {
            Log(L"Restart helper: AIMP did not close - cancelled");
            still.Remove(0);
            return;
        }
        WaitPumping(nullptr, 150);
    }
    auto write = [&](const std::wstring& path, const std::wstring& value) {
        size_t sep = path.find_last_of(L'\\');
        std::wstring section = sep == std::wstring::npos ? L"AIMPSoundOut" : path.substr(0, sep);
        std::wstring key = sep == std::wstring::npos ? path : path.substr(sep + 1);
        bool ok = false;
        for (int i = 0; i < 20 && !ok; i++) {  // the file may still be locked for a moment
            ok = PatchIni(ini, section, key, value);
            if (!ok) WaitPumping(nullptr, 150);
        }
        Log(L"Restart helper: " + path + L" = " + value + (ok ? L"" : L"  (could NOT write AIMP.ini)"));
    };
    if (hz != L"0" && keyPath != L"-") write(keyPath, hz);
    for (auto& p : patches) write(p.first, p.second);
    if (exe == L"-") { still.Remove(0); return; }

    // In the background: ask AIMP not to activate its window (the plugin in the new AIMP makes sure)
    STARTUPINFOW si = {sizeof(si)};
    if (background) {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWNOACTIVATE;
    }
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        Log(L"Restart helper: could not start AIMP (error " + std::to_wstring(GetLastError()) + L")");
        still.Remove(0);
        return;
    }
    // Created before AIMP runs: the plugin in the new AIMP signals it when its windows are back
    HANDLE placed = CreateEventW(nullptr, TRUE, FALSE, PlacedEventName(pi.dwProcessId).c_str());
    newPid = pi.dwProcessId;
    if (!background) AllowSetForegroundWindow(pi.dwProcessId);  // AIMP comes back in front
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    Log(L"Restart helper: starting AIMP");
    if (!still.Empty()) {
        // Keep the still image until AIMP's windows are back in their place, then fade it out.
        // Without the plugin's signal: 4 s after the first AIMP window appeared (at most 20 s).
        DWORD start = GetTickCount(), visibleAt = 0;
        bool signalled = false, exited = false;
        while (GetTickCount() - start < 20000) {
            if (placed && WaitForSingleObject(placed, 0) == WAIT_OBJECT_0) { signalled = true; break; }
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) { exited = true; break; }
            if (!visibleAt && !StillImage::VisibleWindows(pi.dwProcessId).empty()) visibleAt = GetTickCount();
            if (visibleAt && GetTickCount() - visibleAt > 4000) break;
            keepFront();
            WaitPumping(nullptr, 30);
        }
        keepFront();
        if (!exited && (signalled || visibleAt)) WaitPumping(nullptr, 200);  // let the new windows paint
        still.Remove(180);
        Log(signalled ? L"Restart helper: AIMP is back after " + std::to_wstring(GetTickCount() - start) + L" ms"
            : visibleAt ? L"Restart helper: AIMP is back after " + std::to_wstring(GetTickCount() - start) +
                              L" ms (windows not confirmed by the plugin)"
                        : std::wstring(L"Restart helper: no AIMP window appeared, still image removed"));
    }
    else if (placed) {
        // Nothing on screen (minimized or in the tray): only wait for the plugin to report it is done
        DWORD start = GetTickCount();
        int r = -1;
        while (r < 0 && GetTickCount() - start < 20000) {
            if (WaitForSingleObject(placed, 0) == WAIT_OBJECT_0) r = 0;
            else if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) r = 1;
            else WaitPumping(nullptr, 30);
        }
        Log(r == 0 ? L"Restart helper: AIMP is back after " + std::to_wstring(GetTickCount() - start) + L" ms (no window on screen)"
                   : std::wstring(L"Restart helper: AIMP started, windows not confirmed by the plugin"));
    }
    if (placed) CloseHandle(placed);
    CloseHandle(pi.hProcess);
}

}  // namespace win
}  // namespace ar
