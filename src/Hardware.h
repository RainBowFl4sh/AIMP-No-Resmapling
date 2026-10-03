// Hardware detection and timing profile.
//
// Switching sample rates involves several components that need a moment to settle (the Windows
// audio engine, Voicemeeter's engine restart, the DAC). Fast PCs can switch with shorter pauses,
// slower PCs get more time. The profile scales all waits; timeouts are never shortened.
#pragma once
#include "Common.h"

#ifndef _WIN32
#include <fstream>
#endif

namespace ar {

enum class Speed { Fast, Normal, Conservative };

struct Hardware {
    int threads = 0;    // logical processors
    int mhz = 0;        // nominal CPU clock
    int ramGB = 0;      // installed memory
    tstring cpu;        // CPU name
    Speed speed = Speed::Normal;

    static Hardware Detect() {
        Hardware h;
#ifdef _WIN32
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        h.threads = (int)si.dwNumberOfProcessors;
        MEMORYSTATUSEX ms = {sizeof(ms)};
        if (GlobalMemoryStatusEx(&ms)) h.ramGB = (int)((ms.ullTotalPhys + (512ull << 20)) >> 30);
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k) ==
            ERROR_SUCCESS) {
            DWORD mhz = 0, sz = sizeof(mhz);
            if (RegQueryValueExW(k, L"~MHz", nullptr, nullptr, (BYTE*)&mhz, &sz) == ERROR_SUCCESS) h.mhz = (int)mhz;
            wchar_t name[256] = {};
            sz = sizeof(name) - sizeof(wchar_t);
            if (RegQueryValueExW(k, L"ProcessorNameString", nullptr, nullptr, (BYTE*)name, &sz) == ERROR_SUCCESS)
                h.cpu = Trim(name);
            RegCloseKey(k);
        }
#else
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        h.threads = n > 0 ? (int)n : 0;
        long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
        if (pages > 0 && page > 0) h.ramGB = (int)(((unsigned long long)pages * page + (512ull << 20)) >> 30);
        std::ifstream max("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
        long khz = 0;
        if (max >> khz && khz > 0) h.mhz = (int)(khz / 1000);
        std::ifstream info("/proc/cpuinfo");
        std::string line;
        while (std::getline(info, line)) {
            size_t c = line.find(':');
            if (c == std::string::npos) continue;
            std::string key = line.substr(0, c), val = Trim(line.substr(c + 1));
            if (h.cpu.empty() && key.find("model name") == 0) h.cpu = val;
            if (!h.mhz && key.find("cpu MHz") == 0) h.mhz = atoi(val.c_str());
        }
#endif
        if (h.threads <= 2 || (h.mhz > 0 && h.mhz < 1800) || (h.ramGB > 0 && h.ramGB < 4))
            h.speed = Speed::Conservative;
        else if (h.threads >= 8 && (h.mhz == 0 || h.mhz >= 2800) && (h.ramGB == 0 || h.ramGB >= 8))
            h.speed = Speed::Fast;
        return h;
    }

    // Multiplier for all waits
    double Factor() const { return speed == Speed::Fast ? 0.5 : speed == Speed::Normal ? 1.0 : 2.0; }

    const TChar* SpeedName() const {
        return speed == Speed::Fast ? AR_T("fast") : speed == Speed::Normal ? AR_T("normal") : AR_T("conservative");
    }

    tstring Describe() const {
        tstring s = Num(threads) + AR_T(" threads");
        if (mhz) {
            tstring ghz = Num(mhz / 1000) + AR_T(".") + Num((mhz % 1000) / 100);
            s += AR_T(", ") + ghz + AR_T(" GHz");
        }
        if (ramGB) s += AR_T(", ") + Num(ramGB) + AR_T(" GB RAM");
        return s;
    }
};

// Effective waits for one switch (milliseconds)
struct Timing {
    int settleMs = 150;       // pause after switching, before playback continues
    int vmFirstPollMs = 60;   // first look at the Voicemeeter engine after a restart
    int vmTimeoutMs = 3000;   // maximum wait for the Voicemeeter engine
    int vmRestartMs = 400;    // minimum pause after an engine restart without rate change

    static Timing For(const Hardware& hw, bool automatic, int manualSettleMs) {
        Timing t;
        double f = hw.Factor();
        t.settleMs = automatic ? (int)(160 * f) : manualSettleMs;
        t.vmFirstPollMs = (int)(60 * f);
        t.vmRestartMs = (int)(400 * f);
        t.vmTimeoutMs = (int)(3000 * (f < 1.0 ? 1.0 : f));
        return t;
    }
};

}  // namespace ar
