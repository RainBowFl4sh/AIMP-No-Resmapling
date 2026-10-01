#pragma once
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace ar {

inline std::wstring DataDir() {
    wchar_t p[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, p))) {
        std::wstring d = std::wstring(p) + L"\\AIMP_AutoRate";
        CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }
    return L".";
}

inline void Log(const wchar_t* fmt, ...) {
    static std::mutex m;
    std::lock_guard<std::mutex> g(m);
    static std::wstring path = DataDir() + L"\\autorate.log";
    static bool first = true;
    if (first) {  // Log nicht endlos wachsen lassen
        first = false;
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) && a.nFileSizeLow > 1024 * 1024)
            DeleteFileW(path.c_str());
    }
    FILE* f = _wfopen(path.c_str(), L"a, ccs=UTF-8");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fwprintf(f, L"%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfwprintf(f, fmt, a);
    va_end(a);
    fwprintf(f, L"\n");
    fclose(f);
}

inline std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
// Teilstring-Suche ohne Beachtung der Gross-/Kleinschreibung
inline bool ContainsI(const std::wstring& hay, const std::wstring& needle) {
    if (needle.empty()) return false;
    return Lower(hay).find(Lower(needle)) != std::wstring::npos;
}

struct Config {
    bool enabled = true;
    bool useVoicemeeter = true;       // Voicemeeter-Kette automatisch auflösen
    bool restartVoicemeeter = true;   // Engine nach Formatwechsel neu starten
    int settleMs = 800;               // Wartezeit nach dem Umschalten
    std::wstring aimpDevice;          // leer = Windows-Standardgerät
    std::vector<std::wstring> extraDevices;  // zusätzlich mitzuschaltende Geräte (Namensteil)

    static Config Load() {
        Config c;
        std::wstring p = DataDir() + L"\\config.ini";
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
            WritePrivateProfileStringW(L"General", L"Enabled", L"1", p.c_str());
            WritePrivateProfileStringW(L"General", L"UseVoicemeeter", L"1", p.c_str());
            WritePrivateProfileStringW(L"General", L"RestartVoicemeeter", L"1", p.c_str());
            WritePrivateProfileStringW(L"General", L"SettleMs", L"800", p.c_str());
            WritePrivateProfileStringW(L"Devices", L"AimpDevice", L"", p.c_str());
            WritePrivateProfileStringW(L"Devices", L"ExtraDevices", L"", p.c_str());
        }
        c.enabled = GetPrivateProfileIntW(L"General", L"Enabled", 1, p.c_str()) != 0;
        c.useVoicemeeter = GetPrivateProfileIntW(L"General", L"UseVoicemeeter", 1, p.c_str()) != 0;
        c.restartVoicemeeter = GetPrivateProfileIntW(L"General", L"RestartVoicemeeter", 1, p.c_str()) != 0;
        c.settleMs = (int)GetPrivateProfileIntW(L"General", L"SettleMs", 800, p.c_str());
        wchar_t buf[1024];
        GetPrivateProfileStringW(L"Devices", L"AimpDevice", L"", buf, 1024, p.c_str());
        c.aimpDevice = buf;
        GetPrivateProfileStringW(L"Devices", L"ExtraDevices", L"", buf, 1024, p.c_str());
        std::wstring all = buf, cur;
        for (wchar_t ch : all + L";") {
            if (ch == L';') { if (!cur.empty()) c.extraDevices.push_back(cur); cur.clear(); }
            else cur += ch;
        }
        return c;
    }
};

}  // namespace ar
