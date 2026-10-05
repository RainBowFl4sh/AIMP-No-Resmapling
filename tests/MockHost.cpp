// Minimal AIMP stand-in for testing the plugin without AIMP.
// Loads PreventResampling.so/.dll, calls Initialize, simulates track starts with different sample
// rates, runs "main thread" tasks like AIMP does and finalizes the plugin again.
// Linux: expects a fake "pw-metadata" in PATH that writes its arguments to $AR_PW_LOG.
// Windows (also under Wine): checks flow, threading and configuration without real hardware.
#ifndef _WIN32
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/time.h>
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "apiCore.h"
#include "apiFileManager.h"
#include "apiMessages.h"
#include "apiOptions.h"
#include "apiPlayer.h"
#include "apiPlugin.h"
#include "apiInternet.h"
#include "apiThreading.h"
#include "../src/Sha256.h"
#include "mock_ui.h"

static int g_failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) { printf("FAILED: %s (line %d)\n", #c, __LINE__); g_failures++; } \
    } while (0)

typedef std::basic_string<TChar> tstr;
static tstr T(const std::string& a) { return tstr(a.begin(), a.end()); }  // ASCII only
static std::string N(const tstr& w) {
    std::string r;
    for (TChar c : w) r += (c > 0 && c < 128) ? (char)c : '?';
    return r;
}

template <typename Base>
struct Obj : Base {
    std::atomic<long> refs{1};
    std::vector<const GUID*> iids;
    HRESULT QueryInterface(REFIID riid, LPVOID* p) override {
        if (EqualGUID(riid, IID_IUnknown)) { *p = this; this->AddRef(); return S_OK; }
        for (auto* g : iids)
            if (EqualGUID(riid, *g)) { *p = this; this->AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    DWORD AddRef() override { return ++refs; }
    DWORD Release() override { long r = --refs; if (!r) delete this; return r; }
};

struct Str : Obj<IAIMPString> {
    tstr s;
    Str() { iids = {&IID_IAIMPString}; }
    HRESULT Get(INT32, TChar*) override { return E_NOTIMPL; }
    PChar GetData() override { return &s[0]; }
    INT32 GetLength() override { return (INT32)s.size(); }
    INT32 GetHashCode() override { return 0; }
    HRESULT Set(INT32, TChar) override { return E_NOTIMPL; }
    HRESULT SetData(PChar c, INT32 n) override { s.assign(c, n); return S_OK; }
    HRESULT Add(IAIMPString*) override { return E_NOTIMPL; }
    HRESULT Add2(PChar, INT32) override { return E_NOTIMPL; }
    HRESULT ChangeCase(INT32) override { return E_NOTIMPL; }
    HRESULT Clone(IAIMPString**) override { return E_NOTIMPL; }
    HRESULT Compare(IAIMPString*, INT32*, BOOL) override { return E_NOTIMPL; }
    HRESULT Compare2(PChar, INT32, INT32*, BOOL) override { return E_NOTIMPL; }
    HRESULT Delete(INT32, INT32) override { return E_NOTIMPL; }
    HRESULT Find(IAIMPString*, INT32*, INT32, INT32) override { return E_NOTIMPL; }
    HRESULT Find2(TChar*, INT32, INT32*, INT32, INT32) override { return E_NOTIMPL; }
    HRESULT Insert(INT32, IAIMPString*) override { return E_NOTIMPL; }
    HRESULT Insert2(INT32, TChar*, INT32) override { return E_NOTIMPL; }
    HRESULT Replace(IAIMPString*, IAIMPString*, INT32) override { return E_NOTIMPL; }
    HRESULT Replace2(TChar*, INT32, TChar*, INT32, INT32) override { return E_NOTIMPL; }
    HRESULT SubString(INT32, INT32, IAIMPString**) override { return E_NOTIMPL; }
};
static Str* MkStr(const std::string& v) { auto* s = new Str(); s->s = T(v); return s; }

struct FileInfo : Obj<IAIMPFileInfo> {
    int rate = 44100;
    FileInfo() { iids = {&IID_IAIMPFileInfo, &IID_IAIMPPropertyList}; }
    void BeginUpdate() override {}
    void EndUpdate() override {}
    HRESULT Reset() override { return S_OK; }
    HRESULT GetValueAsFloat(INT32, DOUBLE*) override { return E_FAIL; }
    HRESULT GetValueAsInt32(INT32 id, INT32* v) override {
        if (id == AIMP_FILEINFO_PROPID_SAMPLERATE) { *v = rate; return S_OK; }
        if (id == AIMP_FILEINFO_PROPID_BITDEPTH) { *v = 24; return S_OK; }
        if (id == AIMP_FILEINFO_PROPID_CHANNELS) { *v = 2; return S_OK; }
        return E_FAIL;
    }
    HRESULT GetValueAsInt64(INT32, INT64*) override { return E_FAIL; }
    HRESULT GetValueAsObject(INT32 id, CONSTIID, void** o) override {
        if (id == AIMP_FILEINFO_PROPID_CODEC) { *o = MkStr("FLAC"); return S_OK; }
        if (id == AIMP_FILEINFO_PROPID_FILENAME) { *o = MkStr("/musik/track" + std::to_string(rate) + ".flac"); return S_OK; }
        return E_FAIL;
    }
    HRESULT SetValueAsFloat(INT32, const DOUBLE) override { return E_NOTIMPL; }
    HRESULT SetValueAsInt32(INT32, INT32) override { return E_NOTIMPL; }
    HRESULT SetValueAsInt64(INT32, const INT64) override { return E_NOTIMPL; }
    HRESULT SetValueAsObject(INT32, IUnknown*) override { return E_NOTIMPL; }
    HRESULT Assign(IAIMPFileInfo*) override { return E_NOTIMPL; }
    HRESULT Clone(IAIMPFileInfo**) override { return E_NOTIMPL; }
};

// Player: IAIMPServicePlayer + IAIMPPropertyList (for AIMP_PLAYER_PROPID_OUTPUT)
struct PlayerProps;
struct Player : Obj<IAIMPServicePlayer> {
    FileInfo* info = new FileInfo();
    PlayerProps* props = nullptr;
    int stops = 0, plays = 0, reinits = 0;
    int state = AIMP_PLAYER_STATE_PLAYING;
    bool aimp4 = false;  // AIMP 4: no AIMP_PLAYER_PROPID_OUTPUT
#ifdef _WIN32
    std::string output = "WASAPI: Lautsprecher (Mock Audio)";
#else
    std::string output = "ALSA: default";
#endif
    Player();
    HRESULT QueryInterface(REFIID riid, LPVOID* p) override;
    HRESULT Play(IAIMPPlaybackQueueItem*) override { return E_NOTIMPL; }
    HRESULT Play2(IAIMPPlaylistItem*) override { plays++; return S_OK; }
    HRESULT Play3(IAIMPPlaylist*) override { return E_NOTIMPL; }
    HRESULT Play4(IAIMPString*, DWORD) override { plays++; return S_OK; }
    HRESULT GoToNext() override { return E_NOTIMPL; }
    HRESULT GoToPrev() override { return E_NOTIMPL; }
    HRESULT GetDuration(double*) override { return E_NOTIMPL; }
    HRESULT GetPosition(double*) override { return E_NOTIMPL; }
    double seekPos = -1;  // like AIMP 4 (no IAIMPServicePlayer2): position is set after Play
    int seekFailures = 0;  // AIMP 4 opens the track asynchronously: the first seeks can fail
    HRESULT SetPosition(const double v) override {
        if (seekFailures > 0) { seekFailures--; return E_FAIL; }
        seekPos = v;
        return S_OK;
    }
    HRESULT GetMute(BOOL*) override { return E_NOTIMPL; }
    HRESULT SetMute(const BOOL) override { return E_NOTIMPL; }
    HRESULT GetVolume(float*) override { return E_NOTIMPL; }
    HRESULT SetVolume(const float) override { return E_NOTIMPL; }
    HRESULT GetInfo(IAIMPFileInfo** fi) override { info->AddRef(); *fi = info; return S_OK; }
    HRESULT GetPlaylistItem(IAIMPPlaylistItem**) override { return E_FAIL; }
    int GetState() override { return state; }
    HRESULT Pause() override { return E_NOTIMPL; }
    HRESULT Resume() override { return E_NOTIMPL; }
    HRESULT Stop() override { stops++; return S_OK; }
    HRESULT StopAfterTrack() override { return E_NOTIMPL; }
};
struct PlayerProps : Obj<IAIMPPropertyList> {
    Player* owner;
    explicit PlayerProps(Player* p) : owner(p) { iids = {&IID_IAIMPPropertyList}; }
    void BeginUpdate() override {}
    void EndUpdate() override {}
    HRESULT Reset() override { return S_OK; }
    HRESULT GetValueAsFloat(INT32, DOUBLE*) override { return E_FAIL; }
    HRESULT GetValueAsInt32(INT32, INT32*) override { return E_FAIL; }
    HRESULT GetValueAsInt64(INT32, INT64*) override { return E_FAIL; }
    HRESULT GetValueAsObject(INT32 id, CONSTIID iid, void** o) override {
        if (owner->aimp4) return E_FAIL;
        if (id == AIMP_PLAYER_PROPID_OUTPUT && EqualGUID(iid, IID_IAIMPString)) { *o = MkStr(owner->output); return S_OK; }
        return E_FAIL;
    }
    HRESULT SetValueAsFloat(INT32, const DOUBLE) override { return E_NOTIMPL; }
    HRESULT SetValueAsInt32(INT32, INT32) override { return E_NOTIMPL; }
    HRESULT SetValueAsInt64(INT32, const INT64) override { return E_NOTIMPL; }
    HRESULT SetValueAsObject(INT32 id, IUnknown* v) override {
        if (id != AIMP_PLAYER_PROPID_OUTPUT || !v) return E_NOTIMPL;
        IAIMPString* str = nullptr;
        if (Failed(v->QueryInterface(IID_IAIMPString, (void**)&str)) || !str) return E_INVALIDARG;
        owner->output = N(tstr(str->GetData(), str->GetLength()));
        owner->reinits++;
        str->Release();
        return S_OK;
    }
};
Player::Player() { iids = {&IID_IAIMPServicePlayer}; props = new PlayerProps(this); }
HRESULT Player::QueryInterface(REFIID riid, LPVOID* p) {
    if (EqualGUID(riid, IID_IAIMPPropertyList)) { props->AddRef(); *p = props; return S_OK; }
    return Obj<IAIMPServicePlayer>::QueryInterface(riid, p);
}

// "Main thread": tasks from other threads are executed in pump()
struct MainQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::function<void()>> q;
    std::thread::id mainId = std::this_thread::get_id();
    void Pump() {
        for (;;) {
            std::function<void()> f;
            {
                std::lock_guard<std::mutex> g(m);
                if (q.empty()) return;
                f = std::move(q.front());
                q.pop_front();
            }
            f();
        }
    }
} g_main;

struct Threads : Obj<IAIMPServiceThreads> {
    Threads() { iids = {&IID_IAIMPServiceThreads}; }
    HRESULT ExecuteInMainThread(IAIMPTask* t, DWORD flags) override {
        if (std::this_thread::get_id() == g_main.mainId) { t->Execute(nullptr); return S_OK; }
        t->AddRef();
        auto done = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard<std::mutex> g(g_main.m);
            g_main.q.push_back([t, done] { t->Execute(nullptr); t->Release(); *done = true; });
        }
        if (flags & AIMP_SERVICE_THREADS_FLAGS_WAITFOR)
            while (!*done) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return S_OK;
    }
    HRESULT ExecuteInThread(IAIMPTask*, TTaskHandle*) override { return E_NOTIMPL; }
    HRESULT Cancel(TTaskHandle, DWORD) override { return E_NOTIMPL; }
    HRESULT WaitFor(TTaskHandle) override { return E_NOTIMPL; }
};

struct Dispatcher : Obj<IAIMPServiceMessageDispatcher> {
    IAIMPMessageHook* hook = nullptr;
    Dispatcher() { iids = {&IID_IAIMPServiceMessageDispatcher}; }
    HRESULT Send(DWORD, INT32, void*) override { return E_NOTIMPL; }
    DWORD Register(PChar) override { return 0; }
    HRESULT Hook(IAIMPMessageHook* h) override { h->AddRef(); hook = h; return S_OK; }
    HRESULT Unhook(IAIMPMessageHook* h) override { if (h == hook) { hook->Release(); hook = nullptr; } return S_OK; }
};

struct ConfigSvc : Obj<IAIMPServiceConfig> {
    std::vector<std::string> deleted;
    std::map<std::string, std::string> v;
    ConfigSvc() { iids = {&IID_IAIMPServiceConfig}; }
    static std::string K(IAIMPString* s) { return N(tstr(s->GetData(), s->GetLength())); }
    void CheckMain() { CHECK(std::this_thread::get_id() == g_main.mainId); }  // AIMP allows the main thread only
    HRESULT Delete(IAIMPString* k) override { CheckMain(); deleted.push_back(K(k)); return S_OK; }
    HRESULT GetValueAsFloat(IAIMPString*, DOUBLE*) override { return E_FAIL; }
    HRESULT GetValueAsInt32(IAIMPString* k, INT32* r) override {
        CheckMain();
        auto it = v.find(K(k));
        if (it == v.end() || it->second.empty()) return E_FAIL;
        *r = std::atoi(it->second.c_str());
        return S_OK;
    }
    HRESULT GetValueAsInt64(IAIMPString*, INT64*) override { return E_FAIL; }
    HRESULT GetValueAsStream(IAIMPString*, IAIMPStream**) override { return E_FAIL; }
    HRESULT GetValueAsString(IAIMPString* k, IAIMPString** r) override {
        CheckMain();
        auto it = v.find(K(k));
        if (it == v.end()) return E_FAIL;
        *r = MkStr(it->second);
        return S_OK;
    }
    HRESULT SetValueAsFloat(IAIMPString*, const DOUBLE) override { return S_OK; }
    HRESULT SetValueAsInt32(IAIMPString* k, INT32 x) override { CheckMain(); v[K(k)] = std::to_string(x); return S_OK; }
    HRESULT SetValueAsInt64(IAIMPString*, const INT64) override { return S_OK; }
    HRESULT SetValueAsStream(IAIMPString*, IAIMPStream*) override { return S_OK; }
    HRESULT SetValueAsString(IAIMPString* k, IAIMPString* x) override { CheckMain(); v[K(k)] = K(x); return S_OK; }
    HRESULT FlushCache() override { return S_OK; }
};

struct MemStream : Obj<IAIMPMemoryStream> {
    std::string d;
    INT64 pos = 0;
    MemStream() { iids = {&IID_IAIMPStream, &IID_IAIMPMemoryStream}; }
    INT64 GetSize() override { return (INT64)d.size(); }
    HRESULT SetSize(const INT64 v) override { d.resize((size_t)v); return S_OK; }
    INT64 GetPosition() override { return pos; }
    HRESULT Seek(const INT64 o, INT32 m) override { pos = m == 0 ? o : m == 1 ? pos + o : (INT64)d.size() + o; return S_OK; }
    INT32 Read(void* b, DWORD n) override {
        INT64 k = (std::min)((INT64)n, (INT64)d.size() - pos);
        if (k <= 0) return 0;
        memcpy(b, d.data() + pos, (size_t)k);
        pos += k;
        return (INT32)k;
    }
    HRESULT Write(void* b, DWORD n, DWORD* w) override {
        if ((size_t)pos + n > d.size()) d.resize((size_t)pos + n);
        memcpy(&d[(size_t)pos], b, n);
        pos += n;
        if (w) *w = n;
        return S_OK;
    }
    void* GetData() override { return d.empty() ? nullptr : &d[0]; }
};

// HTTP client: answers from a URL -> content map; completion is reported on the main thread
struct HttpSvc : Obj<IAIMPServiceHTTPClient> {
    std::map<std::string, std::string> pages;
    std::vector<std::string> requested;
    HttpSvc() { iids = {&IID_IAIMPServiceHTTPClient}; }
    HRESULT Get(IAIMPString* url, DWORD, IAIMPStream* answer, IAIMPHTTPClientEvents* ev, IAIMPConfig*, TTaskHandle* task) override {
        std::string u = N(tstr(url->GetData(), url->GetLength()));
        requested.push_back(u);
        auto it = pages.find(u);
        bool found = it != pages.end();
        if (found) answer->Write((void*)it->second.data(), (DWORD)it->second.size(), nullptr);
        ev->AddRef();
        g_main.q.push_back([ev, found] { ev->OnComplete(nullptr, found ? 0 : 1); ev->Release(); });
        if (task) *task = 1;
        return S_OK;
    }
    HRESULT Post(IAIMPString*, DWORD, IAIMPStream*, IAIMPStream*, IAIMPHTTPClientEvents*, IAIMPConfig*, TTaskHandle*) override { return E_NOTIMPL; }
    HRESULT Cancel(TTaskHandle, DWORD) override { return S_OK; }
};

IAIMPString* mockui::NewString(const std::basic_string<TChar>& v) { auto* s = new Str(); s->s = v; return s; }
static mockui::Service g_ui;
static mockui::OptionsService g_options;

struct ShutdownSvc : Obj<IAIMPServiceShutdown> {
    int calls = 0, restarts = 0;
    ShutdownSvc() { iids = {&IID_IAIMPServiceShutdown}; }
    HRESULT Restart(IAIMPString*) override { restarts++; return S_OK; }
    HRESULT Shutdown(DWORD) override { calls++; return S_OK; }
};

struct CoreImpl : Obj<IAIMPCore> {
    Player* player = new Player();
    Threads* threads = new Threads();
    Dispatcher* disp = new Dispatcher();
    ConfigSvc* config = new ConfigSvc();
    ShutdownSvc* shutdown = new ShutdownSvc();
    HttpSvc* http = new HttpSvc();
    std::string profile;
    IUnknown* frame = nullptr;
    CoreImpl() { iids = {&IID_IAIMPCore}; }
    HRESULT QueryInterface(REFIID riid, LPVOID* p) override {
        IUnknown* s = nullptr;
        if (EqualGUID(riid, IID_IAIMPServicePlayer)) s = player;
        else if (EqualGUID(riid, IID_IAIMPServiceThreads)) s = threads;
        else if (EqualGUID(riid, IID_IAIMPServiceMessageDispatcher)) s = disp;
        else if (EqualGUID(riid, IID_IAIMPServiceConfig)) s = config;
        else if (EqualGUID(riid, IID_IAIMPServiceShutdown)) s = shutdown;
        else if (EqualGUID(riid, IID_IAIMPServiceHTTPClient)) s = http;
        else if (EqualGUID(riid, IID_IAIMPServiceUI)) s = static_cast<IAIMPServiceUI*>(&g_ui);
        else if (EqualGUID(riid, IID_IAIMPServiceOptionsDialog)) s = static_cast<IAIMPServiceOptionsDialog*>(&g_options);
        if (s) { s->AddRef(); *p = s; return S_OK; }
        return Obj<IAIMPCore>::QueryInterface(riid, p);
    }
    HRESULT CreateObject(CONSTIID iid, void** o) override {
        if (EqualGUID(iid, IID_IAIMPString)) { *o = new Str(); return S_OK; }
        if (EqualGUID(iid, IID_IAIMPMemoryStream)) { *o = static_cast<IAIMPMemoryStream*>(new MemStream()); return S_OK; }
        return E_NOTIMPL;
    }
    HRESULT GetPath(int, IAIMPString** v) override { *v = MkStr(profile); return S_OK; }
    HRESULT RegisterExtension(CONSTIID iid, IUnknown* e) override {
        CHECK(EqualGUID(iid, IID_IAIMPServiceOptionsDialog));
        e->AddRef();
        frame = e;
        return S_OK;
    }
    HRESULT RegisterService(IUnknown*) override { return E_NOTIMPL; }
    HRESULT UnregisterExtension(IUnknown* e) override {
        if (e == frame) { frame->Release(); frame = nullptr; }
        return S_OK;
    }
};

static std::string ReadFile(const std::string& p) {
    std::string r;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return r;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) r.append(buf, n);
    fclose(f);
    return r;
}

// The plugin's settings file: <dir>/AIMP/PreventResampling.ini (APPDATA / XDG_CONFIG_HOME = <dir>)
static std::string g_ini;
static std::string IniGet(const std::string& section, const std::string& key) {
    std::string data = ReadFile(g_ini), cur;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) end = data.size();
        std::string line = data.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '[') { cur = line.substr(1, line.size() - 2); continue; }
        size_t eq = line.find('=');
        if (cur == section && eq != std::string::npos && line.substr(0, eq) == key) return line.substr(eq + 1);
    }
    return "";
}
static void IniSetIn(const std::string& section, const std::string& key, const std::string& value) {
    std::string data = ReadFile(g_ini), out, cur;
    bool done = false;
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) end = data.size();
        std::string line = data.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '[') {
            if (cur == section && !done) { out += key + "=" + value + "\r\n"; done = true; }
            cur = line.substr(1, line.size() - 2);
        } else if (cur == section && line.compare(0, key.size() + 1, key + "=") == 0) {
            line = key + "=" + value;
            done = true;
        }
        out += line + "\r\n";
    }
    if (!done && cur == section) { out += key + "=" + value + "\r\n"; done = true; }
    if (!done) out += "[" + section + "]\r\n" + key + "=" + value + "\r\n";
    FILE* f = fopen(g_ini.c_str(), "wb");
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
}
static void IniSet(const std::string& key, const std::string& value) { IniSetIn("PreventResampling", key, value); }

// Waits (and pumps the main thread) until cond is true
static bool WaitUntil(std::function<bool()> cond, int ms = 3000) {
    for (int i = 0; i < ms / 5; i++) {
        g_main.Pump();
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

// Layout of the settings page (same grid as the Discord RP plugin): every control at a fixed position,
// anchored top-left, inside 465 x 420 (96 DPI), no overlaps on a tab, texts fit (estimated: Segoe UI 9 pt
// is about 6 px per character, bold about 6.6, 16 px per line). Writes the layout for tests/render_layout.py.
static void CheckLayout(const std::string& out) {
    using Info = mockui::Registry::Info;
    std::vector<Info> all;
    for (auto& f : g_ui.reg.all) all.push_back(f());
    std::map<void*, std::string> sheets;
    std::vector<void*> order;
    for (auto& i : all)
        if (i.kind == mockui::Service::K("S")) {
            auto c = i.strs->find(AIMPUI_TABSHEET_PROPID_CAPTION);
            sheets[i.self] = c == i.strs->end() ? "?" : N(c->second);
            order.push_back(i.self);
        }
    CHECK(order.size() >= 3);
    printf("layout: %d objects, %d tabs\n", (int)all.size(), (int)order.size());
    FILE* f = fopen(out.c_str(), "wb");
    int controls = 0;
    for (auto& i : all) {
        if (!sheets.count(i.parent)) continue;
        controls++;
        const RECT& b = i.bounds;
        std::string kind = N(i.kind), text;
        for (int prop : {AIMPUI_LABEL_PROPID_TEXT, AIMPUI_CHECKBOX_PROPID_CAPTION, AIMPUI_BUTTON_PROPID_CAPTION})
            if (i.strs->count(prop)) text = N(i.strs->at(prop));
        if (kind == "Check") text = N(i.strs->count(AIMPUI_CHECKBOX_PROPID_CAPTION) ? i.strs->at(AIMPUI_CHECKBOX_PROPID_CAPTION) : tstr());
        if (kind == "Button") text = N(i.strs->count(AIMPUI_BUTTON_PROPID_CAPTION) ? i.strs->at(AIMPUI_BUTTON_PROPID_CAPTION) : tstr());
        bool bold = i.ints->count(AIMPUI_LABEL_PROPID_TEXTSTYLE) && kind == "Label";
        bool hidden = i.ints->count(AIMPUI_CONTROL_PROPID_VISIBLE) && i.ints->at(AIMPUI_CONTROL_PROPID_VISIBLE) == 0;
        std::string where = sheets[i.parent] + ": " + kind + " '" + text.substr(0, 40) + "'";
        int w = b.right - b.left, h = b.bottom - b.top;
        if (!(i.placed && i.alignment == 0 && i.anchors.left == 1 && i.anchors.top == 1)) {
            printf("layout: not anchored: %s\n", where.c_str());
            g_failures++;
        }
        if (b.left < 0 || b.top < 0 || b.right > 465 || b.bottom > 420 || w <= 0 || h <= 0) {
            printf("layout: outside 465x420: %s (%ld,%ld,%ld,%ld)\n", where.c_str(), (long)b.left, (long)b.top, (long)b.right, (long)b.bottom);
            g_failures++;
        }
        // text fits?
        bool path = !text.empty() && (text[0] == '/' || text.find(":\\") != std::string::npos);  // depends on the PC
        if (!text.empty() && !path && (kind == "Label" || kind == "Check" || kind == "Button")) {
            double cw = bold ? 6.6 : 6.0;
            int size = i.ints->count(AIMPUI_LABEL_PROPID_TEXTSIZE) ? (int)i.ints->at(AIMPUI_LABEL_PROPID_TEXTSIZE) : 0;
            if (size) cw = cw * size / 9.0;
            double avail = w - (kind == "Check" ? 20 : kind == "Button" ? 12 : 0);
            bool wrap = i.ints->count(AIMPUI_LABEL_PROPID_WORDWRAP) && i.ints->at(AIMPUI_LABEL_PROPID_WORDWRAP);
            int lines = wrap ? (int)(text.size() * cw / (avail * 0.92)) + 1 : 1;
            bool fits = wrap ? lines * 16 <= h + 2 : text.size() * cw <= avail;
            if (!fits) {
                printf("layout: text does not fit: %s (%d px wide, %d high)\n", where.c_str(), w, h);
                g_failures++;
            }
        }
        // combo box entries fit next to the drop-down button (device names come from the PC: not checked)
        if (kind == "Combo")
            for (auto& item : *i.items) {
                std::string t = N(item);
                if (!t.empty() && (t[0] == '(' || t.compare(0, 9, "Automatic") == 0) && t.size() * 6.0 > w - 24) {
                    printf("layout: combo entry does not fit: %s: '%s' (%d px wide)\n", sheets[i.parent].c_str(), t.c_str(), w);
                    g_failures++;
                }
            }
        // overlaps with other visible controls on the same tab
        for (auto& o : all) {
            if (o.self == i.self || o.parent != i.parent || &o < &i) continue;
            bool oh = o.ints->count(AIMPUI_CONTROL_PROPID_VISIBLE) && o.ints->at(AIMPUI_CONTROL_PROPID_VISIBLE) == 0;
            if (hidden || oh) continue;
            const RECT& c = o.bounds;
            if (b.left < c.right && c.left < b.right && b.top < c.bottom && c.top < b.bottom) {
                printf("layout: overlap on %s: '%s' and '%s'\n", sheets[i.parent].c_str(), text.substr(0, 30).c_str(), N(o.kind).c_str());
                g_failures++;
            }
        }
#ifndef _WIN32
        // Linux: the Voicemeeter tab is shown, but nothing on it can be changed
        if (sheets[i.parent] == "Voicemeeter" && (kind == "Check" || kind == "Combo")) {
            bool off = i.ints->count(AIMPUI_CONTROL_PROPID_ENABLED) && i.ints->at(AIMPUI_CONTROL_PROPID_ENABLED) == 0;
            if (!off) {
                printf("layout: Voicemeeter control enabled on Linux: %s\n", where.c_str());
                g_failures++;
            }
        }
#endif
        int tab = (int)(std::find(order.begin(), order.end(), i.parent) - order.begin());
        if (f) fprintf(f, "%d\t%s\t%s\t%ld\t%ld\t%d\t%d\t%d\t%d\t%s\n", tab, sheets[i.parent].c_str(), kind.c_str(),
                       (long)b.left, (long)b.top, w, h, bold ? 1 : 0, hidden ? 1 : 0, text.c_str());
    }
    if (f) fclose(f);
    CHECK(controls > 25);
}

static void StartTrack(CoreImpl* core, int rate) {
    core->player->info->rate = rate;
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_STREAM_START, 0, nullptr, nullptr);
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("Usage: MockHost <plugin> <work folder>\n"); return 2; }
    std::string dir = argv[2];
    // SHA-256 test vectors (FIPS 180-2): "abc" and the 56-byte two-block message
    CHECK(ar::Sha256::Of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(ar::Sha256::Of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    std::string pwLog = dir + "/pw.log";
    remove(pwLog.c_str());
    remove((dir + "/PreventResampling.log").c_str());
    remove((dir + "/AIMP.ini").c_str());
    remove((dir + "/PreventResampling.resume").c_str());
    g_ini = dir + "/AIMP/PreventResampling.ini";
    remove(g_ini.c_str());
#ifdef _WIN32
    SetEnvironmentVariableA("APPDATA", dir.c_str());
#else
    setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
#endif

#ifdef _WIN32
    HMODULE lib = LoadLibraryA(argv[1]);
    if (!lib) { printf("LoadLibrary failed: %lu\n", GetLastError()); return 1; }
    auto getHeader = (TAIMPPluginGetHeaderProc)GetProcAddress(lib, "AIMPPluginGetHeader");
#else
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
    auto getHeader = (TAIMPPluginGetHeaderProc)dlsym(lib, "AIMPPluginGetHeader");
#endif
    CHECK(getHeader != nullptr);
    if (!getHeader) return 1;

    auto* core = new CoreImpl();
    core->profile = dir + "/";
    IAIMPPlugin* plugin = nullptr;
    CHECK(Succeeded(getHeader(&plugin)) && plugin);
    printf("Plugin: %s\n", N(plugin->InfoGet(AIMP_PLUGIN_INFO_NAME)).c_str());
    CHECK(Succeeded(plugin->Initialize(core)));
    CHECK(core->disp->hook != nullptr);
    CHECK(core->frame != nullptr);

    // Settings page: name, then build it with the mock UI service and check the layout
    IAIMPOptionsDialogFrame* frame = nullptr;
    CHECK(Succeeded(core->frame->QueryInterface(IID_IAIMPOptionsDialogFrame, (void**)&frame)) && frame);
    if (frame) {
        IAIMPString* name = nullptr;
        CHECK(Succeeded(frame->GetName(&name)) && name && N(tstr(name->GetData(), name->GetLength())) == "Prevent Resampling");
        // AIMP links the plugin list entry and the settings page by name
        if (name) CHECK(N(plugin->InfoGet(AIMP_PLUGIN_INFO_NAME)) == N(tstr(name->GetData(), name->GetLength())));
        CHECK(N(plugin->InfoGet(AIMP_PLUGIN_INFO_AUTHOR)) == "Fl4sh");
        IAIMPExternalSettingsDialog* dlg = nullptr;
        CHECK(Succeeded(plugin->QueryInterface(IID_IAIMPExternalSettingsDialog, (void**)&dlg)) && dlg);
        if (dlg) { dlg->Show((HWND)0); dlg->Release(); }
        if (name) name->Release();
        CHECK(frame->CreateFrame((HWND)0) != (HWND)0);
        CheckLayout(dir + "/layout.txt");
        {
            // About tab: the changelog lists the installed version first and ends with the versions
            // before 2.0 (res/changelog-history.md)
            std::string log;
            for (auto& f : g_ui.reg.all) {
                auto i = f();
                for (auto& kv : *i.strs)
                    if (N(kv.second).find("(installed)") != std::string::npos) log = N(kv.second);
            }
            size_t installed = log.find("  (installed)"), v2 = log.find("Version 2.0.0"),
                   v1 = log.find("Version 1.0 (alpha, never released)");
            CHECK(installed != std::string::npos && v2 != std::string::npos && v1 != std::string::npos);
            CHECK(installed < v2 && v2 < v1);
            CHECK(log.find("AIMP AutoRate") != std::string::npos && log.find("Versions before 2.0") == std::string::npos);
            // every line fits the box (wrapped at 76 characters - list items and paragraphs alike)
            size_t longest = 0, cur = 0;
            for (unsigned char c : log) {
                if (c == '\n') { longest = (std::max)(longest, cur); cur = 0; }
                else if (c != '\r' && (c & 0xC0) != 0x80) cur++;  // count characters, not UTF-8 bytes
            }
            longest = (std::max)(longest, cur);
            if (longest > 76) printf("changelog: longest line %d characters\n", (int)longest);
            CHECK(longest <= 76);
        }
        frame->DestroyFrame();
        frame->Release();
    }

    // Fresh installation: the plugin must be disabled and must not touch anything
    StartTrack(core, 96000);
    WaitUntil([] { return false; }, 300);
    CHECK(Succeeded(plugin->Finalize()));
    plugin->Release();
    CHECK(IniGet("Statistics", "Tracks") == "0");
    CHECK(IniGet("PreventResampling", "Enabled") == "0");  // defaults written, plugin off
    CHECK(IniGet("PreventResampling", "RestartAimp") == "0");
    // settings of older versions in AIMP's configuration are removed on the first start
    CHECK(core->config->deleted.size() == 2 && core->config->deleted[0] == "PreventResampling");
    CHECK(ReadFile(pwLog).empty());
    CHECK(ReadFile(dir + "/PreventResampling.log").find("Plugin is disabled") != std::string::npos);
    CHECK(core->player->stops == 0 && core->shutdown->calls == 0);

    IniSet("Enabled", "1");
    plugin = nullptr;
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));

#ifdef _WIN32
    // Windows: flow without real hardware (Wine has no IPolicyConfig)
    for (int r : {96000, 44100, 48000}) {
        StartTrack(core, r);
        WaitUntil([] { return false; }, 300);
    }
    CHECK(Succeeded(plugin->Finalize()));
    CHECK(core->disp->hook == nullptr);
    CHECK(core->frame == nullptr);
    CHECK(IniGet("Statistics", "Tracks") == "3");
    plugin->Release();
    {
        std::string log = ReadFile(dir + "/PreventResampling.log");
        printf("---- PreventResampling.log ----\n%s----------------------\n", log.c_str());
        CHECK(log.find("Windows x") != std::string::npos && log.find("stopped") != std::string::npos);
        // The Windows audio API must be creatable (caught a typo in an IID)
        CHECK(log.find("MMDevice enumerator not available") == std::string::npos);
    }
    // ASIO mode without installed drivers
    IniSet("Mode", "3");
    plugin = nullptr;
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));
    StartTrack(core, 88200);
    WaitUntil([] { return false; }, 300);
    CHECK(Succeeded(plugin->Finalize()));
    CHECK(IniGet("Statistics", "Tracks") == "4");
    plugin->Release();
    CHECK(core->player->stops == core->player->plays);

    // AIMP 4 has no AIMP_PLAYER_PROPID_OUTPUT: the output is read from AIMP's settings
    IniSet("Mode", "0");
    core->player->aimp4 = true;
    core->config->v["AIMPSoundOut\\DeviceName"] = "DirectSound: Mock 4";
    plugin = nullptr;
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));
    StartTrack(core, 96000);
    CHECK(WaitUntil([&] { return ReadFile(dir + "/PreventResampling.log").find("Track 96 kHz | DirectSound |") != std::string::npos; }));
    CHECK(ReadFile(dir + "/PreventResampling.log").find("AIMP output (from AIMP's settings): DirectSound: Mock 4") != std::string::npos);
    CHECK(Succeeded(plugin->Finalize()));
    plugin->Release();
    core->player->aimp4 = false;

    // Check the restart helper's INI editing directly (UTF-8 with BOM, value present)
    typedef void(CALLBACK * RestartProc)(HWND, HINSTANCE, LPWSTR, int);
    auto restartProc = (RestartProc)GetProcAddress(lib, "RestartAimpW");
    CHECK(restartProc != nullptr);
    if (restartProc) {
        FILE* ini = fopen((dir + "/test8.ini").c_str(), "wb");
        fputs("\xEF\xBB\xBF[System]\r\nX=1\r\n[AIMPSoundOut]\r\nDeviceName=ASIO: Test\r\nDeviceFreq=48000\r\nDeviceBitDepth=3\r\n", ini);
        fclose(ini);
        std::wstring args = L"0 44100 \"" + T(dir) + L"/test8.ini\" \"AIMPSoundOut\\DeviceFreq\" -";
        restartProc(nullptr, nullptr, &args[0], 0);
        std::string after = ReadFile(dir + "/test8.ini");
        CHECK(after.find("DeviceFreq=44100\r\n") != std::string::npos);
        CHECK(after.find("DeviceBitDepth=3") != std::string::npos && after.compare(0, 3, "\xEF\xBB\xBF") == 0);
        // Anything but a plain number is not written into AIMP.ini
        args = L"0 \"44100\r\n[X]\" \"" + T(dir) + L"/test8.ini\" \"AIMPSoundOut\\DeviceFreq\" -";
        restartProc(nullptr, nullptr, &args[0], 0);
        CHECK(ReadFile(dir + "/test8.ini") == after);
    }

    // AIMP outputs at a fixed 48 kHz (AIMP.ini in UTF-16), WASAPI exclusive, track at 96 kHz.
    {
        std::wstring w = L"\xFEFF[AIMPSoundOut]\r\nDeviceName=WASAPI Exclusive (Push): Windows Default\r\nDeviceFreq=48000\r\n";
        FILE* ini = fopen((dir + "/AIMP.ini").c_str(), "wb");
        fwrite(w.data(), sizeof(wchar_t), w.size(), ini);
        fclose(ini);
    }
    IniSet("Mode", "2");
    int stops0 = core->player->stops, plays0 = core->player->plays;
    //  a) restart option off (default): nothing is changed, playback is not interrupted
    plugin = nullptr;
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));
    StartTrack(core, 96000);
    CHECK(WaitUntil([&] { return ReadFile(dir + "/PreventResampling.log").find("restart option off") != std::string::npos; }));
    WaitUntil([] { return false; }, 300);
    CHECK(core->shutdown->calls == 0 && core->player->stops == stops0);
    CHECK(Succeeded(plugin->Finalize()));
    plugin->Release();
    //  b) restart option on -> stop playback, write the resume file, start the helper, close AIMP
    //     (do not keep playing). The helper (rundll32) waits for this process to end, writes 96000
    //     to AIMP.ini and starts MockHost.exe without arguments (prints usage only) - the CI script
    //     checks that afterwards.
    IniSet("RestartAimp", "1");
    plugin = nullptr;
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));
    StartTrack(core, 96000);
    CHECK(WaitUntil([&] { return core->shutdown->calls == 1; }));
    CHECK(core->player->stops == stops0 + 1 && core->player->plays == plays0);
    CHECK(ReadFile(dir + "/PreventResampling.resume").find("96000") != std::string::npos);
    CHECK(Succeeded(plugin->Finalize()));
    plugin->Release();
    // Closed by the plugin: the rates stay, the originals are handed to the next AIMP process
    CHECK(ReadFile(dir + "/PreventResampling.log").find("Restart: kept the current rates") != std::string::npos);
    CHECK(IniGet("Session", "Saved") != "");
    //  c) the "restarted" AIMP takes the session over on its first track and removes it from the file
    plugin = nullptr;
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));
    StartTrack(core, 96000);
    CHECK(WaitUntil([&] { return ReadFile(dir + "/PreventResampling.log").find("from before the restart") != std::string::npos; }));
    CHECK(Succeeded(plugin->Finalize()));
    plugin->Release();
    CHECK(IniGet("Session", "Saved") == "");
    g_ui.reg.Clear();  // before unloading: the controls hold the plugin's event handlers
    FreeLibrary(lib);
#else
    // AIMP for Linux resamples every track to its own rate (AIMPSoundOut\DeviceFreq in AIMP.ini):
    // PipeWire must follow that rate, not the track's, or it resamples a second time
    auto setAimpRate = [&](int rate) {
        FILE* ini = fopen((dir + "/AIMP.ini").c_str(), "wb");
        fprintf(ini, "\xEF\xBB\xBF[System]\nVersion=6000\n[AIMPSoundOut]\nDeviceName=ALSA: default\nDeviceFreq=%d\n", rate);
        fclose(ini);
    };
    // 0) AIMP's rate unknown (no AIMP.ini yet) -> nothing is changed
    StartTrack(core, 96000);
    CHECK(WaitUntil([&] { return ReadFile(dir + "/PreventResampling.log").find("AIMP's output rate unknown - nothing changed") != std::string::npos; }));
    CHECK(ReadFile(pwLog).find("clock.force-rate") == std::string::npos);
    // 1) AIMP plays at 96 kHz -> PipeWire must be forced to 96000, without stopping playback
    setAimpRate(96000);
    StartTrack(core, 96000);
    CHECK(WaitUntil([&] { return ReadFile(pwLog).find("clock.force-rate 96000") != std::string::npos; }));
    // 2) same rate again -> no further call
    size_t before = ReadFile(pwLog).size();
    StartTrack(core, 96000);
    WaitUntil([] { return false; }, 400);
    CHECK(ReadFile(pwLog).size() == before);
    // 3) 44.1 kHz
    setAimpRate(44100);
    StartTrack(core, 44100);
    CHECK(WaitUntil([&] { return ReadFile(pwLog).find("clock.force-rate 44100") != std::string::npos; }));
    // 4) quick succession: only the last track counts in the end
    setAimpRate(88200);
    StartTrack(core, 48000);
    StartTrack(core, 88200);
    CHECK(WaitUntil([&] { return ReadFile(pwLog).find("clock.force-rate 88200") != std::string::npos; }));
    CHECK(core->player->stops == 0 && core->player->plays == 0);
    // 4b) AIMP loads a track without playing it (e.g. restored paused at start-up): nothing may be
    //     switched or started until the user presses play
    core->player->state = AIMP_PLAYER_STATE_PAUSED;
    setAimpRate(176400);
    StartTrack(core, 176400);
    WaitUntil([] { return false; }, 400);
    CHECK(ReadFile(pwLog).find("clock.force-rate 176400") == std::string::npos);
    CHECK(core->player->plays == 0);
    core->player->state = AIMP_PLAYER_STATE_PLAYING;
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_PLAYER_STATE, AIMP_PLAYER_STATE_PLAYING, nullptr, nullptr);
    CHECK(WaitUntil([&] { return ReadFile(pwLog).find("clock.force-rate 176400") != std::string::npos; }));
    CHECK(core->player->plays == 0);

    // 5) AIMP outputs at a fixed 48 kHz, the track has 96 kHz: AIMP resamples to 48 kHz anyway, so
    //    PipeWire is set to 48 kHz (no second resampling), no restart on Linux, no interruption
    setAimpRate(48000);
    StartTrack(core, 96000);
    CHECK(WaitUntil([&] { return ReadFile(dir + "/PreventResampling.log").find("AIMP rate: 48 kHz") != std::string::npos; }));
    CHECK(WaitUntil([&] { return ReadFile(pwLog).find("clock.force-rate 48000") != std::string::npos; }));
    CHECK(ReadFile(dir + "/PreventResampling.log").find("PipeWire follows AIMP") != std::string::npos);
    // 6) same rate again -> nothing to do
    StartTrack(core, 96000);
    WaitUntil([] { return false; }, 400);
    CHECK(core->player->stops == 0 && core->shutdown->calls == 0);

    // Finalize: force-rate is reset, statistics are saved
    CHECK(Succeeded(plugin->Finalize()));
    CHECK(ReadFile(pwLog).find("clock.force-rate 0") != std::string::npos);
    CHECK(core->disp->hook == nullptr);
    CHECK(core->frame == nullptr);
    CHECK(IniGet("Statistics", "Tracks") == "9");
    CHECK(IniGet("Statistics", "Rates").find("96000=5") != std::string::npos);
    plugin->Release();

    std::string log = ReadFile(dir + "/PreventResampling.log");
    printf("---- PreventResampling.log ----\n%s----------------------\n", log.c_str());
    printf("---- pw-metadata calls ----\n%s-----------------------------\n", ReadFile(pwLog).c_str());
    CHECK(log.find("started") != std::string::npos && log.find("stopped") != std::string::npos);

    // Second run: statistics are loaded from the configuration; playback after a restart by the
    // plugin (resume file) continues after AIMP_MSG_EVENT_LOADED
    {
        FILE* rf = fopen((dir + "/PreventResampling.resume").c_str(), "wb");
        fprintf(rf, "/music/resume.flac\n12500\n96000\n%lld\n", (long long)time(nullptr));
        fclose(rf);
    }
    // ... and an update is found on start-up: downloaded, SHA-256 checked, opened in AIMP
    const std::string pkg = "PK-fake-aimppack-content";
    const std::string pkgUrl = "https://github.com/RainBowFl4sh/AIMP-No-Resmapling/releases/download/v9.9.9/PreventResampling-9.9.9.aimppack";
    core->http->pages["https://api.github.com/repos/RainBowFl4sh/AIMP-No-Resmapling/releases/latest"] =
        "{\"tag_name\": \"v9.9.9\", \"body\": \"Fixes \\u00e4\\r\\n- one\", \"assets\": [{\"name\": "
        "\"PreventResampling-9.9.9.aimppack\", \"browser_download_url\": \"" + pkgUrl + "\", "
        "\"digest\": \"sha256:" + ar::Sha256::Of(pkg) + "\"}]}";
    core->http->pages[pkgUrl] = pkg;
    IniSet("UpdateInterval", "0");
    IniSet("UpdateAutoInstall", "1");
    int playsBefore = core->player->plays;
    core->player->seekFailures = 1;  // seek right after Play fails, it is repeated at the stream start
    plugin = nullptr;
    getHeader(&plugin);
    CHECK(Succeeded(plugin->Initialize(core)));
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_LOADED, 0, nullptr, nullptr);
    CHECK(WaitUntil([&] { return ReadFile(dir + "/AIMP/PreventResampling-9.9.9.aimppack") == pkg; }));
    CHECK(WaitUntil([&] { return ReadFile(dir + "/PreventResampling.log").find("SHA-256 OK") != std::string::npos; }));
    CHECK(IniGet("Updates", "AutoOpened") == "9.9.9" && IniGet("Updates", "LatestVersion") == "9.9.9");
    CHECK(IniGet("Updates", "LastCheckFailed") == "0");
    CHECK(core->http->requested.size() == 2);
    // AIMP installs the package (the plugin file on disk changes) -> the plugin restarts AIMP to load it
    // and writes the resume file for the track that is playing
    {
        struct stat st;
        CHECK(stat(argv[1], &st) == 0);
        struct timeval tv[2] = {{(time_t)(st.st_mtime + 60), 0}, {(time_t)(st.st_mtime + 60), 0}};
        CHECK(utimes(argv[1], tv) == 0);
        CHECK(WaitUntil([&] { return core->shutdown->restarts == 1; }, 8000));
        CHECK(ReadFile(dir + "/PreventResampling.resume").find("track") != std::string::npos);
        CHECK(ReadFile(dir + "/PreventResampling.log").find("Update installed - restarting AIMP") != std::string::npos);
        remove((dir + "/PreventResampling.resume").c_str());
    }
    CHECK(core->player->plays == playsBefore + 1);
    CHECK(core->player->seekPos < 0);  // not yet: the track is still opening
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_STREAM_START, 0, nullptr, nullptr);
    CHECK(core->player->seekPos > 12.4 && core->player->seekPos < 12.6);  // continued at 12.5 s
    CHECK(ReadFile(dir + "/PreventResampling.resume").empty());  // the file is removed after reading
    setAimpRate(192000);
    StartTrack(core, 192000);
    CHECK(WaitUntil([&] { return ReadFile(pwLog).find("clock.force-rate 192000") != std::string::npos; }));
    setAimpRate(176400);
    // A track that AIMP restored at start-up plays without any stream-start event: pressing play
    // must still switch to its rate. Pausing and resuming the same track again does nothing.
    auto count = [&](const std::string& what) {
        std::string t = ReadFile(pwLog);
        size_t n = 0;
        for (size_t p = t.find(what); p != std::string::npos; p = t.find(what, p + 1)) n++;
        return n;
    };
    size_t before176 = count("clock.force-rate 176400");
    core->player->info->rate = 176400;  // another file, loaded without AIMP_MSG_EVENT_STREAM_START
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_PLAYER_STATE, AIMP_PLAYER_STATE_PLAYING, nullptr, nullptr);
    CHECK(WaitUntil([&] { return count("clock.force-rate 176400") == before176 + 1; }));
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_PLAYER_STATE, AIMP_PLAYER_STATE_PAUSED, nullptr, nullptr);
    core->disp->hook->CoreMessage(AIMP_MSG_EVENT_PLAYER_STATE, AIMP_PLAYER_STATE_PLAYING, nullptr, nullptr);
    WaitUntil([] { return false; }, 400);
    CHECK(count("clock.force-rate 176400") == before176 + 1);
    CHECK(Succeeded(plugin->Finalize()));
    CHECK(IniGet("Statistics", "Tracks") == "12");
    plugin->Release();

    // Further runs: a failed update check (GitHub not reachable) is marked as failed (not shown as
    // "latest version") and repeated at the next start after an hour, although the weekly interval
    // has not passed - not at a start right after it (e.g. a restart by the plugin)
    core->http->pages.clear();
    for (int run = 0; run < 3; run++) {
        IniSet("UpdateInterval", run == 0 ? "0" : "2");
        if (run == 2) IniSetIn("Updates", "LastCheck", std::to_string((long long)time(nullptr) - 3700));
        size_t requests = core->http->requested.size();
        plugin = nullptr;
        getHeader(&plugin);
        CHECK(Succeeded(plugin->Initialize(core)));
        core->disp->hook->CoreMessage(AIMP_MSG_EVENT_LOADED, 0, nullptr, nullptr);
        if (run == 1) {
            WaitUntil([] { return false; }, 300);
            CHECK(core->http->requested.size() == requests);
        } else {
            CHECK(WaitUntil([&] { return core->http->requested.size() == requests + 1 && IniGet("Updates", "LastCheckFailed") == "1"; }));
        }
        CHECK(Succeeded(plugin->Finalize()));
        plugin->Release();
    }
    CHECK(ReadFile(dir + "/PreventResampling.log").find("Update: Update check failed") != std::string::npos);

    // A release whose package is not in this repository's releases, or whose name is not a plain file
    // name, is shown but never downloaded
    core->http->pages["https://api.github.com/repos/RainBowFl4sh/AIMP-No-Resmapling/releases/latest"] =
        "{\"tag_name\": \"v9.9.10\", \"body\": \"\", \"assets\": ["
        "{\"name\": \"PreventResampling-9.9.10.aimppack\", \"browser_download_url\": \"https://example.invalid/pkg\", \"digest\": \"sha256:" + ar::Sha256::Of(pkg) + "\"},"
        "{\"name\": \"..\\\\x.aimppack\", \"browser_download_url\": \"https://github.com/RainBowFl4sh/AIMP-No-Resmapling/releases/download/v9.9.10/x.aimppack\", \"digest\": \"sha256:" + ar::Sha256::Of(pkg) + "\"}]}";
    IniSet("UpdateInterval", "0");
    {
        size_t requests = core->http->requested.size();
        plugin = nullptr;
        getHeader(&plugin);
        CHECK(Succeeded(plugin->Initialize(core)));
        core->disp->hook->CoreMessage(AIMP_MSG_EVENT_LOADED, 0, nullptr, nullptr);
        CHECK(WaitUntil([&] { return IniGet("Updates", "LatestVersion") == "9.9.10"; }));
        WaitUntil([] { return false; }, 300);
        CHECK(core->http->requested.size() == requests + 1);  // only the release information
        CHECK(IniGet("Updates", "AutoOpened") == "9.9.9");
        CHECK(Succeeded(plugin->Finalize()));
        plugin->Release();
    }

    g_ui.reg.Clear();
    dlclose(lib);
#endif
    printf(g_failures ? "%d check(s) failed\n" : "All checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
