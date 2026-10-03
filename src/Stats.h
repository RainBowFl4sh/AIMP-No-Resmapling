// Runtime statistics for the settings page.
#pragma once
#include <map>

#include "Common.h"

namespace ar {

class Stats {
    mutable std::mutex m_;

public:
    // Counters (kept in AIMP's configuration)
    long tracks = 0;          // analysed tracks
    long switches = 0;        // successful switches
    long skipped = 0;         // rate already matched
    long failures = 0;        // no rate found or error
    long vmRestarts = 0;      // Voicemeeter engine restarts
    long aimpRestarts = 0;    // AIMP restarts for a new output rate
    int64_t switchMsTotal = 0;
    std::map<uint32_t, long> rates;  // sample rates of played tracks

    // Current state
    int64_t lastSwitchMs = 0;
    uint32_t trackRate = 0, trackBits = 0, trackChannels = 0, deviceRate = 0, aimpRate = 0;
    tstring trackCodec, aimpOutput, modeText, chainText, vmText, hardwareText, note;

    template <typename F>
    void Update(F f) {
        std::lock_guard<std::mutex> g(m_);
        f(*this);
    }

    Stats Snapshot() const {
        std::lock_guard<std::mutex> g(m_);
        Stats s;
        s.Assign(*this);
        return s;
    }

    void ResetCounters() {
        std::lock_guard<std::mutex> g(m_);
        tracks = switches = skipped = failures = vmRestarts = aimpRestarts = 0;
        switchMsTotal = lastSwitchMs = 0;
        rates.clear();
    }

    Stats() = default;
    Stats(const Stats& o) { Assign(o); }

private:
    void Assign(const Stats& o) {
        tracks = o.tracks; switches = o.switches; skipped = o.skipped; failures = o.failures;
        vmRestarts = o.vmRestarts; aimpRestarts = o.aimpRestarts; switchMsTotal = o.switchMsTotal; rates = o.rates;
        lastSwitchMs = o.lastSwitchMs; trackRate = o.trackRate; trackBits = o.trackBits;
        trackChannels = o.trackChannels; deviceRate = o.deviceRate; aimpRate = o.aimpRate;
        trackCodec = o.trackCodec; aimpOutput = o.aimpOutput; modeText = o.modeText; chainText = o.chainText;
        vmText = o.vmText; hardwareText = o.hardwareText; note = o.note;
    }

public:
    tstring Format(bool enabled) const {
        tstring t;
        auto line = [&](const tstring& s) { t += s + AR_NL; };
        auto pad = [](tstring s, size_t w) { while (s.size() < w) s += AR_T(' '); return s; };
        auto row = [&](const TChar* label, const tstring& value) { line(AR_T("  ") + pad(label, 18) + value); };
        line(AR_T(AR_PLUGIN_NAME " " AR_VERSION "  ·  by " AR_AUTHOR));
        if (!enabled) line(AR_T("The plugin is DISABLED (General tab)."));
        line(tstring());

        line(AR_T("NOW PLAYING"));
        row(AR_T("AIMP output:"), aimpOutput.empty() ? tstring(AR_T("-")) : aimpOutput);
        row(AR_T("Mode:"), modeText.empty() ? tstring(AR_T("-")) : modeText);
        if (trackRate) {
            tstring tr = Khz(trackRate);
            if (trackBits) tr += AR_T(" / ") + Num(trackBits) + AR_T(" bit");
            if (trackChannels) tr += AR_T(" / ") + Num(trackChannels) + AR_T(" ch");
            if (!trackCodec.empty()) tr += AR_T(" (") + trackCodec + AR_T(")");
            row(AR_T("Track:"), tr);
        }
        if (aimpRate)
            row(AR_T("AIMP rate:"), Khz(aimpRate) + (trackRate && aimpRate != trackRate ? tstring(AR_T("  (AIMP resamples)")) : tstring()));
        bool aimpOk = !aimpRate || aimpRate == deviceRate;
        row(AR_T("Output rate:"),
            (deviceRate ? Khz(deviceRate) : tstring(AR_T("-"))) +
                (deviceRate && trackRate && aimpOk
                     ? (deviceRate == trackRate ? tstring(AR_T("  (bit-perfect, no resampling)"))
                                                : tstring(AR_T("  (integer multiple)")))
                     : tstring()));
        if (!chainText.empty()) row(AR_T("Devices:"), chainText);
        if (!vmText.empty()) row(AR_T("Voicemeeter:"), vmText);
        if (!note.empty()) row(AR_T("Note:"), note);
        if (!hardwareText.empty()) row(AR_T("Timing:"), hardwareText);
        line(tstring());

        line(AR_T("COUNTERS"));
        row(AR_T("Tracks analysed:"), Num(tracks));
        row(AR_T("Switches:"), Num(switches));
        row(AR_T("Already matching:"), Num(skipped));
        row(AR_T("Failed:"), Num(failures));
        row(AR_T("Voicemeeter restarts:"), Num(vmRestarts));
        row(AR_T("AIMP restarts:"), Num(aimpRestarts));
        if (switches)
            row(AR_T("Switch time:"), Num(lastSwitchMs) + AR_T(" ms (last), ") + Num(switchMsTotal / switches) + AR_T(" ms (average)"));

        if (!rates.empty()) {
            line(tstring());
            line(AR_T("TRACK SAMPLE RATES"));
            long total = 0;
            for (auto& r : rates) total += r.second;
            for (auto& r : rates)
                line(AR_T("  ") + pad(Khz(r.first), 12) + Num(r.second) + AR_T("  (") + Num(total ? r.second * 100 / total : 0) +
                     AR_T(" %)"));
        }
        return t;
    }
};

}  // namespace ar
