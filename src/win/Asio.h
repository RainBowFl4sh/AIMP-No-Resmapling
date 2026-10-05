// ASIO: list the drivers from the registry and query/set their sample rate.
// IASIO is the public ASIO driver ABI (thiscall on x86, like all C++ methods with MSVC/MinGW).
// The driver's CLSID is used as the IID as well.
#pragma once
#include "../Common.h"

namespace ar {
namespace win {

class IASIO : public IUnknown {
public:
    virtual long init(void* sysHandle) = 0;
    virtual void getDriverName(char* name) = 0;
    virtual long getDriverVersion() = 0;
    virtual void getErrorMessage(char* string) = 0;
    virtual long start() = 0;
    virtual long stop() = 0;
    virtual long getChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual long getLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual long getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
    virtual long canSampleRate(double sampleRate) = 0;
    virtual long getSampleRate(double* sampleRate) = 0;
    virtual long setSampleRate(double sampleRate) = 0;
    // further methods (getClockSources ... outputReady) are not needed
};

const long ASE_OK = 0;
const long ASE_SUCCESS = 0x3f4847a0;

struct AsioDriver {
    std::wstring name;
    CLSID clsid;
};

inline std::vector<AsioDriver> ListAsioDrivers() {
    std::vector<AsioDriver> r;
    HKEY root;
    // 32-bit processes automatically see the 32-bit drivers (WOW6432Node), 64-bit ones the 64-bit drivers
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS) return r;
    wchar_t name[256];
    for (DWORD i = 0;; i++) {
        DWORD len = 256;
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t clsid[64] = {};
        DWORD sz = sizeof(clsid) - sizeof(wchar_t);
        HKEY k;
        if (RegOpenKeyExW(root, name, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        LONG ok = RegQueryValueExW(k, L"CLSID", nullptr, nullptr, (BYTE*)clsid, &sz);
        RegCloseKey(k);
        AsioDriver d;
        if (ok == ERROR_SUCCESS && SUCCEEDED(CLSIDFromString(clsid, &d.clsid))) {
            d.name = name;
            r.push_back(d);
        }
    }
    RegCloseKey(root);
    return r;
}

// Finds the driver from the setting or from AIMP's output ("ASIO: <driver name>")
inline bool FindAsioDriver(const std::wstring& configured, const std::wstring& aimpOutput, AsioDriver& out) {
    auto all = ListAsioDrivers();
    if (!configured.empty()) {
        for (auto& d : all)
            if (EqualsI(d.name, configured)) { out = d; return true; }
        for (auto& d : all)
            if (ContainsI(d.name, configured)) { out = d; return true; }
        return false;
    }
    size_t best = 0;
    for (auto& d : all)
        if (d.name.size() > best && ContainsI(aimpOutput, d.name)) { out = d; best = d.name.size(); }
    return best > 0;
}

// Opens a driver for short queries. ASIO drivers are apartment-threaded COM objects:
// the calling thread must be initialised with COINIT_APARTMENTTHREADED.
class AsioSession {
    IASIO* drv_ = nullptr;

public:
    explicit AsioSession(const AsioDriver& d) {
        if (FAILED(CoCreateInstance(d.clsid, nullptr, CLSCTX_INPROC_SERVER, d.clsid, (void**)&drv_)) || !drv_) {
            drv_ = nullptr;
            Log(L"ASIO: cannot load driver '" + d.name + L"'");
            return;
        }
        if (!drv_->init(GetDesktopWindow())) {
            char msg[128] = {};
            drv_->getErrorMessage(msg);
            Log(L"ASIO: init() failed (" + FromAnsi(msg) + L") - the driver may be in use by AIMP");
            drv_->Release();
            drv_ = nullptr;
        }
    }
    ~AsioSession() { if (drv_) drv_->Release(); }
    AsioSession(const AsioSession&) = delete;
    AsioSession& operator=(const AsioSession&) = delete;

    bool Ok() const { return drv_ != nullptr; }
    bool Can(uint32_t hz) { return drv_ && drv_->canSampleRate((double)hz) == ASE_OK; }
    uint32_t Rate() {
        double r = 0;
        return (drv_ && drv_->getSampleRate(&r) == ASE_OK) ? (uint32_t)(r + 0.5) : 0;
    }
    bool SetRate(uint32_t hz) {
        if (!drv_) return false;
        long e = drv_->setSampleRate((double)hz);
        return e == ASE_OK || e == ASE_SUCCESS;
    }
};

}  // namespace win
}  // namespace ar
