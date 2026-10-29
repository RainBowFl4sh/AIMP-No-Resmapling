// Bit-perfect mode (Output tab): switches off everything in AIMP that changes the signal and brings
// it back when the mode is switched off again - also after AIMP was restarted in between.
//
// AIMP lets a plugin change most of these settings while it runs (crossfade, equalizer, sound effects,
// volume, pause between tracks). ReplayGain, peak normalization, loudness compensation and the voice
// remover are only read at start-up and AIMP writes its own values back when it
// closes; those are written into AIMP.ini after AIMP has closed (restart helper on Windows), so they
// take effect with the next start of AIMP.
//
// The values from before the mode are kept in the settings file, section [BitPerfectSaved]; a value is
// only kept for a setting the mode really changed. Everything runs on AIMP's main thread.
#pragma once
#include <cmath>

#include "AimpUtil.h"
#include "Config.h"
#include "Ini.h"
#include "apiMessages.h"
#include "apiPlayer.h"

namespace ar {

// AIMP setting that only takes effect at start-up: "Section\Key" in AIMP's configuration, 0 = off.
// alt: the same setting in AIMP 4 (no [AIMPSoundOut.DSP] section there)
struct BpStartupKey {
    const TChar* path;
    const TChar* alt;
    const TChar* saved;  // key in [BitPerfectSaved]
    const char* name;    // shown on the settings page (translated)
};

inline const BpStartupKey* BpStartupKeys(size_t* n) {
    static const BpStartupKey keys[] = {
        {AR_T("AIMPSoundOut\\ReplayGain"), nullptr, AR_T("ReplayGain"), "ReplayGain"},
        {AR_T("AIMPSoundOut.DSP\\Normalizer"), AR_T("AIMPSoundOut\\Normalizer"), AR_T("Normalizer"), "peak normalization"},
        {AR_T("AIMPSoundOut\\LoudnessCompensation"), nullptr, AR_T("LoudnessCompensation"), "loudness compensation"},
        {AR_T("AIMPSoundOut.DSP\\VoiceRemover"), AR_T("AIMPSoundOut\\VoiceRemover"), AR_T("VoiceRemover"), "voice remover"},
    };
    *n = sizeof(keys) / sizeof(keys[0]);
    return keys;
}

// Only these keys and plain numbers may be written into AIMP.ini by the restart helper
inline bool BpAllowedPatch(const tstring& path, const tstring& value) {
    if (value.empty() || value.size() > 6 || value.find_first_not_of(AR_T("0123456789")) != tstring::npos) return false;
    size_t n = 0;
    const BpStartupKey* k = BpStartupKeys(&n);
    for (size_t i = 0; i < n; i++)
        if (path == k[i].path || (k[i].alt && path == k[i].alt)) return true;
    return false;
}

struct BpPatch {
    tstring path;  // "Section\Key"
    int value;
};

// "Section\Key=0;Section\Key=1" (argument of the restart helper)
inline tstring BpPatchText(const std::vector<BpPatch>& p) {
    tstring r;
    for (auto& x : p) r += (r.empty() ? AR_T("") : AR_T(";")) + x.path + AR_T("=") + Num(x.value);
    return r;
}

class BitPerfect {
public:
    static const TChar* Section() { return AR_T("BitPerfectSaved"); }

    // Brings AIMP in line with the settings: neutral values while the mode is on, the saved values
    // back as soon as it (or one of its options, or the whole plugin) is off. Called at start-up and
    // after saving the page.
    void Apply(const Config& cfg) {
        auto player = Service<IAIMPServicePlayer>(IID_IAIMPServicePlayer);
        auto disp = Service<IAIMPServiceMessageDispatcher>(IID_IAIMPServiceMessageDispatcher);
        Ptr<IAIMPPropertyList> props;
        if (player) player->QueryInterface(IID_IAIMPPropertyList, props.OutV());
        Ini& ini = Ini::Get();
        changed_.clear();
        restored_.clear();
        bool on = cfg.BitPerfectOn();

        // Volume: 1.0 = 100 %
        if (player)
            Float(ini, AR_T("Volume"), "volume", on && cfg.bpVolume100, 1.0,
                  [&](double& v) { float f = 0; if (Failed(player->GetVolume(&f))) return false; v = f; return true; },
                  [&](double v) { return Succeeded(player->SetVolume((float)v)); });

        // Crossfade / fading on automatic and manual track changes
        if (props) {
            Int(ini, AR_T("AutoSwitching"), "crossfade", on, 0, props.Get(), AIMP_PLAYER_PROPID_AUTOSWITCHING);
            Int(ini, AR_T("ManualSwitching"), "crossfade", on, 0, props.Get(), AIMP_PLAYER_PROPID_MANUALSWITCHING);
            // No gapless playback: AIMP's "add pause between tracks" (ms, 0 = off)
            Int(ini, AR_T("PauseBetweenTracks"), "gapless", on && cfg.bpNoGapless, kPauseMs, props.Get(),
                AIMP_PLAYER_PROPID_AUTOSWITCHING_PAUSE_BETWEEN_TRACKS);
        }

        if (disp) {
            Bool(ini, AR_T("CrossMixing"), "crossfade", on, disp.Get(), AIMP_MSG_PROPERTY_CROSSMIXING);
            Bool(ini, AR_T("Equalizer"), "equalizer", on, disp.Get(), AIMP_MSG_PROPERTY_EQUALIZER);
            struct Fx { const TChar* key; int msg; double neutral; };
            static const Fx fx[] = {
                {AR_T("Chorus"), AIMP_MSG_PROPERTY_CHORUS, 0.0},   {AR_T("Echo"), AIMP_MSG_PROPERTY_ECHO, 0.0},
                {AR_T("Flanger"), AIMP_MSG_PROPERTY_FLANGER, 0.0}, {AR_T("Reverb"), AIMP_MSG_PROPERTY_REVERB, 0.0},
                {AR_T("TrueBass"), AIMP_MSG_PROPERTY_TRUEBASS, 0.0}, {AR_T("Pitch"), AIMP_MSG_PROPERTY_PITCH, 0.0},
                {AR_T("Speed"), AIMP_MSG_PROPERTY_SPEED, 1.0},     {AR_T("Tempo"), AIMP_MSG_PROPERTY_TEMPO, 1.0},
                {AR_T("Balance"), AIMP_MSG_PROPERTY_BALANCE, 0.0},
                {AR_T("Enhancer"), AIMP_MSG_PROPERTY_ENHANCER, 1.0},  // 1.0 = off (AIMP.ini Enhancer=1)
            };
            for (const Fx& f : fx) {
                int msg = f.msg;
                Float(ini, f.key, "sound effects", on, f.neutral,
                      [&](double& v) { float x = 0; if (Failed(disp->Send(msg, AIMP_MSG_PROPVALUE_GET, &x))) return false; v = x; return true; },
                      [&](double v) { float x = (float)v; return Succeeded(disp->Send(msg, AIMP_MSG_PROPVALUE_SET, &x)); });
            }
        }

        // Settings AIMP only reads at start-up: remember the value before the mode; once AIMP runs
        // with the saved value again, it is no longer needed
        size_t n = 0;
        const BpStartupKey* k = BpStartupKeys(&n);
        for (size_t i = 0; i < n; i++) {
            int cur = 0;
            if (!AimpConfigInt(k[i], &cur)) continue;
            if (on) {
                if (cur != 0 && !ini.Has(Section(), k[i].saved)) ini.Set(Section(), k[i].saved, (long long)cur);
            } else if (ini.Has(Section(), k[i].saved) && cur == (int)ini.Int(Section(), k[i].saved, 0)) {
                ini.Remove(Section(), k[i].saved);
            }
        }
        ini.Save();
        if (!changed_.empty()) Log(AR_T("Bit-perfect mode: switched off ") + Join(changed_));
        if (!restored_.empty()) Log(AR_T("Bit-perfect mode: restored ") + Join(restored_));
        auto p = Pending(cfg);
        if (!p.empty()) Log(AR_T("Bit-perfect mode: with the next start of AIMP ") + BpPatchText(p));
    }

    // Settings that only take effect at start-up and still differ from what the mode wants
    std::vector<BpPatch> Pending(const Config& cfg) {
        std::vector<BpPatch> r;
        Ini& ini = Ini::Get();
        size_t n = 0;
        const BpStartupKey* k = BpStartupKeys(&n);
        for (size_t i = 0; i < n; i++) {
            int cur = 0;
            const TChar* path = nullptr;
            if (!AimpConfigInt(k[i], &cur, &path)) continue;
            int want = cur;
            if (cfg.BitPerfectOn()) want = 0;
            else if (ini.Has(Section(), k[i].saved)) want = (int)ini.Int(Section(), k[i].saved, cur);
            if (want != cur && want >= 0) r.push_back({path, want});
        }
        return r;
    }

    // Names of the pending settings (English keys for Tr)
    std::vector<const char*> PendingNames(const Config& cfg) {
        std::vector<const char*> r;
        size_t n = 0;
        const BpStartupKey* k = BpStartupKeys(&n);
        for (auto& p : Pending(cfg))
            for (size_t i = 0; i < n; i++)
                if (p.path == k[i].path || (k[i].alt && p.path == k[i].alt)) r.push_back(k[i].name);
        return r;
    }

    static const int kPauseMs = 500;
    static constexpr double kScale = 1e8;

private:
    std::vector<tstring> changed_, restored_;

    static tstring Join(const std::vector<tstring>& v) {
        tstring r;
        for (auto& s : v)
            if (r.find(s) == tstring::npos) r += (r.empty() ? AR_T("") : AR_T(", ")) + s;
        return r;
    }

    // The key AIMP uses (AIMP 4: alt). A missing key is AIMP's default, which is "off" for all of
    // these settings (AIMP leaves out ReplayGain=0, for example).
    static bool AimpConfigInt(const BpStartupKey& k, int* v, const TChar** used = nullptr) {
        auto cfg = Service<IAIMPServiceConfig>(IID_IAIMPServiceConfig);
        if (!cfg) return false;
        *v = 0;
        if (used) *used = k.path;
        for (const TChar* p : {k.path, k.alt}) {
            auto key = p ? MakeString(p) : Ptr<IAIMPString>();
            INT32 x = 0;
            if (key && Succeeded(cfg->GetValueAsInt32(key.Get(), &x))) {
                *v = x;
                if (used) *used = p;
                break;
            }
        }
        return true;
    }

    // Floats are kept as value x 10^8 (exact for AIMP's single-precision values, no locale-dependent
    // decimal point in the file)
    template <typename Get, typename Set>
    void Float(Ini& ini, const TChar* key, const char* what, bool neutralWanted, double neutral, Get get, Set set) {
        double cur = 0;
        if (!get(cur)) return;
        bool isNeutral = std::fabs(cur - neutral) < 0.0005;
        if (neutralWanted) {
            if (isNeutral) return;
            if (!ini.Has(Section(), key)) ini.Set(Section(), key, (long long)std::llround(cur * kScale));
            if (set(neutral)) changed_.push_back(FromUtf8(what));
        } else if (ini.Has(Section(), key)) {
            double saved = (double)ini.Int(Section(), key, 0) / kScale;
            if (set(saved)) restored_.push_back(FromUtf8(what));
            ini.Remove(Section(), key);
        }
    }

    void Int(Ini& ini, const TChar* key, const char* what, bool neutralWanted, int neutral, IAIMPPropertyList* pl, int id) {
        INT32 cur = 0;
        if (Failed(pl->GetValueAsInt32(id, &cur))) return;
        if (neutralWanted) {
            if (cur == neutral) return;
            if (!ini.Has(Section(), key)) ini.Set(Section(), key, (long long)cur);
            if (Succeeded(pl->SetValueAsInt32(id, neutral))) changed_.push_back(FromUtf8(what));
        } else if (ini.Has(Section(), key)) {
            if (Succeeded(pl->SetValueAsInt32(id, (INT32)ini.Int(Section(), key, 0)))) restored_.push_back(FromUtf8(what));
            ini.Remove(Section(), key);
        }
    }

    // LongBool properties (AIMP returns -1 for true)
    void Bool(Ini& ini, const TChar* key, const char* what, bool neutralWanted, IAIMPServiceMessageDispatcher* d, int msg) {
        INT32 cur = 0;
        if (Failed(d->Send(msg, AIMP_MSG_PROPVALUE_GET, &cur))) return;
        if (neutralWanted) {
            if (!cur) return;
            if (!ini.Has(Section(), key)) ini.Set(Section(), key, (long long)1);
            INT32 off = 0;
            if (Succeeded(d->Send(msg, AIMP_MSG_PROPVALUE_SET, &off))) changed_.push_back(FromUtf8(what));
        } else if (ini.Has(Section(), key)) {
            INT32 v = ini.Int(Section(), key, 0) ? 1 : 0;
            if (Succeeded(d->Send(msg, AIMP_MSG_PROPVALUE_SET, &v))) restored_.push_back(FromUtf8(what));
            ini.Remove(Section(), key);
        }
    }
};

}  // namespace ar
