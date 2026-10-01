#pragma once
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include "Common.h"
#include "PolicyConfig.h"

namespace ar {
using Microsoft::WRL::ComPtr;

// PKEY_AudioEngine_DeviceFormat = "Standardformat" im Windows-Soundmenü
static const PROPERTYKEY KEY_DeviceFormat =
    {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

struct Endpoint {
    std::wstring id, name;
    // Voicemeeter-/VB-Audio-Geräte erkennt man am Namen (Hersteller steht immer drin)
    bool IsVB() const { return ContainsI(name, L"voicemeeter") || ContainsI(name, L"vb-audio"); }
};

inline ComPtr<IMMDeviceEnumerator> Enumerator() {
    ComPtr<IMMDeviceEnumerator> e;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
    return e;
}

inline bool ToEndpoint(IMMDevice* d, Endpoint& out) {
    LPWSTR id = nullptr;
    if (FAILED(d->GetId(&id))) return false;
    out.id = id;
    CoTaskMemFree(id);
    ComPtr<IPropertyStore> ps;
    if (FAILED(d->OpenPropertyStore(STGM_READ, &ps))) return false;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) out.name = v.pwszVal;
    PropVariantClear(&v);
    return true;
}

inline bool DefaultRender(Endpoint& out) {
    auto en = Enumerator();
    if (!en) return false;
    ComPtr<IMMDevice> d;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d))) return false;
    return ToEndpoint(d.Get(), out);
}

inline std::vector<Endpoint> ListRender() {
    std::vector<Endpoint> r;
    auto en = Enumerator();
    if (!en) return r;
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return r;
    UINT n = 0;
    col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        ComPtr<IMMDevice> d;
        Endpoint e;
        if (SUCCEEDED(col->Item(i, &d)) && ToEndpoint(d.Get(), e)) r.push_back(e);
    }
    return r;
}

inline bool FindByName(const std::wstring& part, Endpoint& out) {
    for (auto& e : ListRender())
        if (ContainsI(e.name, part)) { out = e; return true; }
    return false;
}

inline bool GetDeviceFormat(const std::wstring& id, WAVEFORMATEXTENSIBLE& fmt) {
    auto en = Enumerator();
    if (!en) return false;
    ComPtr<IMMDevice> d;
    if (FAILED(en->GetDevice(id.c_str(), &d))) return false;
    ComPtr<IPropertyStore> ps;
    if (FAILED(d->OpenPropertyStore(STGM_READ, &ps))) return false;
    PROPVARIANT v;
    PropVariantInit(&v);
    bool ok = false;
    if (SUCCEEDED(ps->GetValue(KEY_DeviceFormat, &v)) && v.vt == VT_BLOB &&
        v.blob.cbSize >= sizeof(WAVEFORMATEX)) {
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

// Fragt den Treiber, ob er dieses Format im Exclusive-Modus kann (= echte Hardware-Fähigkeit)
inline bool IsSupportedExclusive(const std::wstring& id, WAVEFORMATEXTENSIBLE f) {
    auto en = Enumerator();
    if (!en) return false;
    ComPtr<IMMDevice> d;
    if (FAILED(en->GetDevice(id.c_str(), &d))) return false;
    ComPtr<IAudioClient> ac;
    if (FAILED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)ac.GetAddressOf()))) return false;
    return ac->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, (WAVEFORMATEX*)&f, nullptr) == S_OK;
}

inline bool SetDeviceFormat(const std::wstring& id, WAVEFORMATEXTENSIBLE f) {
    ComPtr<IPolicyConfig> pc;
    if (FAILED(CoCreateInstance(CLSID_PolicyConfigClient_ar, nullptr, CLSCTX_ALL, IID_IPolicyConfig_ar,
                                (void**)pc.GetAddressOf())))
        return false;
    HRESULT hr = pc->SetDeviceFormat(id.c_str(), (WAVEFORMATEX*)&f, (WAVEFORMATEX*)&f);
    if (FAILED(hr)) Log(L"SetDeviceFormat Fehler 0x%08X", (unsigned)hr);
    return SUCCEEDED(hr);
}

}  // namespace ar
