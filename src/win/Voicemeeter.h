// Voicemeeter Remote API (VoicemeeterRemote.dll / VoicemeeterRemote64.dll).
// VB-Audio says login/logout should happen once per process, therefore there is one shared,
// thread-safe session for the whole lifetime of the plugin. Exception: if Voicemeeter is started
// after the login, the session does not see it - then the plugin logs in again (see Type()).
#pragma once
#include "../Common.h"

namespace ar {
namespace win {

struct VmDevice {
    long type = 0;  // 1 = MME, 3 = WDM, 4 = KS, 5 = ASIO
    std::wstring name;
    std::wstring Label() const {
        const wchar_t* t = type == 1 ? L"MME" : type == 3 ? L"WDM" : type == 4 ? L"KS" : type == 5 ? L"ASIO" : L"?";
        return std::wstring(t) + L": " + name;
    }
    const char* ParamSuffix() const {
        return type == 1 ? "mme" : type == 3 ? "wdm" : type == 4 ? "ks" : type == 5 ? "asio" : nullptr;
    }
};

class Voicemeeter {
    typedef long(__stdcall* T_Void)(void);
    typedef long(__stdcall* T_GetLong)(long*);
    typedef long(__stdcall* T_GetFloat)(char*, float*);
    typedef long(__stdcall* T_GetStrW)(char*, unsigned short*);
    typedef long(__stdcall* T_SetFloat)(char*, float);
    typedef long(__stdcall* T_SetStrW)(char*, unsigned short*);
    typedef long(__stdcall* T_DevDescW)(long, long*, unsigned short*, unsigned short*);

    std::recursive_mutex m_;
    HMODULE h_ = nullptr;
    bool tried_ = false, loggedIn_ = false;
    int64_t lastLogin_ = 0;
    T_Void login_ = nullptr, logout_ = nullptr, dirty_ = nullptr, outNum_ = nullptr;
    T_GetLong getType_ = nullptr, getVersion_ = nullptr;
    T_GetFloat getFloat_ = nullptr;
    T_GetStrW getStrW_ = nullptr;
    T_SetFloat setFloat_ = nullptr;
    T_SetStrW setStrW_ = nullptr;
    T_DevDescW outDesc_ = nullptr;

    static std::wstring ReadUninstallDir(REGSAM view, const wchar_t* sub) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return L"";
        wchar_t buf[2048] = {};
        DWORD sz = sizeof(buf) - sizeof(wchar_t);
        LONG r = RegQueryValueExW(k, L"UninstallString", nullptr, nullptr, (BYTE*)buf, &sz);
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

    static std::wstring InstallDir() {
        const wchar_t* key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\VB:Voicemeeter {17359A74-1236-5467}";
        std::wstring d = ReadUninstallDir(KEY_WOW64_32KEY, key);
        if (d.empty()) d = ReadUninstallDir(KEY_WOW64_64KEY, key);
        return d;
    }

    bool Load() {
        if (tried_) return h_ != nullptr;
        tried_ = true;
        std::wstring dir = InstallDir();
        if (dir.empty()) return false;  // not installed -> module stays inactive
        const wchar_t* dll = sizeof(void*) == 8 ? L"\\VoicemeeterRemote64.dll" : L"\\VoicemeeterRemote.dll";
        h_ = LoadLibraryW((dir + dll).c_str());
        if (!h_) { Log(L"Voicemeeter: cannot load " + std::wstring(dll + 1)); return false; }
        login_ = (T_Void)GetProcAddress(h_, "VBVMR_Login");
        logout_ = (T_Void)GetProcAddress(h_, "VBVMR_Logout");
        dirty_ = (T_Void)GetProcAddress(h_, "VBVMR_IsParametersDirty");
        getType_ = (T_GetLong)GetProcAddress(h_, "VBVMR_GetVoicemeeterType");
        getVersion_ = (T_GetLong)GetProcAddress(h_, "VBVMR_GetVoicemeeterVersion");
        getFloat_ = (T_GetFloat)GetProcAddress(h_, "VBVMR_GetParameterFloat");
        getStrW_ = (T_GetStrW)GetProcAddress(h_, "VBVMR_GetParameterStringW");
        setFloat_ = (T_SetFloat)GetProcAddress(h_, "VBVMR_SetParameterFloat");
        setStrW_ = (T_SetStrW)GetProcAddress(h_, "VBVMR_SetParameterStringW");
        outNum_ = (T_Void)GetProcAddress(h_, "VBVMR_Output_GetDeviceNumber");
        outDesc_ = (T_DevDescW)GetProcAddress(h_, "VBVMR_Output_GetDeviceDescW");
        if (!login_ || !logout_ || !dirty_ || !getType_ || !getFloat_ || !getStrW_ || !setFloat_) {
            Log(L"Voicemeeter: Remote API incomplete");
            FreeLibrary(h_);
            h_ = nullptr;
            return false;
        }
        long r = login_();
        lastLogin_ = NowMs();
        if (r < 0) { Log(L"Voicemeeter: login error " + Num(r)); return false; }
        loggedIn_ = true;
        return true;
    }

    void Refresh() {
        // Refresh the parameter cache (waits at most ~100 ms)
        for (int i = 0; i < 10 && dirty_() > 0; i++) SleepMs(10);
    }

public:
    static Voicemeeter& Get() {
        static Voicemeeter v;
        return v;
    }

    void Shutdown() {
        std::lock_guard<std::recursive_mutex> g(m_);
        if (loggedIn_ && logout_) logout_();
        loggedIn_ = false;
        if (h_) FreeLibrary(h_);
        h_ = nullptr;
    }

    bool Installed() {
        std::lock_guard<std::recursive_mutex> g(m_);
        return Load();
    }

    // 0 = not running, 1 = Standard, 2 = Banana, 3 = Potato
    long Type() {
        std::lock_guard<std::recursive_mutex> g(m_);
        if (!Load()) return 0;
        long t = 0;
        bool ok = loggedIn_ && getType_(&t) == 0;
        if (!ok && NowMs() - lastLogin_ >= 3000) {
            // Not reachable: Voicemeeter may have been started after the login (at most every 3 s)
            if (loggedIn_) logout_();
            loggedIn_ = login_() >= 0;
            lastLogin_ = NowMs();
            ok = loggedIn_ && getType_(&t) == 0;
            if (ok) Log(L"Voicemeeter: connected (it was started after AIMP)");
        }
        if (!ok) return 0;
        if (t >= 4 && t <= 6) t -= 3;  // x64 variants
        return (t >= 1 && t <= 3) ? t : 0;
    }
    bool Running() { return Type() != 0; }

    static const wchar_t* TypeName(long t) {
        return t == 1 ? L"Voicemeeter" : t == 2 ? L"Voicemeeter Banana" : t == 3 ? L"Voicemeeter Potato" : L"-";
    }
    static int HardwareBusCount(long t) { return t == 1 ? 2 : t == 2 ? 3 : t == 3 ? 5 : 0; }

    std::wstring GetString(const char* param) {
        std::lock_guard<std::recursive_mutex> g(m_);
        if (!Running()) return L"";
        Refresh();
        unsigned short out[512] = {};
        if (getStrW_(const_cast<char*>(param), out) != 0) return L"";
        return std::wstring((const wchar_t*)out);
    }

    bool GetFloat(const char* param, float& v) {
        std::lock_guard<std::recursive_mutex> g(m_);
        if (!Running()) return false;
        Refresh();
        return getFloat_(const_cast<char*>(param), &v) == 0;
    }

    bool SetFloat(const char* param, float v) {
        std::lock_guard<std::recursive_mutex> g(m_);
        if (!Running()) return false;
        return setFloat_(const_cast<char*>(param), v) == 0;
    }

    bool SetString(const char* param, const std::wstring& v) {
        std::lock_guard<std::recursive_mutex> g(m_);
        if (!Running() || !setStrW_) return false;
        return setStrW_(const_cast<char*>(param), (unsigned short*)const_cast<wchar_t*>(v.c_str())) == 0;
    }

    // Devices on the hardware outputs: index 0 = A1, 1 = A2, ... (only outputs with a device)
    std::vector<std::pair<int, std::wstring>> BusDevices(bool allBuses) {
        std::vector<std::pair<int, std::wstring>> r;
        long t = Type();
        int count = allBuses ? HardwareBusCount(t) : (t ? 1 : 0);
        for (int i = 0; i < count; i++) {
            char key[64];
            snprintf(key, sizeof(key), "Bus[%d].device.name", i);
            std::wstring n = GetString(key);
            if (!n.empty()) r.push_back({i, n});
        }
        return r;
    }

    // Depending on the version values come in Hz or kHz
    static uint32_t ToHz(float v) {
        if (v <= 0) return 0;
        if (v < 1000.0f) v *= 1000.0f;
        return (uint32_t)(v + 0.5f);
    }

    // Current engine rate (= rate of A1)
    uint32_t EngineRate() {
        float sr = 0;
        return GetFloat("Bus[0].device.sr", sr) ? ToHz(sr) : 0;
    }

    uint32_t PreferredRate() {
        float sr = 0;
        return GetFloat("Option.sr", sr) ? ToHz(sr) : 0;
    }

    // Rates Voicemeeter accepts as "Preferred Main SampleRate"
    static bool EngineSupports(uint32_t hz) {
        switch (hz) {
            case 44100: case 48000: case 88200: case 96000: case 176400: case 192000: return true;
        }
        return false;
    }

    std::vector<VmDevice> OutputDevices() {
        std::vector<VmDevice> r;
        std::lock_guard<std::recursive_mutex> g(m_);
        if (!Load() || !outNum_ || !outDesc_) return r;
        long n = outNum_();
        for (long i = 0; i < n && i < 256; i++) {
            VmDevice d;
            unsigned short name[512] = {}, hw[512] = {};
            if (outDesc_(i, &d.type, name, hw) == 0) {
                d.name = (const wchar_t*)name;
                if (!d.name.empty()) r.push_back(d);
            }
        }
        return r;
    }

    bool SetBusDevice(int bus, const VmDevice& d) {
        const char* suffix = d.ParamSuffix();
        if (!suffix) return false;
        char key[64];
        snprintf(key, sizeof(key), "Bus[%d].device.%s", bus, suffix);
        return SetString(key, d.name);
    }

    bool Restart() { return SetFloat("Command.Restart", 1.0f); }

    // Waits until the engine runs with the requested rate after a restart (polls quickly)
    bool WaitForEngine(uint32_t hz, int firstPollMs, int timeoutMs) {
        int64_t end = NowMs() + timeoutMs;
        SleepMs(firstPollMs);
        while (NowMs() < end) {
            if (EngineRate() == hz) return true;
            SleepMs(40);
        }
        return false;
    }
};

}  // namespace win
}  // namespace ar
