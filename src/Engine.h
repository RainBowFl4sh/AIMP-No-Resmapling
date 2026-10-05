// Connects AIMP with the RateController: collects events on the main thread, performs the
// switching on a worker thread and keeps the statistics.
#pragma once
#include <condition_variable>
#ifndef _WIN32
#include <dlfcn.h>
#include <sys/stat.h>
#endif
#include <functional>

#include "AimpOutput.h"
#include "AimpUtil.h"
#include "Hardware.h"
#include "RateController.h"
#include "Updater.h"
#include "apiFileManager.h"
#include "apiMessages.h"
#include "apiPlayer.h"
#include "apiPlaylists.h"
#include "apiThreading.h"
#ifdef _WIN32
#include "win/Restart.h"
#endif

namespace ar {

class Task : public ComObject<IAIMPTask> {
    std::function<void()> fn_;

protected:
    bool Supports(REFIID riid) override { return EqualGUID(riid, IID_IAIMPTask); }

public:
    explicit Task(std::function<void()> f) : fn_(std::move(f)) {}
    void WINAPI Execute(IAIMPTaskOwner*) override { fn_(); }
};

struct Job {
    TrackInfo track;
    Config cfg;
    tstring file;
    Ptr<IAIMPPlaylistItem> item;
    unsigned gen = 0;
    bool selfRestart = false;  // playback restarted by the plugin itself (not counted)
};

class Engine {
public:
    Ptr<IAIMPServicePlayer> player;
    Ptr<IAIMPServiceThreads> threads;
    Config cfg;                // read/written on the main thread only
    Stats stats;
    Hardware hw;               // detected once at start-up
    Updater updater;           // About tab: update check (main thread)
    AimpOutputSync aimpSync;   // main thread only
    std::function<void()> onStatsChanged;  // main thread; refreshes the settings page

private:
    std::thread worker_;
    std::mutex m_;
    std::condition_variable cv_;
    std::atomic<bool> quit_{false};
    double pendingSeek_ = 0;  // AIMP 4: position to seek to once the track is open (main thread)
    std::atomic<bool> selfRestart_{false};  // AIMP is being closed by the plugin to apply a new rate
    std::thread watcher_;                   // update: waits for AIMP to install the new plugin files
    std::atomic<bool> watchStop_{false};
    bool sessionLoaded_ = false;            // worker only
    bool waitForVm_ = false;                // worker only: switch as soon as Voicemeeter runs
    bool has_ = false;
    std::atomic<bool> done_{false};
    Job job_;
    std::atomic<unsigned> gen_{0};
    // Protection against endless loops if a device ignores the switch
    tstring lastFile_;
    uint32_t lastRate_ = 0;
    int64_t lastTime_ = 0;
    tstring restartFile_;  // main thread only: file the plugin is restarting right now
    bool resumed_ = false;
    bool waitingForPlay_ = false;  // main thread only: a track was loaded while AIMP was not playing
    tstring handledFile_;          // main thread only: file of the last track the plugin looked at
    Config shutdownCfg_;

    void OnMain(std::function<void()> f, bool wait) {
        if (quit_) return;  // Finalize waits on the main thread -> do not block
        Ptr<IAIMPTask> t;
        Task* raw = new Task(std::move(f));
        raw->AddRef();
        *t.Out() = raw;
        threads->ExecuteInMainThread(t.Get(), wait ? AIMP_SERVICE_THREADS_FLAGS_WAITFOR : 0);
    }

    tstring AimpOutput() {
        Ptr<IAIMPPropertyList> pl;
        if (Failed(player->QueryInterface(IID_IAIMPPropertyList, pl.OutV())) || !pl) return tstring();
        Ptr<IAIMPString> s;
        if (Failed(pl->GetValueAsObject(AIMP_PLAYER_PROPID_OUTPUT, IID_IAIMPString, s.OutV())) || !s)
            return ConfiguredAimpOutput();  // AIMP 4
        return FromString(s.Get());
    }

    // Prepares the restart with a new output rate (main thread): resume file, helper process and,
    // in seamless mode, waits until the still image is on screen. AIMP itself is closed
    // asynchronously afterwards (see Handle). false = not possible.
    bool RestartAimpForRate(const tstring& file, double pos, uint32_t hz, bool seamless) {
#ifdef _WIN32
        if (aimpSync.rateKey.empty() || aimpSync.profileDir.empty()) return false;
        ResumeInfo r;
        r.file = file;
        r.position = pos;
        r.rate = hz;
        r.time = (long long)time(nullptr);
        aimpSync.SaveResume(r);
        HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, win::ReadyEventName(GetCurrentProcessId()).c_str());
        if (!win::LaunchRestartHelper(hz, aimpSync.profileDir + AR_T("AIMP.ini"), aimpSync.rateKey, seamless)) {
            if (ready) CloseHandle(ready);
            return false;
        }
        if (ready) {
            WaitForSingleObject(ready, seamless ? 2500 : 1000);
            CloseHandle(ready);
        }
        Log(AR_T("Restarting AIMP with ") + Khz(hz) + AR_T(" (") + aimpSync.rateKey + AR_T(")"));
        return true;
#else
        (void)file; (void)pos; (void)hz; (void)seamless;
        return false;
#endif
    }

    void Handle(Job& j, RateController& rc) {
        waitForVm_ = false;
        if (!sessionLoaded_) {
            // After a restart by the plugin: take over the original rates of the previous AIMP process
            sessionLoaded_ = true;
            OnMain([this, &rc] { rc.LoadSession(!aimpSync.resume.file.empty()); }, true);
        }
        Timing tm = Timing::For(hw, j.cfg.autoTiming, j.cfg.settleMs);
        rc.SetConfig(j.cfg, tm);
        OutputMode mode = rc.Resolve(j.track.aimpOutput);

        // Which rate does AIMP itself play with? (AIMPSoundOut\DeviceFreq)
        uint32_t aimpRate = 0;
        OnMain([this, &aimpRate] { aimpRate = aimpSync.AimpRate(); }, true);

        // ASIO / WASAPI exclusive / DirectSound: AIMP only follows the track if it is restarted.
        // Without the (experimental) restart option the plugin does not follow the track there; it
        // only keeps a Voicemeeter chain at AIMP's own rate so the pitch is always right.
        bool restartOutput = NeedsAimpRestart(mode);
#ifdef _WIN32
        bool canRestart = restartOutput && j.cfg.restartAimp && aimpRate;
#else
        bool canRestart = false;
#endif
        bool passive = restartOutput && !canRestart;
        TrackInfo t = j.track;
        tstring note;
        if (passive) {
            if (!aimpRate) {
                stats.Update([&](Stats& s) {
                    s.modeText = ModeName(mode);
                    s.note = AR_T("restart option off - nothing changed");
                    if (!j.selfRestart) s.skipped++;
                });
                if (!j.selfRestart)
                    Log(AR_T("Track ") + Khz(j.track.rate) + AR_T(" | ") + ModeName(mode) +
                        AR_T(" | restart option off - nothing changed"));
                return;
            }
            t.rate = aimpRate;
            note = tstring(AR_T("restart option off - devices stay at AIMP's rate (")) + Khz(aimpRate) + AR_T(")");
        }
#ifndef _WIN32
        // AIMP for Linux also resamples every track to its own fixed rate (AIMPSoundOut\DeviceFreq),
        // whatever the device does. Forcing PipeWire to the track's rate would resample a second time
        // (e.g. 96 -> 44.1 in AIMP, 44.1 -> 96 in PipeWire), so PipeWire follows AIMP's rate instead.
        if (!aimpRate) {
            stats.Update([&](Stats& s) {
                s.modeText = ModeName(mode);
                s.note = AR_T("AIMP's output rate unknown - nothing changed");
                if (!j.selfRestart) s.skipped++;
            });
            if (!j.selfRestart)
                Log(AR_T("Track ") + Khz(j.track.rate) + AR_T(" | ") + ModeName(mode) +
                    AR_T(" | AIMP's output rate unknown - nothing changed"));
            return;
        }
        if (t.rate != aimpRate)
            note = tstring(AR_T("AIMP resamples to its own rate (")) + Khz(aimpRate) + AR_T(")") +
                   (mode == OutputMode::PipeWire ? AR_T(" - PipeWire follows AIMP") : AR_T(""));
        t.rate = aimpRate;
#endif

        Decision d = rc.Prepare(t);
#ifdef _WIN32
        if (passive && !d.vm) d.apply = d.stopPlayback = false;  // nothing to keep in sync
        if (d.vmNotRunning) {
            waitForVm_ = true;
            note += tstring(note.empty() ? AR_T("") : AR_T("; ")) + AR_T("Voicemeeter is not running - switching as soon as it starts");
        }
#endif
        tstring vmText = rc.VoicemeeterStatus();
        stats.Update([&](Stats& s) {
            s.modeText = ModeName(d.mode);
            if (j.cfg.mode == OutputMode::Auto) s.modeText = tstring(AR_T("Automatic -> ")) + s.modeText;
            s.chainText = d.chain;
            s.vmText = vmText;
            s.deviceRate = d.currentRate;
            s.aimpRate = aimpRate;
            s.note = note;
            s.hardwareText = hw.Describe() + AR_T(" -> ") + hw.SpeedName() + AR_T(" profile, ") + Num(tm.settleMs) +
                             AR_T(" ms settle time");
        });
        if (!j.selfRestart)
            Log(AR_T("Track ") + Khz(j.track.rate) + AR_T(" | AIMP output: ") +
                (j.track.aimpOutput.empty() ? tstring(AR_T("?")) : j.track.aimpOutput) + AR_T(" | AIMP rate: ") +
                (aimpRate ? Khz(aimpRate) : tstring(AR_T("?"))) + AR_T(" | ") + ModeName(d.mode) +
                (d.chain.empty() ? tstring() : AR_T(" | ") + d.chain) + (note.empty() ? tstring() : AR_T(" | ") + note));
        if (!d.info.empty()) Log(d.info);
        if (!d.rate) {
            if (d.info.empty()) Log(AR_T("No suitable rate found for ") + Khz(t.rate));
            stats.Update([](Stats& s) { s.failures++; });
            return;
        }

        // AIMP itself has to play with the target rate -> otherwise restart it with the new rate
        bool aimpNeeds = canRestart && aimpRate != d.rate;
        if (aimpNeeds && aimpSync.resume.rate == d.rate && (long long)time(nullptr) - aimpSync.resume.time < 90) {
            Log(AR_T("  AIMP did not take ") + Khz(d.rate) + AR_T(" after the restart - no further restart"));
            aimpNeeds = false;
        }
        bool devApply = d.apply;
        if (aimpNeeds) d.apply = d.stopPlayback = true;
        if (!d.apply) {
            stats.Update([&](Stats& s) {
                if (!j.selfRestart) s.skipped++;
                if (!s.deviceRate) s.deviceRate = d.rate;
            });
            return;
        }
        if (!aimpNeeds && j.file == lastFile_ && d.rate == lastRate_ && NowMs() - lastTime_ < 15000) {
            Log(AR_T("Switching to ") + Khz(d.rate) + AR_T(" has no effect - skipped for this track"));
            stats.Update([](Stats& s) { s.failures++; });
            return;
        }
        if (gen_ != j.gen) return;  // another track is playing meanwhile

        Log(AR_T("  -> switching output to ") + Khz(d.rate) +
            (aimpNeeds ? tstring(AR_T(" (AIMP: ")) + Khz(aimpRate) + AR_T(" -> ") + Khz(d.rate) + AR_T(", restart)") : tstring()));
        int64_t t0 = NowMs();
        double pos = 0;
        if (d.stopPlayback) {
            bool playing = true;
            OnMain([this, &pos, &playing] {
                playing = player->GetState() == AIMP_PLAYER_STATE_PLAYING;
                if (!playing) { waitingForPlay_ = true; return; }  // paused meanwhile: redo on play
                player->GetPosition(&pos);
                player->Stop();
            }, true);
            if (!playing) {
                Log(AR_T("  playback was paused meanwhile - switching when it continues"));
                return;
            }
        }
        uint32_t result = devApply ? rc.Apply(d) : d.rate;
        int64_t dt = NowMs() - t0;
        lastFile_ = j.file;
        lastRate_ = d.rate;
        lastTime_ = NowMs();
        stats.Update([&](Stats& s) {
            if (result) {
                s.switches++;
                s.lastSwitchMs = dt;
                s.switchMsTotal += dt;
                s.deviceRate = result;
            } else {
                s.failures++;
            }
#ifdef _WIN32
            if (d.vm && devApply && d.vmEngine) s.vmRestarts++;
#endif
        });
        if (quit_ || gen_ != j.gen) return;

        if (aimpNeeds) {
            tstring file = j.file;
            uint32_t hz = d.rate;
            bool seamless = j.cfg.seamlessRestart;
            bool restarting = false;
            OnMain([this, file, pos, hz, seamless, &restarting] { restarting = RestartAimpForRate(file, pos, hz, seamless); }, true);
            if (restarting) {
                selfRestart_ = true;  // keep the new rates when this AIMP closes (see Run)
                stats.Update([](Stats& s) { s.aimpRestarts++; });
                // Do not wait: while closing, AIMP calls Finalize, which waits for this worker
                OnMain([] {
                    auto sd = Service<IAIMPServiceShutdown>(IID_IAIMPServiceShutdown);
                    if (!sd || Failed(sd->Shutdown(AIMP_SERVICE_SHUTDOWN_FLAGS_CLOSE_APP | AIMP_SERVICE_SHUTDOWN_FLAGS_NO_CONFIRM)))
                        Log(AR_T("AIMP could not be closed"));
                }, false);
                return;  // continues after the start (ResumeAfterRestart)
            }
        }
        if (d.stopPlayback) {
            IAIMPPlaylistItem* item = j.item.Get();
            tstring file = j.file;
            float offset = pos > 1.0 ? (float)pos : 0.0f;  // continue at the same position
            OnMain([this, item, file, offset] {
                restartFile_ = file;
                PlayAt(item, file, offset, AIMP_SERVICE_PLAYER_FLAGS_PLAY_WITHOUT_ADDING_TO_PLAYLIST);
            }, true);
        }
    }

    // Worker: Voicemeeter was not running for the current track - has it been started meanwhile?
    void CheckVoicemeeterStarted() {
#ifdef _WIN32
        if (!win::Voicemeeter::Get().Running()) return;
        waitForVm_ = false;
        lastFile_.clear();  // the same track again is not a switch "without effect" now
        Log(AR_T("Voicemeeter is running now - switching for the current track"));
        OnMain([this] { RecheckSamePlayback(); }, false);
#endif
    }

    void Run() {
#ifdef _WIN32
        // STA, because ASIO drivers are apartment-threaded COM objects without marshalling
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
        RateController rc;
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> l(m_);
                auto ready = [&] { return quit_ || has_; };
                if (!waitForVm_) cv_.wait(l, ready);
                else if (!cv_.wait_for(l, std::chrono::seconds(3), ready)) {
                    l.unlock();
                    CheckVoicemeeterStarted();
                    continue;
                }
                if (quit_) break;
                j = std::move(job_);
                job_ = Job();
                has_ = false;
            }
            Handle(j, rc);
            if (!quit_) OnMain([this] { if (onStatsChanged) onStatsChanged(); }, false);
        }
        rc.SetConfig(shutdownCfg_, Timing::For(hw, shutdownCfg_.autoTiming, shutdownCfg_.settleMs));
        // The main thread waits in Stop() meanwhile, so the settings file can be written from here.
        // Closed by the plugin for a new rate: keep the devices at that rate, remember the originals.
        if (selfRestart_) rc.SaveSession();
        else rc.Shutdown();
#ifdef _WIN32
        win::Voicemeeter::Get().Shutdown();
        if (SUCCEEDED(co)) CoUninitialize();
#endif
        done_ = true;
    }

public:
    void Start() {
        hw = Hardware::Detect();
        Timing tm = Timing::For(hw, cfg.autoTiming, cfg.settleMs);
        Log(AR_T("Hardware: ") + (hw.cpu.empty() ? tstring() : hw.cpu + AR_T(", ")) + hw.Describe() + AR_T(" -> ") +
            hw.SpeedName() + AR_T(" timing profile (settle ") + Num(tm.settleMs) + AR_T(" ms)"));
        stats.Update([&](Stats& s) {
            s.hardwareText = hw.Describe() + AR_T(" -> ") + hw.SpeedName() + AR_T(" profile, ") + Num(tm.settleMs) +
                             AR_T(" ms settle time");
        });
        worker_ = std::thread([this] { Run(); });
    }

    // ---- Update: AIMP installs the downloaded .aimppack after the user confirmed it. The running plugin
    // stays loaded, so it notices the new files on disk itself and restarts AIMP to load them.
    struct FileStamp {
        long long size = -1, time = 0, id = 0;
        bool operator!=(const FileStamp& o) const { return size != o.size || time != o.time || id != o.id; }
    };
    static FileStamp Stamp(const tstring& path) {
        FileStamp f;
#ifdef _WIN32
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) {
            f.size = ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
            f.time = ((long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
        }
#else
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            f.size = (long long)st.st_size;
            f.time = (long long)st.st_mtime;
            f.id = (long long)st.st_ino;  // the new .so usually replaces the file (new inode)
        }
#endif
        return f;
    }
    static tstring PluginFile() {
#ifdef _WIN32
        return win::ModulePath(win::ThisModule());
#else
        Dl_info info;
        if (dladdr((void*)&Engine::Stamp, &info) && info.dli_fname) return info.dli_fname;
        return tstring();
#endif
    }

public:
    void WatchForUpdate() {
        if (watcher_.joinable()) return;  // already watching
        tstring path = PluginFile();
        if (path.empty()) return;
        FileStamp before = Stamp(path);
        Log(AR_T("Update: waiting for AIMP to install the new version (") + path + AR_T(")"));
        watchStop_ = false;
        watcher_ = std::thread([this, path, before] {
            int64_t end = NowMs() + 30 * 60 * 1000;  // give up after 30 minutes (installation cancelled)
            while (!watchStop_ && NowMs() < end) {
                for (int i = 0; i < 10 && !watchStop_; i++) SleepMs(100);
                if (watchStop_) return;
                if (Stamp(path) != before) {
                    SleepMs(1500);  // let AIMP finish writing all files of the package
                    if (!watchStop_) OnMain([this] { RestartForUpdate(); }, false);
                    return;
                }
            }
        });
    }

    // Main thread: continue the current track after the restart, keep the current rates, restart AIMP
    void RestartForUpdate() {
        tstring file;
        double pos = 0;
        if (player && player->GetState() == AIMP_PLAYER_STATE_PLAYING) {
            file = CurrentFile();
            player->GetPosition(&pos);
        }
        if (!file.empty() && !aimpSync.profileDir.empty()) {
            ResumeInfo r;
            r.file = file;
            r.position = pos;
            r.rate = 0;
            r.time = (long long)time(nullptr);
            aimpSync.SaveResume(r);
        }
        Log(AR_T("Update installed - restarting AIMP to load the new version"));
        updater.Installed();
        selfRestart_ = true;  // keep the rates; the new version takes the session over
        auto sd = Service<IAIMPServiceShutdown>(IID_IAIMPServiceShutdown);
        auto none = MakeString(tstring());
        if (!sd || Failed(sd->Restart(none.Get()))) {
            selfRestart_ = false;
            Log(AR_T("AIMP could not restart itself - please restart AIMP to finish the update"));
        }
    }

private:
    void StopWatcher() {
        watchStop_ = true;
        if (watcher_.joinable()) watcher_.join();
    }

public:
    // true = worker finished cleanly; false = still hanging (then deliberately not freed)
    bool Stop() {
        StopWatcher();
        {
            std::lock_guard<std::mutex> l(m_);
            shutdownCfg_ = cfg;
            quit_ = true;
            cv_.notify_all();
        }
        for (int i = 0; i < 160 && !done_; i++) SleepMs(50);  // max. 8 s (restoring incl. Voicemeeter restart)
        if (done_) worker_.join();
        else worker_.detach();
        return done_;
    }

    // Main thread, after AIMP_MSG_EVENT_LOADED: continue the track after a restart by the plugin
    void ResumeAfterRestart() {
        const ResumeInfo& r = aimpSync.resume;
        if (r.file.empty() || resumed_) return;
        resumed_ = true;
        Log(AR_T("Continuing after restart: ") + r.file + AR_T(" at ") + Num((long long)r.position) + AR_T(" s"));
        restartFile_ = r.file;
        DWORD flags = AIMP_SERVICE_PLAYER_FLAGS_PLAY_FROM_PLAYLIST | AIMP_SERVICE_PLAYER_FLAGS_PLAY_FROM_PLAYLIST_CAN_ADD;
        if (Failed(PlayAt(nullptr, r.file, (float)r.position, flags))) Log(AR_T("Continuing failed"));
    }

    // Main thread: plays a playlist item (or else a file) from the given position. AIMP 5.40+ starts
    // there directly (IAIMPServicePlayer2); AIMP 4 starts at the beginning and then seeks.
    HRESULT PlayAt(IAIMPPlaylistItem* item, const tstring& file, float offset, DWORD flags) {
        Ptr<IAIMPServicePlayer2> p2;
        if (offset > 0) player->QueryInterface(IID_IAIMPServicePlayer2, p2.OutV());
        auto s = MakeString(file);
        HRESULT hr = E_FAIL;
        if (item) hr = p2 ? p2->Play2(item, offset, 0) : player->Play2(item);
        else if (s) hr = p2 ? p2->Play4(s.Get(), offset, flags) : player->Play4(s.Get(), flags);
        // AIMP 4 opens the track asynchronously: seek now if possible, otherwise at the stream start
        if (Succeeded(hr) && !p2 && offset > 0) pendingSeek_ = Failed(player->SetPosition(offset)) ? offset : 0;
        return hr;
    }

    // Main thread: seek that PlayAt could not do yet (AIMP 4). finalTry = give up if it fails again.
    void ApplyPendingSeek(bool finalTry) {
        if (pendingSeek_ <= 0) return;
        double pos = pendingSeek_;
        if (Succeeded(player->SetPosition(pos))) pendingSeek_ = 0;
        else if (finalTry) {
            pendingSeek_ = 0;
            Log(AR_T("Could not continue at ") + Num((long long)pos) + AR_T(" s"));
        }
    }

    // Main thread: playback state changed. A track that AIMP loaded without playing it (e.g. the last
    // track restored paused at start-up) is handled as soon as playback really starts.
    // Also covers the track AIMP restores at start-up: it is loaded before the plugin receives any
    // events, so there is no stream start for it - the first "playing" checks it instead.
    void OnPlayerState(int state) {
        if (state != AIMP_PLAYER_STATE_PLAYING) return;
        if (player) ApplyPendingSeek(true);
        if (waitingForPlay_ || CurrentFile() != handledFile_) {
            waitingForPlay_ = false;
            OnStreamStart();
        }
    }

    // Main thread: checks the playing track again without counting it as a new track
    void RecheckSamePlayback() {
        handledFile_.clear();
        if (!player || player->GetState() != AIMP_PLAYER_STATE_PLAYING) return;  // done when playback starts
        restartFile_ = CurrentFile();
        OnStreamStart();
    }

    // Main thread: checks the playing track again (e.g. after the plugin was enabled)
    void Recheck() {
        handledFile_.clear();
        if (player && player->GetState() == AIMP_PLAYER_STATE_PLAYING) OnStreamStart();
    }

    tstring CurrentFile() {
        Ptr<IAIMPFileInfo> info;
        Ptr<IAIMPString> s;
        if (!player || Failed(player->GetInfo(info.Out())) || !info) return tstring();
        if (Succeeded(info->GetValueAsObject(AIMP_FILEINFO_PROPID_FILENAME, IID_IAIMPString, s.OutV())) && s)
            return FromString(s.Get());
        return tstring();
    }

    // Main thread: AIMP started a new stream
    void OnStreamStart() {
        if (player) ApplyPendingSeek(false);
        if (!cfg.enabled || !player) return;
        // Only act while AIMP is really playing: switching restarts playback, which must never start
        // music that the user had paused or stopped.
        if (player->GetState() != AIMP_PLAYER_STATE_PLAYING) {
            waitingForPlay_ = true;
            return;
        }
        waitingForPlay_ = false;
        Ptr<IAIMPFileInfo> info;
        if (Failed(player->GetInfo(info.Out())) || !info) return;
        Job j;
        INT32 v = 0;
        if (Succeeded(info->GetValueAsInt32(AIMP_FILEINFO_PROPID_SAMPLERATE, &v)) && v > 0) j.track.rate = (uint32_t)v;
        if (!j.track.rate) return;
        if (Succeeded(info->GetValueAsInt32(AIMP_FILEINFO_PROPID_BITDEPTH, &v)) && v > 0) j.track.bits = (uint32_t)v;
        if (Succeeded(info->GetValueAsInt32(AIMP_FILEINFO_PROPID_CHANNELS, &v)) && v > 0) j.track.channels = (uint32_t)v;
        Ptr<IAIMPString> s;
        if (Succeeded(info->GetValueAsObject(AIMP_FILEINFO_PROPID_CODEC, IID_IAIMPString, s.OutV())) && s)
            j.track.codec = FromString(s.Get());
        if (Succeeded(info->GetValueAsObject(AIMP_FILEINFO_PROPID_FILENAME, IID_IAIMPString, s.OutV())) && s)
            j.file = FromString(s.Get());
        j.track.aimpOutput = AimpOutput();
        handledFile_ = j.file;
        player->GetPlaylistItem(j.item.Out());
        j.cfg = cfg;
        j.gen = ++gen_;
        j.selfRestart = !restartFile_.empty() && restartFile_ == j.file;
        restartFile_.clear();
        stats.Update([&](Stats& st) {
            if (!j.selfRestart) {
                st.tracks++;
                st.rates[j.track.rate]++;
            }
            st.trackRate = j.track.rate;
            st.trackBits = j.track.bits;
            st.trackChannels = j.track.channels;
            st.trackCodec = j.track.codec;
            st.aimpOutput = j.track.aimpOutput;
        });
        std::lock_guard<std::mutex> l(m_);
        job_ = std::move(j);  // drop an older job that was not handled yet
        has_ = true;
        cv_.notify_one();
    }

    // ---- Counters are kept in the settings file, section [Statistics] (main thread)
    void LoadStats() {
        Ini& i = Ini::Get();
        const tstring sec = AR_T("Statistics");
        auto rd = [&](const TChar* n) { return (long)i.Int(sec, n, 0); };
        tstring rates = i.Str(sec, AR_T("Rates"));
        stats.Update([&](Stats& s) {
            s.tracks = rd(AR_T("Tracks"));
            s.switches = rd(AR_T("Switches"));
            s.skipped = rd(AR_T("Skipped"));
            s.failures = rd(AR_T("Failures"));
            s.vmRestarts = rd(AR_T("VoicemeeterRestarts"));
            s.aimpRestarts = rd(AR_T("AimpRestarts"));
            s.switchMsTotal = i.Int(sec, AR_T("SwitchMsTotal"), 0);
            for (auto& part : Split(rates, AR_T(';'))) {
                size_t eq = part.find(AR_T('='));
                if (eq == tstring::npos) continue;
                try {
                    long hz = std::stol(part.substr(0, eq)), n = std::stol(part.substr(eq + 1));
                    if (hz > 0 && n > 0) s.rates[(uint32_t)hz] = n;
                } catch (...) {  // damaged entry - ignore it
                }
            }
        });
    }

    void SaveStats() {
        Ini& i = Ini::Get();
        const tstring sec = AR_T("Statistics");
        Stats s = stats.Snapshot();
        i.Set(sec, AR_T("Tracks"), (long long)s.tracks);
        i.Set(sec, AR_T("Switches"), (long long)s.switches);
        i.Set(sec, AR_T("Skipped"), (long long)s.skipped);
        i.Set(sec, AR_T("Failures"), (long long)s.failures);
        i.Set(sec, AR_T("VoicemeeterRestarts"), (long long)s.vmRestarts);
        i.Set(sec, AR_T("AimpRestarts"), (long long)s.aimpRestarts);
        i.Set(sec, AR_T("SwitchMsTotal"), (long long)s.switchMsTotal);
        tstring rates;
        for (auto& r : s.rates) rates += Num(r.first) + AR_T("=") + Num(r.second) + AR_T(";");
        i.Set(sec, AR_T("Rates"), rates);
        i.Save();
    }
};

}  // namespace ar
