// Restarting AIMP with a new output sample rate (EXPERIMENTAL).
//
// AIMP reads AIMPSoundOut\DeviceFreq only at start-up (or from its own settings page); there is no
// plugin interface to change it at runtime. Therefore:
//   1. The plugin starts "rundll32 PreventResampling.dll,RestartAimp ..." and closes AIMP.
//   2. Seamless mode: the helper first takes a picture of AIMP's visible windows and shows it at
//      exactly the same place and z-order ("freeze frame"), so AIMP seems to stay open.
//   3. The helper waits until AIMP has exited (AIMP still writes its settings while closing), writes
//      the new rate into AIMP.ini and starts AIMP again.
//   4. As soon as the new AIMP window is visible, windows that were maximized before are maximized
//      again (if AIMP did not do it itself) and the still image fades out.
//   5. On start-up the plugin continues the track at the same position.
#pragma once
#include <shellapi.h>

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
            s.ghost = CreateWindowExW(ex, L"PreventResamplingStillImage", L"", WS_POPUP, s.rc.left, s.rc.top,
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

inline std::wstring ClassOf(HWND h) {
    wchar_t cls[128] = {};
    GetClassNameW(h, cls, 128);
    return cls;
}

// Window classes of the process's maximized windows (AIMP's main form when it is maximized)
inline std::vector<std::wstring> MaximizedWindows(DWORD pid) {
    std::vector<std::wstring> r;
    for (HWND h : StillImage::VisibleWindows(pid))
        if (IsZoomed(h)) r.push_back(ClassOf(h));
    return r;
}

// Maximizes the new process's windows of these classes again if AIMP brought them back in normal
// size. Waits up to 2 s for each window to appear.
inline void RestoreMaximized(DWORD pid, const std::vector<std::wstring>& classes) {
    for (const auto& cls : classes) {
        HWND found = nullptr;
        for (int i = 0; i < 50 && !found; i++) {
            for (HWND h : StillImage::VisibleWindows(pid))
                if (ClassOf(h) == cls) { found = h; break; }
            if (!found) WaitPumping(nullptr, 40);
        }
        if (found && !IsZoomed(found)) {
            ShowWindow(found, SW_MAXIMIZE);
            Log(L"Restart helper: " + cls + L" maximized again");
        }
    }
}

inline void EnableDpiAwareness() {
    // The picture must be taken in physical pixels, otherwise it is misplaced on scaled displays
    typedef BOOL(WINAPI * SetCtx)(HANDLE);
    HMODULE user = GetModuleHandleW(L"user32.dll");
    SetCtx set = user ? (SetCtx)GetProcAddress(user, "SetProcessDpiAwarenessContext") : nullptr;
    if (!set || !set((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */)) SetProcessDPIAware();
}

// ---------------------------------------------------------------------------------------------

// Starts the helper process (a 32-bit AIMP automatically gets the 32-bit rundll32)
inline bool LaunchRestartHelper(uint32_t hz, const std::wstring& ini, const std::wstring& keyPath, bool seamless) {
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\rundll32.exe\" \"" + ModulePath(ThisModule()) + L"\",RestartAimp " +
                       std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(hz) + L" \"" + ini + L"\" \"" +
                       keyPath + L"\" \"" + ModulePath(nullptr) + L"\" " + (seamless ? L"seamless" : L"plain");
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log(L"Restart helper could not be started (error " + std::to_wstring(GetLastError()) + L")");
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Body of the helper. Arguments: <pid> <hz> "<ini>" "<section\key>" "<aimp.exe>" [seamless|plain]
// "<aimp.exe>" = "-" does not start anything (used by the tests).
inline void RunRestartHelper(const wchar_t* cmdLine) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine, &argc);
    if (!argv || argc < 5) { if (argv) LocalFree(argv); return; }
    DWORD pid = (DWORD)_wtoi(argv[0]);
    std::wstring hz = argv[1], ini = argv[2], keyPath = argv[3], exe = argv[4];
    bool seamless = argc >= 6 && std::wstring(argv[5]) == L"seamless";
    LocalFree(argv);
    // The value goes into AIMP.ini as it is: only a plain number is accepted
    if (hz.empty() || hz.size() > 7 || hz.find_first_not_of(L"0123456789") != std::wstring::npos) return;

    size_t slash = ini.find_last_of(L"\\/");
    if (slash != std::wstring::npos) Logger::Get().SetFile(ini.substr(0, slash + 1) + L"PreventResampling.log");

    StillImage still;
    if (seamless && pid) {
        EnableDpiAwareness();
        int n = still.Show(pid);
        Log(L"Restart helper: still image of " + std::to_wstring(n) + L" AIMP window(s)");
    }
    std::vector<std::wstring> maximized = pid ? MaximizedWindows(pid) : std::vector<std::wstring>();
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, ReadyEventName(pid).c_str());
    if (ready) { SetEvent(ready); CloseHandle(ready); }

    HANDLE h = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    if (h) {
        bool exited = WaitPumping(h, 30000);
        CloseHandle(h);
        if (!exited) {
            Log(L"Restart helper: AIMP did not close - cancelled");
            still.Remove(0);
            return;
        }
        WaitPumping(nullptr, 150);
    }
    size_t sep = keyPath.find_last_of(L'\\');
    std::wstring section = sep == std::wstring::npos ? L"AIMPSoundOut" : keyPath.substr(0, sep);
    std::wstring key = sep == std::wstring::npos ? keyPath : keyPath.substr(sep + 1);
    bool ok = false;
    for (int i = 0; i < 20 && !ok; i++) {  // the file may still be locked for a moment
        ok = PatchIni(ini, section, key, hz);
        if (!ok) WaitPumping(nullptr, 150);
    }
    Log(L"Restart helper: " + keyPath + L" = " + hz + (ok ? L"" : L"  (could NOT write AIMP.ini)"));
    if (exe == L"-") { still.Remove(0); return; }

    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"\"" + exe + L"\"";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        Log(L"Restart helper: could not start AIMP (error " + std::to_wstring(GetLastError()) + L")");
        still.Remove(0);
        return;
    }
    CloseHandle(pi.hThread);
    Log(L"Restart helper: starting AIMP");
    if (!still.Empty() || !maximized.empty()) {
        // Keep the still image until the new AIMP window is on screen, then fade it out
        DWORD start = GetTickCount();
        bool visible = false;
        while (GetTickCount() - start < 20000) {
            if (!StillImage::VisibleWindows(pi.dwProcessId).empty()) { visible = true; break; }
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
            WaitPumping(nullptr, 40);
        }
        if (visible) {
            RestoreMaximized(pi.dwProcessId, maximized);
            WaitPumping(nullptr, 250);  // let the new window paint
        }
        still.Remove(180);
        Log(visible ? L"Restart helper: AIMP is back after " + std::to_wstring(GetTickCount() - start) + L" ms"
                    : std::wstring(L"Restart helper: no AIMP window appeared, still image removed"));
    }
    CloseHandle(pi.hProcess);
}

}  // namespace win
}  // namespace ar
