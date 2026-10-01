#pragma once
#include "Endpoints.h"
#include "Voicemeeter.h"

namespace ar {

struct Chain {
    std::vector<Endpoint> targets;  // Reihenfolge: Extra-Geräte, Hardware-Busse, ZULETZT das Gerät, in das AIMP spielt
    bool voicemeeter = false;
};

class RateController {
    Config cfg_;

    // Gleiche Rate zuerst, danach ganzzahlige Vielfache derselben Familie (44.1 -> 88.2 -> 176.4 usw.)
    static std::vector<uint32_t> Candidates(uint32_t hz) {
        std::vector<uint32_t> c{hz};
        uint32_t base = (hz % 44100 == 0) ? 44100 : (hz % 48000 == 0) ? 48000 : 0;
        if (base)
            for (uint32_t m = hz / base * 2; m <= 8; m *= 2) c.push_back(base * m);
        return c;
    }

    static bool Supports(const Endpoint& e, uint32_t hz) {
        if (e.IsVB()) return true;  // virtuelle Treiber akzeptieren beliebige Raten
        WAVEFORMATEXTENSIBLE f;
        if (!GetDeviceFormat(e.id, f)) return false;
        return IsSupportedExclusive(e.id, WithRate(f, hz));
    }

public:
    explicit RateController(const Config& c) : cfg_(c) {}

    Chain Resolve() {
        Chain ch;
        auto add = [&](const Endpoint& e) {
            for (auto& t : ch.targets) if (t.id == e.id) return;
            ch.targets.push_back(e);
        };
        for (auto& n : cfg_.extraDevices) {
            Endpoint e;
            if (FindByName(n, e)) add(e); else Log(L"Extra-Gerät '%s' nicht gefunden", n.c_str());
        }
        Endpoint primary;
        bool have = cfg_.aimpDevice.empty() ? DefaultRender(primary) : FindByName(cfg_.aimpDevice, primary);
        if (!have) { Log(L"Kein Ausgabegerät gefunden"); return ch; }

        if (primary.IsVB() && cfg_.useVoicemeeter) {
            Voicemeeter vm;
            if (vm.Open()) {
                ch.voicemeeter = true;
                Log(L"Voicemeeter erkannt: %s", vm.TypeName());
                auto render = ListRender();
                for (auto& bus : vm.BusDeviceNames())
                    for (auto& e : render)
                        if (!e.IsVB() && !e.name.empty() && (ContainsI(e.name, bus) || ContainsI(bus, e.name))) add(e);
            }
        }
        add(primary);
        return ch;
    }

    // 0 = keine passende Rate gefunden
    uint32_t ChooseRate(const Chain& ch, uint32_t hz) {
        if (ch.targets.empty() || !hz) return 0;
        for (uint32_t c : Candidates(hz)) {
            bool all = true;
            for (auto& t : ch.targets) if (!Supports(t, c)) { all = false; break; }
            if (all) return c;
        }
        for (uint32_t c : Candidates(hz))  // Fallback: nur das AIMP-Gerät muss es können
            if (Supports(ch.targets.back(), c)) return c;
        return 0;
    }

    bool NeedsSwitch(const Chain& ch, uint32_t hz) {
        for (auto& t : ch.targets) {
            WAVEFORMATEXTENSIBLE f;
            if (GetDeviceFormat(t.id, f) && f.Format.nSamplesPerSec != hz) return true;
        }
        return false;
    }

    void Apply(const Chain& ch, uint32_t hz) {
        for (auto& t : ch.targets) {
            WAVEFORMATEXTENSIBLE f;
            if (!GetDeviceFormat(t.id, f) || f.Format.nSamplesPerSec == hz) continue;
            bool ok = SetDeviceFormat(t.id, WithRate(f, hz));
            Log(L"%s: %u -> %u Hz  %s", t.name.c_str(), (unsigned)f.Format.nSamplesPerSec, (unsigned)hz, ok ? L"OK" : L"FEHLER");
        }
        if (ch.voicemeeter && cfg_.restartVoicemeeter) {
            Voicemeeter vm;
            if (vm.Open()) { vm.RestartEngine(); Log(L"Voicemeeter Engine neu gestartet"); }
            Sleep(1500);
        }
        Sleep(cfg_.settleMs);
    }
};

}  // namespace ar
