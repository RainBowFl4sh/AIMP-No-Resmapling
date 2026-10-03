// AIMP's own output sample rate (main thread only).
//
// AIMP always plays with AIMPSoundOut\DeviceFreq and resamples everything to it. AIMP reads the
// value only at start-up or from its own settings page - there is no plugin interface to change it
// at runtime. If enabled, AIMP is therefore restarted with the new rate (win/Restart.h) and the
// track continues at the same position.
#pragma once
#include <ctime>

#include "AimpUtil.h"
#include "apiPlayer.h"

namespace ar {

inline bool IsAudioRate(long v) {
    switch (v) {
        case 22050: case 32000: case 44100: case 48000: case 88200: case 96000:
        case 176400: case 192000: case 352800: case 384000: return true;
    }
    return false;
}

struct IniRateKey {
    tstring path;   // "Section\Key" for IAIMPServiceConfig
    long value = 0; // value in the file
};

// Reads AIMP.ini (UTF-8 or UTF-16) and returns keys that look like the output sample rate.
// AIMPSoundOut\DeviceFreq (AIMP 5/6) comes first.
inline std::vector<IniRateKey> FindAimpRateKeys(const tstring& profileDir) {
    std::vector<IniRateKey> r;
    tstring path = profileDir + AR_T("AIMP.ini");
#ifdef _WIN32
    FILE* f = _wfopen(path.c_str(), L"rb");
#else
    FILE* f = fopen(path.c_str(), "rb");
#endif
    if (!f) return r;
    std::string raw;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && raw.size() < 8 * 1024 * 1024) raw.append(buf, n);
    fclose(f);

    tstring text;
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        for (size_t i = 2; i + 1 < raw.size(); i += 2) {  // UTF-16 LE (only BMP/ASCII matters)
            wchar_t c = (wchar_t)((unsigned char)raw[i] | ((unsigned char)raw[i + 1] << 8));
#ifdef _WIN32
            text += c;
#else
            text += (c < 128) ? (char)c : '?';
#endif
        }
    } else {
        size_t start = (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF) ? 3 : 0;
#ifdef _WIN32
        int len = MultiByteToWideChar(CP_UTF8, 0, raw.data() + start, (int)(raw.size() - start), nullptr, 0);
        text.resize(len);
        MultiByteToWideChar(CP_UTF8, 0, raw.data() + start, (int)(raw.size() - start), &text[0], len);
#else
        text = raw.substr(start);
#endif
    }

    tstring section;
    for (auto& line0 : Split(text, AR_T('\n'))) {
        tstring line = Trim(line0);
        if (line.empty() || line[0] == AR_T(';')) continue;
        if (line[0] == AR_T('[') && line.back() == AR_T(']')) { section = line.substr(1, line.size() - 2); continue; }
        size_t eq = line.find(AR_T('='));
        if (eq == tstring::npos) continue;
        tstring key = Trim(line.substr(0, eq)), val = Trim(line.substr(eq + 1));
        if (val.empty() || val.size() > 7) continue;
        long v = 0;
        bool digits = true;
        for (TChar c : val) {
            if (c < AR_T('0') || c > AR_T('9')) { digits = false; break; }
            v = v * 10 + (c - AR_T('0'));
        }
        if (!digits || !IsAudioRate(v)) continue;
        tstring where = Lower(section + AR_T("\\") + key);
        bool outputish = ContainsI(where, AR_T("output")) || ContainsI(where, AR_T("sound")) ||
                         ContainsI(where, AR_T("playback")) || ContainsI(where, AR_T("player"));
        bool rateish = ContainsI(key, AR_T("freq")) || ContainsI(key, AR_T("rate")) || ContainsI(key, AR_T("samplerate")) ||
                       ContainsI(key, AR_T("hz"));
        if (!outputish || !rateish) continue;
        IniRateKey k{section + AR_T("\\") + key, v};
        if (Lower(k.path) == AR_T("aimpsoundout\\devicefreq")) r.insert(r.begin(), k);
        else r.push_back(k);
    }
    return r;
}

// AIMP's output device from its configuration (e.g. "WASAPI: Speakers"). Fallback for AIMP 4, which
// has no AIMP_PLAYER_PROPID_OUTPUT; the value is the same as in the AIMP 5/6 property.
inline tstring ConfiguredAimpOutput() {
    auto cfg = Service<IAIMPServiceConfig>(IID_IAIMPServiceConfig);
    auto key = MakeString(AR_T("AIMPSoundOut\\DeviceName"));
    Ptr<IAIMPString> s;
    if (!cfg || !key || Failed(cfg->GetValueAsString(key.Get(), s.Out())) || !s) return tstring();
    return FromString(s.Get());
}

struct ResumeInfo {
    tstring file;
    double position = 0;
    uint32_t rate = 0;
    long long time = 0;  // Unix time of the restart
};

class AimpOutputSync {
public:
    tstring profileDir;
    ResumeInfo resume;  // after a restart by the plugin

    tstring ResumePath() const { return profileDir + AR_T("PreventResampling.resume"); }

    // Rate AIMP currently plays with (0 = unknown). The plugin never changes the value inside the
    // running AIMP, so the configuration value is the one AIMP uses.
    tstring rateKey;
    uint32_t AimpRate() {
        auto keys = FindAimpRateKeys(profileDir);
        if (keys.empty()) return 0;
        rateKey = keys.front().path;
        INT32 cur = (INT32)keys.front().value;
        auto cfg = Service<IAIMPServiceConfig>(IID_IAIMPServiceConfig);
        auto key = MakeString(rateKey);
        if (cfg && key) cfg->GetValueAsInt32(key.Get(), &cur);
        return cur > 0 ? (uint32_t)cur : 0;
    }

    void LogDiagnostics(IAIMPServicePlayer* player) {
        Ptr<IAIMPPropertyList> pl;
        if (player && Succeeded(player->QueryInterface(IID_IAIMPPropertyList, pl.OutV())) && pl) {
            Ptr<IAIMPString> s;
            if (Succeeded(pl->GetValueAsObject(AIMP_PLAYER_PROPID_OUTPUT, IID_IAIMPString, s.OutV())) && s)
                Log(AR_T("AIMP output at start-up: ") + FromString(s.Get()));
            else if (!ConfiguredAimpOutput().empty())
                Log(AR_T("AIMP output (from AIMP's settings): ") + ConfiguredAimpOutput());
        }
        auto keys = FindAimpRateKeys(profileDir);
        if (keys.empty()) Log(AR_T("AIMP.ini: no output sample rate found (") + profileDir + AR_T("AIMP.ini)"));
        for (auto& k : keys) Log(AR_T("AIMP.ini: ") + k.path + AR_T(" = ") + Num(k.value));
    }

    // ---- Continue after a restart: small text file in the profile folder
    void SaveResume(const ResumeInfo& r) {
        std::string data = ToUtf8(r.file) + "\n" + std::to_string((long long)(r.position * 1000)) + "\n" +
                           std::to_string(r.rate) + "\n" + std::to_string(r.time) + "\n";
#ifdef _WIN32
        FILE* f = _wfopen(ResumePath().c_str(), L"wb");
#else
        FILE* f = fopen(ResumePath().c_str(), "wb");
#endif
        if (!f) return;
        fwrite(data.data(), 1, data.size(), f);
        fclose(f);
    }

    // Reads and deletes the file; only valid if the restart happened moments ago
    void LoadResume() {
        resume = ResumeInfo();
#ifdef _WIN32
        FILE* f = _wfopen(ResumePath().c_str(), L"rb");
#else
        FILE* f = fopen(ResumePath().c_str(), "rb");
#endif
        if (!f) return;
        std::string data;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
        fclose(f);
#ifdef _WIN32
        _wremove(ResumePath().c_str());
#else
        remove(ResumePath().c_str());
#endif
        std::vector<std::string> parts;
        std::string cur;
        for (char c : data) {
            if (c == '\n') { parts.push_back(cur); cur.clear(); }
            else if (c != '\r') cur += c;
        }
        if (parts.size() < 4) return;
        long long t = atoll(parts[3].c_str());
        if (t <= 0 || (long long)time(nullptr) - t > 120) return;
#ifdef _WIN32
        int len = MultiByteToWideChar(CP_UTF8, 0, parts[0].data(), (int)parts[0].size(), nullptr, 0);
        resume.file.resize(len);
        if (len > 0) MultiByteToWideChar(CP_UTF8, 0, parts[0].data(), (int)parts[0].size(), &resume.file[0], len);
#else
        resume.file = parts[0];
#endif
        resume.position = atoll(parts[1].c_str()) / 1000.0;
        resume.rate = (uint32_t)atol(parts[2].c_str());
        resume.time = t;
    }
};

}  // namespace ar
