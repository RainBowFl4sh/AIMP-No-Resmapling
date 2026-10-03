// Windows audio devices: enumeration and the "default format" (shared-mode rate) via IPolicyConfig.
#pragma once
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>

#include "../Common.h"

namespace ar {
namespace win {

// Undocumented COM interface (Windows 7 to 11), also used by SoundSwitch, AudioDeviceCmdlets
// and others. The order of the methods must NOT be changed.
struct IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

static const CLSID CLSID_PolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
static const IID IID_IPolicyConfig = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

// Own copies of the GUIDs/keys so no import libraries are needed (same for MinGW and MSVC)
static const CLSID CLSID_MMDeviceEnumerator_ = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID IID_IMMDeviceEnumerator_ = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID IID_IAudioClient_ = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const PROPERTYKEY KEY_FriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
// PKEY_AudioEngine_DeviceFormat = "Default Format" in the Windows sound settings
static const PROPERTYKEY KEY_DeviceFormat = {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

template <typename T>
class Com {
    T* p_ = nullptr;

public:
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { if (p_) p_->Release(); }
    T* operator->() const { return p_; }
    T* Get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void** Out() { if (p_) { p_->Release(); p_ = nullptr; } return (void**)&p_; }
    T** OutT() { return (T**)Out(); }
};

struct Endpoint {
    std::wstring id, name;
    // Voicemeeter/VB-Audio devices are recognised by name (the vendor is always part of it)
    bool IsVB() const { return ContainsI(name, L"voicemeeter") || ContainsI(name, L"vb-audio"); }
};

inline std::wstring Hex(HRESULT hr);

inline bool Enumerator(Com<IMMDeviceEnumerator>& e) {
    HRESULT hr = CoCreateInstance(CLSID_MMDeviceEnumerator_, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator_, e.Out());
    if (FAILED(hr)) Log(L"MMDevice enumerator not available (" + Hex(hr) + L")");
    return SUCCEEDED(hr);
}

inline bool ToEndpoint(IMMDevice* d, Endpoint& out) {
    LPWSTR id = nullptr;
    if (FAILED(d->GetId(&id))) return false;
    out.id = id;
    CoTaskMemFree(id);
    Com<IPropertyStore> ps;
    if (FAILED(d->OpenPropertyStore(STGM_READ, ps.OutT()))) return false;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(ps->GetValue(KEY_FriendlyName, &v)) && v.vt == VT_LPWSTR) out.name = v.pwszVal;
    PropVariantClear(&v);
    return true;
}

inline bool DefaultRender(Endpoint& out) {
    Com<IMMDeviceEnumerator> en;
    if (!Enumerator(en)) return false;
    Com<IMMDevice> d;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, d.OutT()))) return false;
    return ToEndpoint(d.Get(), out);
}

inline std::vector<Endpoint> ListRender() {
    std::vector<Endpoint> r;
    Com<IMMDeviceEnumerator> en;
    if (!Enumerator(en)) return r;
    Com<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, col.OutT()))) return r;
    UINT n = 0;
    col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        Com<IMMDevice> d;
        Endpoint e;
        if (SUCCEEDED(col->Item(i, d.OutT())) && ToEndpoint(d.Get(), e)) r.push_back(e);
    }
    return r;
}

// Voicemeeter names an A1 device e.g. "FiiO ASIO Driver", Windows calls it "Speakers (FiiO KA11)".
// Matching uses distinctive words (vendor/model); generic words are ignored.
inline bool NamesMatch(const std::wstring& busName, const std::wstring& endpointName) {
    if (busName.empty() || endpointName.empty()) return false;
    if (ContainsI(endpointName, busName) || ContainsI(busName, endpointName)) return true;
    static const wchar_t* generic[] = {L"asio", L"driver", L"usb", L"audio", L"device", L"wdm", L"mme", L"ks",
                                       L"output", L"out", L"speakers", L"speaker", L"lautsprecher", L"headphones",
                                       L"kopfhoerer", L"kopfhörer", L"digital", L"high", L"definition", L"the",
                                       L"and", L"for", L"sound", L"card", L"control", L"panel", L"v2", L"2.0"};
    std::vector<std::wstring> tokens;
    std::wstring cur;
    for (wchar_t c : busName + L" ") {
        if (iswalnum(c)) cur += (wchar_t)towlower(c);
        else {
            bool skip = cur.size() < 3;
            for (auto* g : generic) if (cur == g) skip = true;
            if (!skip) tokens.push_back(cur);
            cur.clear();
        }
    }
    if (tokens.empty()) return false;
    for (auto& t : tokens) if (!ContainsI(endpointName, t)) return false;
    return true;
}

inline bool FindByName(const std::vector<Endpoint>& all, const std::wstring& part, Endpoint& out) {
    for (auto& e : all)  // exact name first
        if (Lower(e.name) == Lower(part)) { out = e; return true; }
    for (auto& e : all)
        if (ContainsI(e.name, part)) { out = e; return true; }
    return false;
}

// Finds the Windows device whose name appears in AIMP's output name
// (e.g. "WASAPI: Speakers (Realtek(R) Audio)"). The longest match wins.
inline bool FindInText(const std::vector<Endpoint>& all, const std::wstring& text, Endpoint& out) {
    size_t best = 0;
    for (auto& e : all)
        if (!e.name.empty() && e.name.size() > best && ContainsI(text, e.name)) { out = e; best = e.name.size(); }
    return best > 0;
}

inline bool GetDeviceFormat(const std::wstring& id, WAVEFORMATEXTENSIBLE& fmt) {
    Com<IMMDeviceEnumerator> en;
    if (!Enumerator(en)) return false;
    Com<IMMDevice> d;
    if (FAILED(en->GetDevice(id.c_str(), d.OutT()))) return false;
    Com<IPropertyStore> ps;
    if (FAILED(d->OpenPropertyStore(STGM_READ, ps.OutT()))) return false;
    PROPVARIANT v;
    PropVariantInit(&v);
    bool ok = false;
    if (SUCCEEDED(ps->GetValue(KEY_DeviceFormat, &v)) && v.vt == VT_BLOB && v.blob.cbSize >= sizeof(WAVEFORMATEX)) {
        ZeroMemory(&fmt, sizeof(fmt));
        memcpy(&fmt, v.blob.pBlobData, (std::min)((size_t)v.blob.cbSize, sizeof(fmt)));
        ok = true;
    }
    PropVariantClear(&v);
    return ok;
}

inline WAVEFORMATEXTENSIBLE WithRate(WAVEFORMATEXTENSIBLE f, uint32_t hz) {
    f.Format.nSamplesPerSec = hz;
    f.Format.nAvgBytesPerSec = hz * f.Format.nBlockAlign;
    return f;
}

// Asks the driver whether it supports this format in exclusive mode (= real hardware capability).
// 1 = yes, 0 = no, -1 = unknown (e.g. the device is held exclusively by Voicemeeter).
inline int SupportsExclusive(const std::wstring& id, WAVEFORMATEXTENSIBLE f) {
    Com<IMMDeviceEnumerator> en;
    if (!Enumerator(en)) return -1;
    Com<IMMDevice> d;
    if (FAILED(en->GetDevice(id.c_str(), d.OutT()))) return -1;
    Com<IAudioClient> ac;
    if (FAILED(d->Activate(IID_IAudioClient_, CLSCTX_ALL, nullptr, ac.Out()))) return -1;
    HRESULT hr = ac->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*)&f, nullptr);
    if (hr == S_OK) return 1;
    if (hr == S_FALSE || hr == AUDCLNT_E_UNSUPPORTED_FORMAT) return 0;
    return -1;
}

// Vista variant of the interface (without ResetDeviceFormat) as a fallback
struct IPolicyConfigVista : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
};
static const CLSID CLSID_PolicyConfigVistaClient = {0x294935CE, 0xF637, 0x4E7C, {0xA4, 0x1B, 0xAB, 0x25, 0x54, 0x60, 0xB8, 0x62}};
static const IID IID_IPolicyConfigVista = {0x568b9108, 0x44bf, 0x40b4, {0x90, 0x06, 0x86, 0xaf, 0xe5, 0xb5, 0xa6, 0x20}};
static const GUID SUBTYPE_IEEE_FLOAT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

inline std::wstring Hex(HRESULT hr) {
    wchar_t buf[16];
    swprintf(buf, 16, L"0x%08X", (unsigned)hr);
    return buf;
}

// Mix format of the Windows mixer (always 32-bit float) matching the new device format
inline WAVEFORMATEXTENSIBLE MixFormatFor(const WAVEFORMATEXTENSIBLE& dev, const WAVEFORMATEX* current) {
    WAVEFORMATEXTENSIBLE m;
    ZeroMemory(&m, sizeof(m));
    if (current && current->wFormatTag == WAVE_FORMAT_EXTENSIBLE && current->cbSize >= 22)
        memcpy(&m, current, sizeof(m));
    else {
        m.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        m.Format.cbSize = 22;
        m.Format.nChannels = dev.Format.nChannels;
        m.dwChannelMask = dev.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE ? dev.dwChannelMask
                                                                          : (dev.Format.nChannels == 1 ? 4u : 3u);
        m.SubFormat = SUBTYPE_IEEE_FLOAT;
        m.Format.wBitsPerSample = 32;
        m.Samples.wValidBitsPerSample = 32;
    }
    m.Format.nSamplesPerSec = dev.Format.nSamplesPerSec;
    m.Format.nBlockAlign = (WORD)(m.Format.nChannels * m.Format.wBitsPerSample / 8);
    m.Format.nAvgBytesPerSec = m.Format.nSamplesPerSec * m.Format.nBlockAlign;
    return m;
}

// Sets a device's "default format" (shared-mode rate) and verifies the result.
inline bool SetDeviceFormat(const std::wstring& id, WAVEFORMATEXTENSIBLE f) {
    HRESULT hr = E_FAIL;
    Com<IPolicyConfig> pc;
    HRESULT co = CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL, IID_IPolicyConfig, pc.Out());
    if (SUCCEEDED(co)) {
        WAVEFORMATEX* cur = nullptr;
        pc->GetMixFormat(id.c_str(), &cur);
        WAVEFORMATEXTENSIBLE mix = MixFormatFor(f, cur);
        if (cur) CoTaskMemFree(cur);
        hr = pc->SetDeviceFormat(id.c_str(), (WAVEFORMATEX*)&f, (WAVEFORMATEX*)&mix);
        if (FAILED(hr)) Log(L"  IPolicyConfig::SetDeviceFormat: " + Hex(hr));
    } else {
        Log(L"  IPolicyConfig not available (" + Hex(co) + L")");
    }
    if (FAILED(hr)) {
        Com<IPolicyConfigVista> pv;
        if (SUCCEEDED(CoCreateInstance(CLSID_PolicyConfigVistaClient, nullptr, CLSCTX_ALL, IID_IPolicyConfigVista, pv.Out()))) {
            WAVEFORMATEX* cur = nullptr;
            pv->GetMixFormat(id.c_str(), &cur);
            WAVEFORMATEXTENSIBLE mix = MixFormatFor(f, cur);
            if (cur) CoTaskMemFree(cur);
            hr = pv->SetDeviceFormat(id.c_str(), (WAVEFORMATEX*)&f, (WAVEFORMATEX*)&mix);
            if (FAILED(hr)) Log(L"  IPolicyConfigVista::SetDeviceFormat: " + Hex(hr));
        }
    }
    // Did it really change? (Windows sometimes reports success without changing anything)
    WAVEFORMATEXTENSIBLE check;
    for (int i = 0; i < 20; i++) {
        if (GetDeviceFormat(id, check) && check.Format.nSamplesPerSec == f.Format.nSamplesPerSec) return true;
        SleepMs(25);
    }
    if (SUCCEEDED(hr)) Log(L"  Windows accepted the call but did not change the format");
    return false;
}

}  // namespace win
}  // namespace ar
