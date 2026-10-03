// AIMP Prevent Resampling - switches the output (Windows device format, ASIO, Voicemeeter engine or
// PipeWire on Linux) to the sample rate of the current track so nothing gets resampled.
// Built against AIMP SDK v6.00 (build 3083); compiles to a Win32/Win64 DLL and a Linux .so.
#include "OptionsFrame.h"
#include "apiPlugin.h"

#if defined(_MSC_VER)
#define AR_EXPORT  // exported via PreventResampling.def (undecorated name even for 32-bit __stdcall)
#elif defined(_WIN32)
#define AR_EXPORT __declspec(dllexport)  // MinGW: -Wl,--kill-at strips the @4
#else
#define AR_EXPORT __attribute__((visibility("default")))
#endif


namespace ar {

static Engine* g_engine = nullptr;
static OptionsFrame* g_frame = nullptr;

class Hook : public ComObject<IAIMPMessageHook> {
protected:
    bool Supports(REFIID riid) override { return EqualGUID(riid, IID_IAIMPMessageHook); }

public:
    void WINAPI CoreMessage(DWORD msg, INT32 param1, void*, HRESULT*) override {
        if (!g_engine) return;
        if (msg == (DWORD)AIMP_MSG_EVENT_STREAM_START) g_engine->OnStreamStart();
        else if (msg == (DWORD)AIMP_MSG_EVENT_PLAYER_STATE) g_engine->OnPlayerState(param1);
        else if (msg == (DWORD)AIMP_MSG_EVENT_LOADED) {
            g_engine->ResumeAfterRestart();
            const Config& c = g_engine->cfg;
            if (c.updateCheck && Updater::Due(c.updateInterval)) g_engine->updater.Check(false, c.updateAutoInstall);
        }
        else if (msg == (DWORD)AIMP_MSG_EVENT_PLAYER_UPDATE_POSITION && g_frame && g_frame->IsOpen()) g_frame->RefreshStats();
    }
};

// Gear button in the plugin list: opens the plugin's settings page
class SettingsDialog : public ComObject<IAIMPExternalSettingsDialog> {
protected:
    bool Supports(REFIID riid) override { return EqualGUID(riid, IID_IAIMPExternalSettingsDialog); }

public:
    void WINAPI Show(HWND) override {
        auto svc = Service<IAIMPServiceOptionsDialog>(IID_IAIMPServiceOptionsDialog);
        if (svc && g_frame) svc->FrameShow(static_cast<IAIMPOptionsDialogFrame*>(g_frame), 1);
    }
};

class Plugin : public ComObject<IAIMPPlugin> {
    Ptr<IAIMPServiceMessageDispatcher> disp_;
    Ptr<IAIMPMessageHook> hook_;
    Ptr<IAIMPOptionsDialogFrame> frame_;
    Ptr<IAIMPExternalSettingsDialog> settings_;

public:
    Plugin() {
        SettingsDialog* d = new SettingsDialog();
        d->AddRef();
        *settings_.Out() = d;
    }

    // According to the SDK, IAIMPExternalSettingsDialog is queried on the plugin object itself
    HRESULT __unknwncall QueryInterface(REFIID riid, LPVOID* ppv) override {
        if (ppv && EqualGUID(riid, IID_IAIMPExternalSettingsDialog) && settings_) {
            settings_->AddRef();
            *ppv = settings_.Get();
            return S_OK;
        }
        return ComObject<IAIMPPlugin>::QueryInterface(riid, ppv);
    }

    PChar WINAPI InfoGet(INT32 id) override {
        switch (id) {
            // Must match the name of the settings page exactly (links the plugin list entry to it)
            case AIMP_PLUGIN_INFO_NAME: return const_cast<PChar>(AR_T(AR_PLUGIN_NAME));
            case AIMP_PLUGIN_INFO_AUTHOR: return const_cast<PChar>(AR_T(AR_AUTHOR));
            case AIMP_PLUGIN_INFO_SHORT_DESCRIPTION:
                return const_cast<PChar>(AR_T("v" AR_VERSION " \u2013 switches the output sample rate to the track's rate (no resampling)"));
            case AIMP_PLUGIN_INFO_FULL_DESCRIPTION:
                return const_cast<PChar>(AR_T("Version " AR_VERSION ". Supports WASAPI shared, Voicemeeter (engine rate and "
                                              "A1-A5 devices) and PipeWire on Linux; ASIO, WASAPI exclusive and DirectSound "
                                              "via an optional, experimental AIMP restart. For AIMP 4.70, 5 and 6. Disabled after installation - "
                                              "enable it in Preferences -> Plugins -> " AR_PLUGIN_NAME "."));
        }
        return nullptr;
    }
    DWORD WINAPI InfoGetCategories() override { return AIMP_PLUGIN_CATEGORY_ADDONS; }

    HRESULT WINAPI Initialize(IAIMPCore* core) override {
        Core() = core;
        Ptr<IAIMPString> profile;
        tstring dir;
        if (Succeeded(core->GetPath(AIMP_CORE_PATH_PROFILE, profile.Out())) && profile) {
            dir = FromString(profile.Get());
            if (!dir.empty() && dir.back() != AR_T('\\') && dir.back() != AR_T('/')) dir += AR_T("/");
            Logger::Get().SetFile(dir + AR_T("PreventResampling.log"));
        }

        auto* e = new Engine();
        e->player = Service<IAIMPServicePlayer>(IID_IAIMPServicePlayer);
        e->threads = Service<IAIMPServiceThreads>(IID_IAIMPServiceThreads);
        disp_ = Service<IAIMPServiceMessageDispatcher>(IID_IAIMPServiceMessageDispatcher);
        if (!e->player || !e->threads || !disp_) {
            // e.g. AIMP 3.60: its SDK has no threads service (AIMP 4 or newer is required)
            Log(tstring(AR_T("AIMP services not available:")) + (e->player ? AR_T("") : AR_T(" player")) +
                (e->threads ? AR_T("") : AR_T(" threads")) + (disp_ ? AR_T("") : AR_T(" message dispatcher")) +
                AR_T(" - this plugin needs AIMP 4.70 or newer"));
            delete e;
            disp_.Reset();
            Core() = nullptr;
            return E_FAIL;
        }
        tstring settingsDir = SettingsDir();
        if (settingsDir.empty()) settingsDir = dir;
        else settingsDir += AR_T(AR_SEP);
        Ini::Get().Open(settingsDir + AR_T("PreventResampling.ini"));
        if (!Ini::Get().Existed()) {
            // Settings of versions before 2.5 lived in AIMP's own configuration - remove them so a
            // new installation really starts with the defaults (plugin disabled)
            auto c = Service<IAIMPServiceConfig>(IID_IAIMPServiceConfig);
            if (c) {
                for (const TChar* k : {AR_T("PreventResampling"), AR_T("AutoRate")}) {
                    auto ks = MakeString(k);
                    if (ks) c->Delete(ks.Get());
                }
                c->FlushCache();
            }
        }
        e->cfg.Load();
        if (!Ini::Get().Existed()) e->cfg.Save();  // new installation: write all defaults (plugin off)
        e->LoadStats();
        g_engine = e;
        e->Start();

        Hook* h = new Hook();
        h->AddRef();
        *hook_.Out() = h;
        disp_->Hook(hook_.Get());

        OptionsFrame* f = new OptionsFrame(e);
        f->AddRef();
        *frame_.Out() = f;
        g_frame = f;
        core->RegisterExtension(IID_IAIMPServiceOptionsDialog, frame_.Get());
        e->onStatsChanged = [] { if (g_frame && g_frame->IsOpen()) g_frame->RefreshStats(); };
        e->updater.downloadDir = settingsDir.substr(0, settingsDir.size() - 1);
        e->updater.onChanged = [] { if (g_frame && g_frame->IsOpen()) g_frame->RefreshAbout(); };
        e->updater.onPackageOpened = [] { if (g_engine) g_engine->WatchForUpdate(); };

        Log(tstring(AR_T(AR_PLUGIN_NAME " " AR_VERSION " started (")) +
#ifdef _WIN32
            (sizeof(void*) == 8 ? AR_T("Windows x64") : AR_T("Windows x86")) +
#else
            AR_T("Linux") +
#endif
            AR_T(")"));
        Log(AR_T("Settings: ") + Ini::Get().Path() + (Ini::Get().Existed() ? AR_T("") : AR_T(" (new)")));
        if (!e->cfg.enabled)
            Log(AR_T("Plugin is disabled - enable it in Preferences -> Plugins -> " AR_PLUGIN_NAME));
        e->aimpSync.profileDir = dir;
        e->aimpSync.LogDiagnostics(e->player.Get());
        e->aimpSync.LoadResume();
        return S_OK;
    }

    HRESULT WINAPI Finalize() override {
        Engine* e = g_engine;
        if (!e) return S_OK;
        if (disp_ && hook_) disp_->Unhook(hook_.Get());
        hook_.Reset();
        if (frame_) Core()->UnregisterExtension(frame_.Get());
        g_frame = nullptr;
        frame_.Reset();
        g_engine = nullptr;
        e->onStatsChanged = nullptr;
        e->updater.Cancel();
        e->SaveStats();
        bool finished = e->Stop();
        Log(AR_T(AR_PLUGIN_NAME " stopped"));
        if (finished) delete e;  // otherwise leak on purpose, the worker is still running
        disp_.Reset();
        Core() = nullptr;
        return S_OK;
    }

    void WINAPI SystemNotification(INT32, IUnknown*) override {}
};

}  // namespace ar

#ifdef _WIN32
// Restart helper, called as: rundll32 PreventResampling.dll,RestartAimp <arguments>
extern "C" AR_EXPORT void CALLBACK RestartAimpW(HWND, HINSTANCE, LPWSTR cmdLine, int) {
    ar::win::RunRestartHelper(cmdLine);
}
#endif

extern "C" AR_EXPORT HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin** header) {
    if (!header) return E_POINTER;
    ar::Plugin* p = new ar::Plugin();
    p->AddRef();
    *header = p;
    return S_OK;
}
