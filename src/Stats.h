// Runtime statistics for the settings page.
#pragma once
#include <map>

#include "Common.h"
#include "Lang.h"

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
    // Text of the Advanced tab in the current language; the label column fits the longest label
    tstring Format(bool enabled) const {
        std::vector<std::pair<tstring, tstring>> rows;  // label (empty = heading or plain line), value
        auto line = [&](const tstring& s) { rows.push_back({tstring(), s}); };
        auto row = [&](const char* label, const tstring& value) { rows.push_back({Tr(label), value}); };
        line(AR_T(AR_PLUGIN_NAME " " AR_VERSION "  \u00B7  ") + Trf("by %1", {AR_T(AR_AUTHOR)}));
        if (!enabled) line(Tr("The plugin is DISABLED (General tab)."));
        line(tstring());

        line(Tr("NOW PLAYING"));
        row("AIMP output:", aimpOutput.empty() ? tstring(AR_T("-")) : aimpOutput);
        row("Mode:", modeText.empty() ? tstring(AR_T("-")) : modeText);
        if (trackRate) {
            tstring tr = Khz(trackRate);
            if (trackBits) tr += AR_T(" / ") + Num(trackBits) + AR_T(" bit");
            if (trackChannels) tr += AR_T(" / ") + Trf("%1 ch", {Num(trackChannels)});
            if (!trackCodec.empty()) tr += AR_T(" (") + trackCodec + AR_T(")");
            row("Track:", tr);
        }
        if (aimpRate)
            row("AIMP rate:", Khz(aimpRate) + (trackRate && aimpRate != trackRate ? AR_T("  (") + Tr("AIMP resamples") + AR_T(")") : tstring()));
        bool aimpOk = !aimpRate || aimpRate == deviceRate;
        row("Output rate:",
            (deviceRate ? Khz(deviceRate) : tstring(AR_T("-"))) +
                (deviceRate && trackRate && aimpOk
                     ? AR_T("  (") + Tr(deviceRate == trackRate ? "bit-perfect, no resampling" : "integer multiple") + AR_T(")")
                     : tstring()));
        if (!chainText.empty()) row("Devices:", chainText);
        if (!vmText.empty()) row("Voicemeeter:", vmText);
        if (!note.empty()) row("Note:", note);
        if (!hardwareText.empty()) row("Timing:", hardwareText);
        line(tstring());

        line(Tr("COUNTERS"));
        row("Tracks analysed:", Num(tracks));
        row("Switches:", Num(switches));
        row("Already matching:", Num(skipped));
        row("Failed:", Num(failures));
        row("Voicemeeter restarts:", Num(vmRestarts));
        row("AIMP restarts:", Num(aimpRestarts));
        if (switches)
            row("Switch time:", Trf("%1 ms (last), %2 ms (average)", {Num(lastSwitchMs), Num(switchMsTotal / switches)}));

        if (!rates.empty()) {
            line(tstring());
            line(Tr("TRACK SAMPLE RATES"));
            long total = 0;
            for (auto& r : rates) total += r.second;
            for (auto& r : rates) {
                tstring k = Khz(r.first);
                while (TextLength(k) < 12) k += AR_T(' ');
                line(AR_T("  ") + k + Num(r.second) + AR_T("  (") + Num(total ? r.second * 100 / total : 0) + AR_T(" %)"));
            }
        }

        size_t width = 18;
        for (auto& r : rows) width = (std::max)(width, TextLength(r.first) + 2);
        tstring t;
        for (auto& r : rows) {
            if (r.first.empty()) { t += r.second + AR_NL; continue; }
            tstring label = r.first;
            while (TextLength(label) < width) label += AR_T(' ');
            t += AR_T("  ") + label + r.second + AR_NL;
        }
        return t;
    }
};

}  // namespace ar
