// Settings. Stored in the plugin's own file %APPDATA%\AIMP\PreventResampling.ini (see Ini.h);
// read and written on AIMP's main thread only.
#pragma once
#include "AimpUtil.h"
#include "Ini.h"

namespace ar {

enum class OutputMode : int {
    Auto = 0,         // detect from AIMP's output device
    Shared = 1,       // Windows: WASAPI shared -> set the Windows default format
    Exclusive = 2,    // Windows: WASAPI exclusive (event/push) -> AIMP opens the device itself
    Asio = 3,         // Windows: ASIO
    PipeWire = 4,     // Linux: set the graph rate via clock.force-rate
    Direct = 5,       // Linux: ALSA direct, nothing to do
    DirectSound = 6,  // Windows: DirectSound -> Windows default format + AIMP's own rate
};

// Outputs where AIMP plays with its own fixed rate (AIMPSoundOut\DeviceFreq) and therefore
// only follows the track if AIMP is restarted
inline bool NeedsAimpRestart(OutputMode m) {
    return m == OutputMode::Exclusive || m == OutputMode::Asio || m == OutputMode::DirectSound;
}

struct Config {
    // New installations start disabled so nobody's sound settings change unexpectedly
    bool enabled = false;
    OutputMode mode = OutputMode::Auto;
    bool allowMultiples = true;     // 44.1 -> 88.2 -> 176.4 etc. if a rate is not supported
    bool restoreOnExit = false;     // restore the original rates when AIMP closes
    bool autoTiming = true;         // waits based on the detected hardware
    int settleMs = 150;             // manual wait after switching (autoTiming off)

    // Experimental: restart AIMP so it plays with the new rate (ASIO / WASAPI exclusive / DirectSound)
    bool restartAimp = false;
    bool seamlessRestart = true;    // show a still image of AIMP while it restarts

    tstring windowsDevice;          // part of the Windows device name; empty = automatic
    tstring extraDevices;           // more devices to switch, name parts separated by ';'
    tstring asioDriver;             // empty = taken from AIMP's output
    bool asioForce = false;         // set the ASIO driver rate directly (normally AIMP does it)

    bool vmEnabled = true;          // resolve the Voicemeeter chain
    bool vmEngineRate = true;       // set Option.sr to the track rate
    bool vmAsioRate = true;         // Option.ASIOsr = 1 (ASIO device on A1 follows Option.sr)
    bool vmAllBuses = true;         // switch the Windows devices on A2..A5 too, not only A1

    // Update check (About tab)
    bool updateCheck = true;        // look for a new release on GitHub
    int updateInterval = 2;         // 0 = every AIMP start, 1 = daily, 2 = weekly, 3 = monthly
    bool updateAutoInstall = false; // download a new version and open it in AIMP (once per version)

    static const TChar* Section() { return AR_T("PreventResampling"); }

    // Reads the plugin's own settings file (see Ini.h). A missing file means a new installation:
    // everything stays at the defaults above, in particular the plugin stays disabled.
    void Load() {
        Ini& i = Ini::Get();
        const tstring s = Section();
        enabled = i.Bool(s, AR_T("Enabled"), enabled);
        int m = (int)i.Int(s, AR_T("Mode"), (int)mode);
        if (m >= 0 && m <= 6) mode = (OutputMode)m;
        allowMultiples = i.Bool(s, AR_T("AllowMultiples"), allowMultiples);
        restoreOnExit = i.Bool(s, AR_T("RestoreOnExit"), restoreOnExit);
        autoTiming = i.Bool(s, AR_T("AutoTiming"), autoTiming);
        settleMs = (int)(std::max)(0LL, (std::min)(i.Int(s, AR_T("SettleMs"), settleMs), 5000LL));
        restartAimp = i.Bool(s, AR_T("RestartAimp"), restartAimp);
        seamlessRestart = i.Bool(s, AR_T("SeamlessRestart"), seamlessRestart);
        windowsDevice = i.Str(s, AR_T("WindowsDevice"), windowsDevice);
        extraDevices = i.Str(s, AR_T("ExtraDevices"), extraDevices);
        asioDriver = i.Str(s, AR_T("AsioDriver"), asioDriver);
        asioForce = i.Bool(s, AR_T("AsioForce"), asioForce);
        vmEnabled = i.Bool(s, AR_T("VoicemeeterEnabled"), vmEnabled);
        vmEngineRate = i.Bool(s, AR_T("VoicemeeterEngineRate"), vmEngineRate);
        vmAsioRate = i.Bool(s, AR_T("VoicemeeterAsioRate"), vmAsioRate);
        vmAllBuses = i.Bool(s, AR_T("VoicemeeterAllBuses"), vmAllBuses);
        updateCheck = i.Bool(s, AR_T("UpdateCheck"), updateCheck);
        updateInterval = (int)(std::max)(0LL, (std::min)(i.Int(s, AR_T("UpdateInterval"), updateInterval), 3LL));
        updateAutoInstall = i.Bool(s, AR_T("UpdateAutoInstall"), updateAutoInstall);
    }

    void Save() const {
        Ini& i = Ini::Get();
        const tstring s = Section();
        i.SetBool(s, AR_T("Enabled"), enabled);
        i.Set(s, AR_T("Mode"), (long long)mode);
        i.SetBool(s, AR_T("AllowMultiples"), allowMultiples);
        i.SetBool(s, AR_T("RestoreOnExit"), restoreOnExit);
        i.SetBool(s, AR_T("AutoTiming"), autoTiming);
        i.Set(s, AR_T("SettleMs"), (long long)settleMs);
        i.SetBool(s, AR_T("RestartAimp"), restartAimp);
        i.SetBool(s, AR_T("SeamlessRestart"), seamlessRestart);
        i.Set(s, AR_T("WindowsDevice"), windowsDevice);
        i.Set(s, AR_T("ExtraDevices"), extraDevices);
        i.Set(s, AR_T("AsioDriver"), asioDriver);
        i.SetBool(s, AR_T("AsioForce"), asioForce);
        i.SetBool(s, AR_T("VoicemeeterEnabled"), vmEnabled);
        i.SetBool(s, AR_T("VoicemeeterEngineRate"), vmEngineRate);
        i.SetBool(s, AR_T("VoicemeeterAsioRate"), vmAsioRate);
        i.SetBool(s, AR_T("VoicemeeterAllBuses"), vmAllBuses);
        i.SetBool(s, AR_T("UpdateCheck"), updateCheck);
        i.Set(s, AR_T("UpdateInterval"), (long long)updateInterval);
        i.SetBool(s, AR_T("UpdateAutoInstall"), updateAutoInstall);
        if (!i.Save()) Log(AR_T("Could not write ") + i.Path());
    }
};

}  // namespace ar
