// AIMP AutoRate - stellt Windows-Geraeteformat (und Voicemeeter-Kette) auf die Abtastrate des Tracks.
// Gegen AIMP SDK v6.00 (build 3083) abgeglichen.
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

#include "apiCore.h"
#include "apiFileManager.h"
#include "apiMessages.h"
#include "apiObjects.h"
#include "apiPlayer.h"
#include "apiPlaylists.h"
#include "apiPlugin.h"
#include "apiThreading.h"
#include "IUnknownImpl.h"   // SDK-Helfer: Referenzzaehler startet bei 0 (AIMP-Konvention)

#include "RateController.h"

// Aufgabe, die im AIMP-Hauptthread laufen soll
class Task : public IUnknownImpl<IAIMPTask> {
    std::function<void()> fn_;
public:
    explicit Task(std::function<void()> f) : fn_(std::move(f)) {}
    void WINAPI Execute(IAIMPTaskOwner*) override { fn_(); }
};

struct Job {
    uint32_t rate = 0;
    IAIMPPlaylistItem* item = nullptr;
    unsigned gen = 0;
};

struct Engine {
    IAIMPCore* core = nullptr;
    IAIMPServicePlayer* player = nullptr;
    IAIMPServiceMessageDispatcher* disp = nullptr;
    IAIMPServiceThreads* threads = nullptr;
    IAIMPMessageHook* hook = nullptr;
    ar::Config cfg;

    std::thread worker;
    std::mutex m;
    std::condition_variable cv;
    bool quit = false, has = false;
    std::atomic<bool> done{false};
    Job job;
    std::atomic<unsigned> gen{0};

    void OnMain(std::function<void()> f) {
        Task* t = new Task(std::move(f));
        t->AddRef();
        threads->ExecuteInMainThread(t, AIMP_SERVICE_THREADS_FLAGS_WAITFOR);
        t->Release();
    }

    // Ausgabe-Einstellung von AIMP ins Log schreiben (Hilfe fuer die AimpDevice-Einstellung)
    void LogAimpOutput() {
        IAIMPPropertyList* pl = nullptr;
        if (FAILED(player->QueryInterface(IID_IAIMPPropertyList, (void**)&pl)) || !pl) return;
        IAIMPString* s = nullptr;
        if (SUCCEEDED(pl->GetValueAsObject(AIMP_PLAYER_PROPID_OUTPUT, IID_IAIMPString, (void**)&s)) && s) {
            static std::wstring last;
            std::wstring cur(s->GetData(), s->GetLength());
            if (cur != last) { ar::Log(L"AIMP-Ausgabe: %s", cur.c_str()); last = cur; }
            s->Release();
        }
        pl->Release();
    }

    void OnStreamStart() {
        if (!cfg.enabled) return;
        IAIMPFileInfo* info = nullptr;
        if (FAILED(player->GetInfo(&info)) || !info) return;
        INT32 sr = 0;
        info->GetValueAsInt32(AIMP_FILEINFO_PROPID_SAMPLERATE, &sr);
        info->Release();
        if (sr <= 0) return;
        LogAimpOutput();
        IAIMPPlaylistItem* item = nullptr;
        player->GetPlaylistItem(&item);
        unsigned g = ++gen;
        std::lock_guard<std::mutex> l(m);
        if (has && job.item) job.item->Release();  // aelteren, unbearbeiteten Job verwerfen
        job = Job{(uint32_t)sr, item, g};
        has = true;
        cv.notify_one();
    }

    void Handle(const Job& j, ar::RateController& rc) {
        ar::Chain chain = rc.Resolve();
        uint32_t rate = rc.ChooseRate(chain, j.rate);
        if (!rate) { ar::Log(L"Keine passende Rate fuer %u Hz", j.rate); return; }
        if (!rc.NeedsSwitch(chain, rate)) return;  // alles schon passend -> nichts tun
        ar::Log(L"Track %u Hz -> Umschalten auf %u Hz", j.rate, rate);
        if (gen != j.gen) return;
        OnMain([&] { player->Stop(); });
        rc.Apply(chain, rate);
        if (gen != j.gen) return;  // Nutzer hat inzwischen etwas anderes gestartet
        OnMain([&] { if (j.item) player->Play2(j.item); });
    }

    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ar::RateController rc(cfg);
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> l(m);
                cv.wait(l, [&] { return quit || has; });
                if (quit) break;
                j = job;
                has = false;
                job.item = nullptr;
            }
            Handle(j, rc);
            if (j.item) j.item->Release();
        }
        CoUninitialize();
        done = true;
    }
};

static Engine* g_engine = nullptr;

class Hook : public IUnknownImpl<IAIMPMessageHook> {
public:
    void WINAPI CoreMessage(DWORD msg, INT32, void*, HRESULT*) override {
        if (msg == AIMP_MSG_EVENT_STREAM_START && g_engine) g_engine->OnStreamStart();
    }
};

class Plugin : public IUnknownImpl<IAIMPPlugin> {
public:
    PChar WINAPI InfoGet(INT32 id) override {
        switch (id) {
            case AIMP_PLUGIN_INFO_NAME: return const_cast<PChar>(L"AutoRate");
            case AIMP_PLUGIN_INFO_AUTHOR: return const_cast<PChar>(L"AutoRate");
            case AIMP_PLUGIN_INFO_SHORT_DESCRIPTION:
                return const_cast<PChar>(L"Passt Windows-Geraet und Voicemeeter automatisch an die Abtastrate an");
        }
        return nullptr;
    }
    DWORD WINAPI InfoGetCategories() override { return AIMP_PLUGIN_CATEGORY_ADDONS; }

    HRESULT WINAPI Initialize(IAIMPCore* core) override {
        auto* e = new Engine();
        e->core = core;
        e->cfg = ar::Config::Load();
        if (FAILED(core->QueryInterface(IID_IAIMPServicePlayer, (void**)&e->player)) ||
            FAILED(core->QueryInterface(IID_IAIMPServiceMessageDispatcher, (void**)&e->disp)) ||
            FAILED(core->QueryInterface(IID_IAIMPServiceThreads, (void**)&e->threads))) {
            ar::Log(L"AIMP-Services nicht verfuegbar");
            delete e;
            return E_FAIL;
        }
        g_engine = e;
        e->worker = std::thread([e] { e->Run(); });
        e->hook = new Hook();
        e->hook->AddRef();
        e->disp->Hook(e->hook);
        ar::Log(L"Plugin gestartet");
        return S_OK;
    }

    HRESULT WINAPI Finalize() override {
        Engine* e = g_engine;
        if (!e) return S_OK;
        g_engine = nullptr;
        e->disp->Unhook(e->hook);
        e->hook->Release();
        {
            std::lock_guard<std::mutex> l(e->m);
            e->quit = true;
            e->cv.notify_all();
        }
        for (int i = 0; i < 60 && !e->done; i++) Sleep(50);  // max. 3 s warten (Deadlock-Schutz)
        bool finished = e->done;
        if (finished) e->worker.join(); else e->worker.detach();
        e->player->Release();
        e->disp->Release();
        e->threads->Release();
        if (finished) delete e;  // sonst bewusst leaken, der Worker laeuft noch
        return S_OK;
    }

    void WINAPI SystemNotification(INT32, IUnknown*) override {}
};

extern "C" __declspec(dllexport) HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin** header) {
    *header = new Plugin();
    return S_OK;
}
