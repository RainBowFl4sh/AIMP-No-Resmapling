// Decides which rate has to be set for the current output and sets it.
// Runs on the worker thread only (on Windows initialised as COM STA).
#pragma once
#include <ctime>
#include <map>

#include "Config.h"
#include "Hardware.h"
#include "Stats.h"

#ifdef _WIN32
#include "win/Asio.h"
#include "win/Voicemeeter.h"
#include "win/Wasapi.h"
#else
#include "linux/PipeWire.h"
#endif

namespace ar {

struct TrackInfo {
    uint32_t rate = 0, bits = 0, channels = 0;
    tstring codec, aimpOutput;
};

struct Decision {
    OutputMode mode = OutputMode::Auto;
    uint32_t rate = 0;          // chosen target rate; 0 = none found
    bool apply = false;         // something has to be changed
    bool stopPlayback = false;  // playback has to be stopped for that
    uint32_t currentRate = 0;   // rate the output runs at right now (statistics)
    tstring chain, info;
#ifdef _WIN32
    std::vector<win::Endpoint> targets;  // order: extra devices, Voicemeeter outputs, AIMP's device LAST
    bool vm = false, vmEngine = false;
    bool asioDirect = false;
    win::AsioDriver asio;
#endif
};

inline const TChar* ModeName(OutputMode m) {
    switch (m) {
        case OutputMode::Auto: return AR_T("Automatic");
        case OutputMode::Shared: return AR_T("WASAPI shared");
        case OutputMode::Exclusive: return AR_T("WASAPI exclusive");
        case OutputMode::Asio: return AR_T("ASIO");
        case OutputMode::PipeWire: return AR_T("PipeWire");
        case OutputMode::Direct: return AR_T("ALSA direct");
        case OutputMode::DirectSound: return AR_T("DirectSound");
    }
    return AR_T("?");
}

class RateController {
    Config cfg_;
    Timing timing_;
#ifdef _WIN32
    std::map<std::wstring, WAVEFORMATEXTENSIBLE> original_;  // for "restore when AIMP closes"
    uint32_t vmOriginalRate_ = 0;
    bool vmUsed_ = false;  // a Voicemeeter chain was switched in this session
    uint32_t vmSet_ = 0;  // last Option.sr set by the plugin (Voicemeeter sometimes returns stale values)
    uint32_t asioRateSet_ = 0;
    bool asioBusy_ = false;
#else
    lnx::PipeWire pw_;
#endif

    // Same rate first, then integer multiples of the same family (44.1 -> 88.2 -> 176.4 etc.)
    std::vector<uint32_t> Candidates(uint32_t hz) const {
        std::vector<uint32_t> c{hz};
        if (!cfg_.allowMultiples) return c;
        uint32_t base = (hz % 44100 == 0) ? 44100 : (hz % 48000 == 0) ? 48000 : 0;
        if (base)
            for (uint32_t m = hz / base * 2; m <= 8; m *= 2) c.push_back(base * m);
        return c;
    }

public:
    void SetConfig(const Config& c, const Timing& t) {
        if (c.asioDriver != cfg_.asioDriver) ResetAsio();
        cfg_ = c;
        timing_ = t;
    }

    OutputMode Resolve(const tstring& aimpOutput) const {
        if (cfg_.mode != OutputMode::Auto) return cfg_.mode;
#ifdef _WIN32
        if (ContainsI(aimpOutput, AR_T("asio"))) return OutputMode::Asio;
        if (ContainsI(aimpOutput, AR_T("exclusive")) || ContainsI(aimpOutput, AR_T("exklusiv"))) return OutputMode::Exclusive;
        if (ContainsI(aimpOutput, AR_T("directsound"))) return OutputMode::DirectSound;
        return OutputMode::Shared;
#else
        (void)aimpOutput;
        return lnx::PipeWire::Available() ? OutputMode::PipeWire : OutputMode::Direct;
#endif
    }

#ifdef _WIN32
    void ResetAsio() { asioBusy_ = false; asioRateSet_ = 0; }

    // ---------------------------------------------------------------- Windows
    Decision Prepare(const TrackInfo& t) {
        Decision d;
        d.mode = Resolve(t.aimpOutput);
        bool sharedLike = d.mode == OutputMode::Shared || d.mode == OutputMode::DirectSound;
        auto all = win::ListRender();
        auto add = [&](const win::Endpoint& e) {
            for (auto& x : d.targets) if (x.id == e.id) return;
            d.targets.push_back(e);
        };
        for (auto& n : Split(cfg_.extraDevices, L';')) {
            win::Endpoint e;
            if (win::FindByName(all, n, e)) add(e);
            else Log(L"Extra device '" + n + L"' not found");
        }

        // Device AIMP plays into
        win::Endpoint primary;
        bool havePrimary = false;
        if (!cfg_.windowsDevice.empty()) havePrimary = win::FindByName(all, cfg_.windowsDevice, primary);
        else if (d.mode != OutputMode::Asio) havePrimary = win::FindInText(all, t.aimpOutput, primary) || win::DefaultRender(primary);

        bool vbAsio = false;
        if (d.mode == OutputMode::Asio) {
            if (win::FindAsioDriver(cfg_.asioDriver, t.aimpOutput, d.asio)) {
                vbAsio = ContainsI(d.asio.name, L"voicemeeter") || ContainsI(d.asio.name, L"vb-audio");
                d.asioDirect = !vbAsio && cfg_.asioForce && !asioBusy_;
                d.chain = L"ASIO: " + d.asio.name;
            } else {
                d.chain = L"ASIO driver not found";
            }
        }

        // Voicemeeter: hardware outputs A1 (or A1..A5) are mapped to Windows devices by name
        auto& vm = win::Voicemeeter::Get();
        if (cfg_.vmEnabled && ((havePrimary && primary.IsVB()) || vbAsio) && vm.Running()) {
            d.vm = true;
            d.vmEngine = cfg_.vmEngineRate;
            for (auto& bus : vm.BusDevices(cfg_.vmAllBuses))
                for (auto& e : all)
                    if (!e.IsVB() && win::NamesMatch(bus.second, e.name)) add(e);
        }
        // Shared/DirectSound: AIMP's device itself is switched (exclusive/ASIO: AIMP opens it itself)
        if (havePrimary && (sharedLike || (d.vm && d.mode == OutputMode::Exclusive))) add(primary);

        if (d.chain.empty()) {
            for (auto& e : d.targets) d.chain += (d.chain.empty() ? L"" : L" -> ") + e.name;
            if (d.chain.empty() && havePrimary) d.chain = primary.name;
        }
        if (d.vm) d.chain += std::wstring(L"  [") + win::Voicemeeter::TypeName(vm.Type()) + L"]";

        if (sharedLike && !havePrimary && d.targets.empty()) {
            d.info = L"No Windows output device found (check the 'Windows device' setting)";
            return d;  // rate = 0 -> counts as failure
        }
        if (d.targets.empty() && !d.vmEngine && !d.asioDirect) {
            // Nothing to switch: AIMP opens the device itself
            d.rate = t.rate;
            d.currentRate = t.rate;
            return d;
        }

        // Choose a rate every device supports
        auto supports = [&](const win::Endpoint& e, uint32_t hz) {
            if (e.IsVB()) return true;  // virtual drivers accept any rate
            WAVEFORMATEXTENSIBLE f;
            if (!win::GetDeviceFormat(e.id, f)) return false;
            // -1 = unknown (e.g. held exclusively by Voicemeeter) -> try anyway
            return win::SupportsExclusive(e.id, win::WithRate(f, hz)) != 0;
        };
        for (uint32_t c : Candidates(t.rate)) {
            if (d.vmEngine && !win::Voicemeeter::EngineSupports(c)) continue;
            bool ok = true;
            for (auto& e : d.targets) if (!supports(e, c)) { ok = false; break; }
            if (ok) { d.rate = c; break; }
        }
        if (!d.rate && !d.targets.empty()) {  // fallback: only the last device (AIMP's target) must support it
            for (uint32_t c : Candidates(t.rate))
                if (supports(d.targets.back(), c) && (!d.vmEngine || win::Voicemeeter::EngineSupports(c))) { d.rate = c; break; }
        }
        if (!d.rate) return d;

        // Does anything have to change at all?
        for (auto& e : d.targets) {
            WAVEFORMATEXTENSIBLE f;
            if (win::GetDeviceFormat(e.id, f)) {
                if (!d.currentRate) d.currentRate = f.Format.nSamplesPerSec;
                if (f.Format.nSamplesPerSec != d.rate) d.apply = true;
            }
        }
        if (d.vmEngine) {
            uint32_t known = vmSet_ ? vmSet_ : vm.PreferredRate();
            if (known) d.currentRate = known;
            if (known != d.rate) d.apply = true;
        }
        if (d.asioDirect && asioRateSet_ != d.rate) d.apply = true;
        if (d.asioDirect && !d.currentRate && asioRateSet_) d.currentRate = asioRateSet_;
        d.stopPlayback = d.apply;
        return d;
    }

    // Returns the resulting output rate (0 = error)
    uint32_t Apply(const Decision& d) {
        bool ok = true, formatsChanged = false;
        for (auto& e : d.targets) {
            WAVEFORMATEXTENSIBLE f;
            if (!win::GetDeviceFormat(e.id, f) || f.Format.nSamplesPerSec == d.rate) continue;
            if (!original_.count(e.id)) original_[e.id] = f;
            bool r = win::SetDeviceFormat(e.id, win::WithRate(f, d.rate));
            ok &= r;
            formatsChanged |= r;
            Log(e.name + L": " + Khz(f.Format.nSamplesPerSec) + L" -> " + Khz(d.rate) + (r ? L"  OK" : L"  FAILED"));
        }
        uint32_t result = ok ? d.rate : 0;

        if (d.asioDirect) {
            win::AsioSession s(d.asio);
            if (!s.Ok()) {
                asioBusy_ = true;  // do not try again for every track
                result = 0;
            } else {
                uint32_t target = 0;
                for (uint32_t c : Candidates(d.rate)) if (s.Can(c)) { target = c; break; }
                uint32_t before = s.Rate();
                if (target && (before == target || s.SetRate(target))) {
                    asioRateSet_ = target;
                    result = target;
                    Log(L"ASIO " + d.asio.name + L": " + Khz(before) + L" -> " + Khz(target));
                } else {
                    Log(L"ASIO " + d.asio.name + L": rate " + Khz(d.rate) + L" not possible");
                    result = 0;
                }
            }
        }

        if (d.vm) {
            auto& vm = win::Voicemeeter::Get();
            // Remember the rate Voicemeeter ran with before the plugin touched it (restored when AIMP
            // closes). The engine's real rate is reliable; Option.sr can report stale values.
            if (!vmOriginalRate_ && !vmSet_) {
                vmOriginalRate_ = vm.EngineRate();
                if (!vmOriginalRate_) vmOriginalRate_ = vm.PreferredRate();
                if (vmOriginalRate_) Log(L"Voicemeeter: engine rate before this session " + Khz(vmOriginalRate_));
            }
            vmUsed_ = true;
            bool engineChange = d.vmEngine && vmSet_ != d.rate;
            if (engineChange) {
                if (vm.SetFloat("Option.sr", (float)d.rate)) vmSet_ = d.rate;
                else Log(L"Voicemeeter: could not set Option.sr");
                if (cfg_.vmAsioRate) vm.SetFloat("Option.ASIOsr", 1.0f);
            }
            // The engine has to restart to use a new rate or to reopen devices with a new format
            if (engineChange || formatsChanged) {
                int64_t t0 = NowMs();
                vm.Restart();
                if (d.vmEngine) {
                    bool ready = vm.WaitForEngine(d.rate, timing_.vmFirstPollMs, timing_.vmTimeoutMs);
                    uint32_t eng = vm.EngineRate();
                    Log(L"Voicemeeter: engine restarted" +
                        (ready ? L" with " + Khz(eng) + L" in " + Num(NowMs() - t0) + L" ms"
                               : L" (reports A1 at " + (eng ? Khz(eng) : std::wstring(L"?")) +
                                     L" - does the A1 device support the rate?)"));
                } else {
                    SleepMs(timing_.vmRestartMs);
                    Log(L"Voicemeeter: engine restarted");
                }
            }
        }
        SleepMs(timing_.settleMs);
        return result;
    }

    // When AIMP closes: restore the original formats (if enabled)
    void Shutdown() {
        if (cfg_.restoreOnExit) {
            auto& vm = win::Voicemeeter::Get();
            bool vmRunning = vmUsed_ && vm.Running();
            bool engine = vmRunning && vmOriginalRate_ && vmSet_ && vmSet_ != vmOriginalRate_;
            if (engine && !vm.SetFloat("Option.sr", (float)vmOriginalRate_)) {
                Log(L"Voicemeeter: could not set Option.sr back");
                engine = false;
            }
            bool restored = false;
            for (auto& o : original_) {
                bool r = win::SetDeviceFormat(o.first, o.second);
                restored |= r;
                Log(L"Restored: " + Khz(o.second.Format.nSamplesPerSec) + (r ? L"" : L" (failed)"));
            }
            // Voicemeeter has to reopen its devices (A1-A5) after their Windows format changed - otherwise
            // a WDM output stays red unless "Auto restart audio engine" is ticked in Voicemeeter.
            // Wait until the engine runs again: logging out right away can drop the pending command.
            if (vmRunning && (engine || restored)) {
                int64_t t0 = NowMs();
                vm.Restart();
                uint32_t target = engine ? vmOriginalRate_ : 0;
                bool ready = target ? vm.WaitForEngine(target, timing_.vmFirstPollMs, (std::max)(timing_.vmTimeoutMs, 3000))
                                    : (SleepMs((std::max)(timing_.vmRestartMs, 400)), true);
                if (engine)
                    Log(L"Voicemeeter: engine rate restored to " + Khz(vmOriginalRate_) +
                        (ready ? L" in " + Num(NowMs() - t0) + L" ms" : std::wstring(L" (engine did not confirm the rate)")));
                else
                    Log(L"Voicemeeter: engine restarted after restoring the device formats");
            }
        }
        original_.clear();
        vmOriginalRate_ = 0;
        vmSet_ = 0;
        vmUsed_ = false;
    }

    // ---- Session across a restart by the plugin (ASIO / exclusive / DirectSound)
    // AIMP is closed only to apply the new rate: the devices must stay at that rate (no restore), and the
    // next AIMP process has to know the rates from before the session so that the real "AIMP closes"
    // still restores them. Stored in the settings file, section [Session].
    void SaveSession() {
        Ini& ini = Ini::Get();
        const tstring sec = AR_T("Session");
        ini.Clear(sec);
        int n = 0;
        for (auto& o : original_) {
            static const char* hex = "0123456789abcdef";
            std::wstring blob;
            const unsigned char* b = (const unsigned char*)&o.second;
            for (size_t i = 0; i < sizeof(o.second); i++) { blob += (wchar_t)hex[b[i] >> 4]; blob += (wchar_t)hex[b[i] & 15]; }
            ini.Set(sec, L"Device" + Num(n++), o.first + L"|" + blob);
        }
        ini.Set(sec, AR_T("VoicemeeterOriginal"), (long long)vmOriginalRate_);
        ini.Set(sec, AR_T("VoicemeeterSet"), (long long)vmSet_);
        ini.SetBool(sec, AR_T("VoicemeeterUsed"), vmUsed_);
        ini.Set(sec, AR_T("Saved"), (long long)time(nullptr));
        ini.Save();
        Log(L"Restart: kept the current rates, remembered " + Num(n) + L" original device format(s)" +
            (vmOriginalRate_ ? L" and Voicemeeter " + Khz(vmOriginalRate_) : std::wstring()));
        original_.clear();
        vmOriginalRate_ = vmSet_ = 0;
        vmUsed_ = false;
    }

    // Takes over the session of the AIMP process before a restart by the plugin (and removes it from the
    // file). fresh = the restart just happened, so Voicemeeter still runs at the rate the plugin set.
    void LoadSession(bool fresh) {
        Ini& ini = Ini::Get();
        const tstring sec = AR_T("Session");
        if (!ini.Int(sec, AR_T("Saved"), 0)) return;
        int n = 0;
        for (int i = 0; i < 64; i++) {
            std::wstring v = ini.Str(sec, L"Device" + Num(i));
            size_t bar = v.find(L'|');
            if (v.empty() || bar == std::wstring::npos || v.size() - bar - 1 != sizeof(WAVEFORMATEXTENSIBLE) * 2) continue;
            WAVEFORMATEXTENSIBLE f;
            unsigned char* b = (unsigned char*)&f;
            auto nib = [](wchar_t c) { return c >= L'a' ? c - L'a' + 10 : c - L'0'; };
            for (size_t k = 0; k < sizeof(f); k++) b[k] = (unsigned char)(nib(v[bar + 1 + 2 * k]) << 4 | nib(v[bar + 2 + 2 * k]));
            if (!original_.count(v.substr(0, bar))) original_[v.substr(0, bar)] = f;
            n++;
        }
        if (!vmOriginalRate_) vmOriginalRate_ = (uint32_t)ini.Int(sec, AR_T("VoicemeeterOriginal"), 0);
        if (fresh) {
            vmSet_ = (uint32_t)ini.Int(sec, AR_T("VoicemeeterSet"), 0);
            vmUsed_ = ini.Bool(sec, AR_T("VoicemeeterUsed"), false);
        }
        ini.Clear(sec);
        ini.Save();
        Log(L"Restart: took over " + Num(n) + L" original device format(s)" +
            (vmOriginalRate_ ? L" and Voicemeeter " + Khz(vmOriginalRate_) : std::wstring()) + L" from before the restart");
    }

    tstring VoicemeeterStatus() {
        auto& vm = win::Voicemeeter::Get();
        if (!vm.Installed()) return L"not installed";
        long t = vm.Type();
        if (!t) return L"installed, not running";
        uint32_t eng = vm.EngineRate();
        tstring s = std::wstring(win::Voicemeeter::TypeName(t)) + L", engine " + (eng ? Khz(eng) : std::wstring(L"?"));
        for (auto& b : vm.BusDevices(true)) s += L", A" + Num(b.first + 1) + L": " + b.second;
        return s;
    }

#else
    void ResetAsio() {}

    // ---------------------------------------------------------------- Linux
    Decision Prepare(const TrackInfo& t) {
        Decision d;
        d.mode = Resolve(t.aimpOutput);
        d.rate = t.rate;
        if (d.mode == OutputMode::PipeWire) {
            d.chain = "PipeWire (clock.force-rate)";
            d.currentRate = pw_.Forced();
            d.apply = pw_.Forced() != t.rate;
            d.stopPlayback = false;  // PipeWire switches the graph while playing
        } else {
            d.currentRate = t.rate;
            d.info = "ALSA direct: AIMP opens the device itself";
        }
        return d;
    }

    uint32_t Apply(const Decision& d) {
        if (d.mode != OutputMode::PipeWire) return d.rate;
        if (!lnx::PipeWire::Available()) {
            Log("pw-metadata not found (is PipeWire installed?)");
            return 0;
        }
        uint32_t before = pw_.Forced();
        bool ok = pw_.ForceRate(d.rate);
        Log("PipeWire: clock.force-rate " + (before ? Khz(before) : std::string("auto")) + " -> " + Khz(d.rate) +
            (ok ? "  OK" : "  FAILED"));
        SleepMs(timing_.settleMs);
        return ok ? d.rate : 0;
    }

    void Shutdown() {
        if (pw_.Forced()) {
            pw_.Release();
            Log("PipeWire: clock.force-rate released");
        }
    }

    tstring VoicemeeterStatus() { return tstring(); }
    void SaveSession() {}  // Linux: AIMP is never restarted by the plugin
    void LoadSession(bool) {}
#endif
};

}  // namespace ar
