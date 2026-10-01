#pragma once
#include "Common.h"

namespace ar {

class Voicemeeter {
    typedef long(__stdcall* T_Login)(void);
    typedef long(__stdcall* T_Logout)(void);
    typedef long(__stdcall* T_GetType)(long*);
    typedef long(__stdcall* T_GetStrW)(char*, wchar_t*);
    typedef long(__stdcall* T_SetFloat)(char*, float);
    typedef long(__stdcall* T_Dirty)(void);

    HMODULE h_ = nullptr;
    T_Login login_ = nullptr;
    T_Logout logout_ = nullptr;
    T_GetType getType_ = nullptr;
    T_GetStrW getStrW_ = nullptr;
    T_SetFloat setFloat_ = nullptr;
    T_Dirty dirty_ = nullptr;
    bool loggedIn_ = false;
    long type_ = 0;  // 1 = Standard, 2 = Banana, 3 = Potato

    static std::wstring InstallDir() {
        HKEY k;
        const wchar_t* sub = L"SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
                             L"VB:Voicemeeter {17359A74-1236-5467}";
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return L"";
        wchar_t buf[2048] = {};
        DWORD sz = sizeof(buf) - sizeof(wchar_t), type = 0;
        LONG r = RegQueryValueExW(k, L"UninstallString", nullptr, &type, (BYTE*)buf, &sz);
        RegCloseKey(k);
        if (r != ERROR_SUCCESS) return L"";
        std::wstring s = buf, path;
        if (!s.empty() && s[0] == L'"') {
            size_t e = s.find(L'"', 1);
            path = s.substr(1, e == std::wstring::npos ? std::wstring::npos : e - 1);
        } else {
            size_t e = Lower(s).find(L".exe");
            path = (e == std::wstring::npos) ? s : s.substr(0, e + 4);
        }
        size_t slash = path.find_last_of(L'\\');
        return slash == std::wstring::npos ? L"" : path.substr(0, slash);
    }

public:
    Voicemeeter() = default;
    Voicemeeter(const Voicemeeter&) = delete;
    ~Voicemeeter() { Close(); }

    // true = installiert, läuft und Login erfolgreich. Gestartet wird Voicemeeter NIE automatisch.
    bool Open() {
        std::wstring dir = InstallDir();
        if (dir.empty()) return false;  // nicht installiert -> Modul bleibt inaktiv
        h_ = LoadLibraryW((dir + L"\\VoicemeeterRemote64.dll").c_str());
        if (!h_) { Log(L"VoicemeeterRemote64.dll nicht ladbar"); return false; }
        login_ = (T_Login)GetProcAddress(h_, "VBVMR_Login");
        logout_ = (T_Logout)GetProcAddress(h_, "VBVMR_Logout");
        getType_ = (T_GetType)GetProcAddress(h_, "VBVMR_GetVoicemeeterType");
        getStrW_ = (T_GetStrW)GetProcAddress(h_, "VBVMR_GetParameterStringW");
        setFloat_ = (T_SetFloat)GetProcAddress(h_, "VBVMR_SetParameterFloat");
        dirty_ = (T_Dirty)GetProcAddress(h_, "VBVMR_IsParametersDirty");
        if (!login_ || !logout_ || !getType_ || !getStrW_ || !setFloat_ || !dirty_) { Close(); return false; }
        long r = login_();
        if (r < 0) { Log(L"Voicemeeter Login Fehler %ld", r); Close(); return false; }
        loggedIn_ = true;
        if (r == 1) { Log(L"Voicemeeter installiert, aber nicht gestartet"); Close(); return false; }
        getType_(&type_);
        Sleep(100);
        dirty_();  // Parameter-Cache aktualisieren
        return type_ >= 1 && type_ <= 3;
    }

    void Close() {
        if (loggedIn_ && logout_) logout_();
        loggedIn_ = false;
        if (h_) { FreeLibrary(h_); h_ = nullptr; }
    }

    long Type() const { return type_; }
    const wchar_t* TypeName() const {
        return type_ == 1 ? L"Voicemeeter" : type_ == 2 ? L"Banana" : type_ == 3 ? L"Potato" : L"?";
    }

    // Namen der Geräte an den Hardware-Ausgängen A1..An (Standard: 1, Banana: 3, Potato: 5)
    std::vector<std::wstring> BusDeviceNames() {
        std::vector<std::wstring> r;
        static const int count[] = {0, 1, 3, 5};
        if (!loggedIn_ || type_ < 1 || type_ > 3) return r;
        for (int i = 0; i < count[type_]; i++) {
            char key[64];
            sprintf_s(key, "Bus[%d].device.name", i);
            wchar_t out[512] = {};
            if (getStrW_(key, out) == 0 && out[0]) r.push_back(out);
        }
        return r;
    }

    void RestartEngine() {
        if (loggedIn_) { char k[] = "Command.Restart"; setFloat_(k, 1.0f); }
    }
};

}  // namespace ar
