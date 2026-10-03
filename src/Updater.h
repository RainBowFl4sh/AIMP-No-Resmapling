// Update check (About tab): asks GitHub for the latest release, downloads the .aimppack, checks its
// SHA-256 and opens it in AIMP, which installs it. Uses AIMP's own HTTP client (proxy settings of
// AIMP apply, works on Windows and Linux). Everything runs on AIMP's main thread: requests are
// asynchronous and AIMP reports their completion on the main thread.
#pragma once
#ifndef _WIN32
#include <spawn.h>
#endif
#include <ctime>
#include <functional>
#include <memory>

#include "AimpUtil.h"
#include "Ini.h"
#include "Json.h"
#include "Sha256.h"
#include "apiInternet.h"
#ifdef _WIN32
#include "win/Restart.h"
#endif

#ifndef _WIN32
extern char** environ;
#endif

#define AR_REPO "RainBowFl4sh/AIMP-No-Resmapling"
#define AR_URL_GITHUB "https://github.com/" AR_REPO
#define AR_URL_RELEASES AR_URL_GITHUB "/releases"
#define AR_URL_ISSUES AR_URL_GITHUB "/issues"
#define AR_URL_AUTHOR "https://github.com/RainBowFl4sh"
#define AR_URL_API_LATEST "https://api.github.com/repos/" AR_REPO "/releases/latest"

namespace ar {

// "v2.10.1" -> {2, 10, 1}; compares numerically
inline int CompareVersions(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& v) {
        std::vector<long> r;
        long cur = -1;
        for (char c : v) {
            if (c >= '0' && c <= '9') cur = (cur < 0 ? 0 : cur * 10) + (c - '0');
            else if (cur >= 0) { r.push_back(cur); cur = -1; if (c != '.') break; }
        }
        if (cur >= 0) r.push_back(cur);
        return r;
    };
    auto x = parts(a), y = parts(b);
    for (size_t i = 0; i < (std::max)(x.size(), y.size()); i++) {
        long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

class HttpEvents : public ComObject<IAIMPHTTPClientEvents> {
    std::function<void(bool, const tstring&)> done_;

protected:
    bool Supports(REFIID riid) override { return EqualGUID(riid, IID_IAIMPHTTPClientEvents); }

public:
    explicit HttpEvents(std::function<void(bool, const tstring&)> f) : done_(std::move(f)) {}
    void WINAPI OnAccept(IAIMPString*, const INT64, BOOL* allow) override { if (allow) *allow = 1; }
    void WINAPI OnProgress(const INT64, const INT64) override {}
    void WINAPI OnComplete(IAIMPErrorInfo* err, BOOL canceled) override {
        tstring msg;
        if (err) {
            Ptr<IAIMPString> s;
            if (Succeeded(err->GetInfoFormatted(s.Out())) && s) msg = FromString(s.Get());
            if (msg.empty()) msg = AR_T("network error");
        }
        auto f = std::move(done_);
        done_ = nullptr;
        if (f) f(!err && !canceled, canceled ? tstring(AR_T("cancelled")) : msg);
    }
};

class Updater {
public:
    enum class State { Idle, Checking, UpToDate, Available, Downloading, Opened, Error };

    State state = State::Idle;
    tstring message;         // status line on the About tab
    std::string latest;      // version of the latest release ("2.5.0")
    std::string notes;       // its release notes (UTF-8)
    std::function<void()> onChanged;
    std::function<void()> onPackageOpened;  // AIMP shows its install dialog now (watch for the new files)
    tstring downloadDir;     // where the package is stored

private:
    std::string assetUrl_, assetName_, digest_, checksumUrl_;
    Ptr<IAIMPMemoryStream> stream_;
    TTaskHandle task_ = 0;
    bool busy_ = false;
    bool autoInstall_ = false;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // false after Cancel()

    static tstring T(const std::string& utf8) { return FromUtf8(utf8); }

    void Set(State s, const tstring& msg) {
        state = s;
        message = msg;
        if (onChanged) onChanged();
    }

    // Asynchronous GET; cb(ok, data, error) is called on the main thread
    bool Get(const std::string& url, std::function<void(bool, const std::string&, const tstring&)> cb) {
        auto http = Service<IAIMPServiceHTTPClient>(IID_IAIMPServiceHTTPClient);
        auto u = MakeString(T(url));
        stream_.Reset();
        if (Core()) Core()->CreateObject(IID_IAIMPMemoryStream, stream_.OutV());
        if (!http || !u || !stream_) return false;
        Ptr<IAIMPHTTPClientEvents> ev;
        std::shared_ptr<bool> alive = alive_;
        HttpEvents* h = new HttpEvents([this, cb, alive](bool ok, const tstring& err) {
            if (!*alive) return;  // plugin is shutting down
            task_ = 0;
            std::string data;
            if (stream_) {
                INT64 n = stream_->GetSize();
                const char* p = (const char*)stream_->GetData();
                if (p && n > 0) data.assign(p, (size_t)n);
            }
            stream_.Reset();
            cb(ok, data, err);
        });
        h->AddRef();
        *ev.Out() = h;
        TTaskHandle t = 0;
        if (Failed(http->Get(u.Get(), AIMP_SERVICE_HTTPCLIENT_FLAGS_PRIORITY_NORMAL, stream_.Get(), ev.Get(), nullptr, &t)))
            return false;
        task_ = t;
        return true;
    }

    void Fail(const tstring& msg0) {
        busy_ = false;
        // AIMP's error texts contain line breaks ("Code: 403 (...)\nMessage: Forbidden\n")
        tstring msg;
        for (TChar c : msg0) {
            if (c == AR_T('\r') || c == AR_T('\n')) {
                if (!msg.empty() && msg.back() != AR_T(' ')) msg += AR_T(' ');
            } else msg += c;
        }
        while (msg.size() > 1 && msg[msg.size() - 2] == AR_T(' ') && msg.back() == AR_T('.')) msg.erase(msg.size() - 2, 1);
        Log(AR_T("Update: ") + msg);
        Set(State::Error, msg);
    }

    // A failed check is repeated at the next start after an hour instead of after the interval (Due)
    static void SetFailed(bool failed) { Ini::Get().SetBool(AR_T("Updates"), AR_T("LastCheckFailed"), failed); }

    void OnRelease(bool ok, const std::string& data, const tstring& err, bool manual) {
        Ini& ini = Ini::Get();
        ini.Set(AR_T("Updates"), AR_T("LastCheck"), (long long)time(nullptr));
        Json j;
        if (!ok || !Json::Parse(data, j) || j.type != Json::Object) {
            SetFailed(true);
            ini.Save();
            Fail(tstring(AR_T("Update check failed")) + (err.empty() ? tstring() : AR_T(": ") + err) + AR_T("."));
            return;
        }
        if (j["tag_name"].type != Json::String) {  // e.g. {"message": "Not Found"} - no release yet
            SetFailed(true);
            ini.Save();
            Fail(AR_T("No release found on GitHub."));
            return;
        }
        latest = j["tag_name"].Str();
        if (!latest.empty() && (latest[0] == 'v' || latest[0] == 'V')) latest.erase(0, 1);
        notes = j["body"].Str();
        assetUrl_.clear(); assetName_.clear(); digest_.clear(); checksumUrl_.clear();
        for (auto& a : j["assets"].items) {
            const std::string& name = a["name"].Str();
            if (name.size() > 9 && name.compare(name.size() - 9, 9, ".aimppack") == 0) {
                assetName_ = name;
                assetUrl_ = a["browser_download_url"].Str();
                std::string d = a["digest"].Str();
                if (d.compare(0, 7, "sha256:") == 0) digest_ = d.substr(7);
            }
        }
        for (auto& a : j["assets"].items)
            if (!assetName_.empty() && a["name"].Str() == assetName_ + ".sha256") checksumUrl_ = a["browser_download_url"].Str();
        ini.Set(AR_T("Updates"), AR_T("LatestVersion"), T(latest));
        SetFailed(false);
        ini.Save();
        busy_ = false;
        if (CompareVersions(latest, AR_VERSION) <= 0) {
            Log(AR_T("Update: ") + T(latest) + AR_T(" is the latest version"));
            Set(State::UpToDate, AR_T("You have the latest version (last check: ") + LastCheckText() + AR_T(")."));
            return;
        }
        Log(AR_T("Update: version ") + T(latest) + AR_T(" is available"));
        Set(State::Available, AR_T("Version ") + T(latest) + AR_T(" is available") +
                                  (assetUrl_.empty() ? tstring(AR_T(" (no package attached yet - see All releases).")) : tstring(AR_T("."))));
        // Automatic installation: each new version is opened only once
        if (autoInstall_ && !manual && !assetUrl_.empty() && ini.Str(AR_T("Updates"), AR_T("AutoOpened")) != T(latest)) {
            ini.Set(AR_T("Updates"), AR_T("AutoOpened"), T(latest));
            ini.Save();
            Install();
        }
    }

    void Verify(const std::string& data) {
        std::string actual = Sha256::Of(data);
        std::string expected = digest_;
        for (auto& c : expected) c = (char)tolower((unsigned char)c);
        if (expected.size() != 64 || actual != expected) {
            Fail(expected.empty() ? tstring(AR_T("The package has no SHA-256 checksum - not installed."))
                                  : tstring(AR_T("Checksum mismatch - the download was discarded.")));
            return;
        }
        tstring dir = downloadDir.empty() ? SettingsDir() : downloadDir;
        tstring file = dir + AR_T(AR_SEP) + T(assetName_);
#ifdef _WIN32
        FILE* f = _wfopen(file.c_str(), L"wb");
#else
        FILE* f = fopen(file.c_str(), "wb");
#endif
        bool ok = f && fwrite(data.data(), 1, data.size(), f) == data.size();
        if (f) ok = fclose(f) == 0 && ok;
        if (!ok) { Fail(AR_T("Could not save ") + file + AR_T(".")); return; }
        Log(AR_T("Update: ") + T(assetName_) + AR_T(" downloaded, SHA-256 OK - opening it in AIMP"));
        busy_ = false;
        if (OpenInAimp(file)) {
            Set(State::Opened, AR_T("Version ") + T(latest) + AR_T(" was opened in AIMP - confirm the installation there; AIMP restarts by itself afterwards."));
            if (onPackageOpened) onPackageOpened();
        } else {
            Fail(AR_T("Could not open ") + file + AR_T(" - please open it in AIMP yourself."));
        }
    }

    static bool OpenInAimp(const tstring& file) {
#ifdef _WIN32
        std::wstring exe = win::ModulePath(nullptr), args = L"\"" + file + L"\"";
        return (INT_PTR)ShellExecuteW(nullptr, L"open", exe.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL) > 32;
#else
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n <= 0) return false;
        exe[n] = 0;
        std::string f = file;
        char* argv[] = {exe, &f[0], nullptr};
        pid_t pid;
        return posix_spawn(&pid, exe, nullptr, nullptr, argv, environ) == 0;
#endif
    }

public:
    bool Busy() const { return busy_; }

    static tstring LastCheckText() {
        long long t = Ini::Get().Int(AR_T("Updates"), AR_T("LastCheck"), 0);
        if (t <= 0) return AR_T("never");
        time_t tt = (time_t)t;
        struct tm lt;
#ifdef _WIN32
        localtime_s(&lt, &tt);
#else
        localtime_r(&tt, &lt);
#endif
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &lt);
        return T(buf);
    }

    // interval: 0 = every start, 1 = daily, 2 = weekly, 3 = monthly
    static bool Due(int interval) {
        static const long long secs[] = {0, 86400, 7 * 86400, 30 * 86400};
        long long last = Ini::Get().Int(AR_T("Updates"), AR_T("LastCheck"), 0);
        long long now = (long long)time(nullptr);
        // After a failed check try again at the next start - but at most once an hour, because the
        // restart option can restart AIMP on every track
        if (Ini::Get().Bool(AR_T("Updates"), AR_T("LastCheckFailed"), false) && interval > 0)
            return last <= 0 || last > now || now - last >= 3600;
        return interval <= 0 || last <= 0 || last > now || now - last >= secs[(std::min)(interval, 3)];
    }

    void Check(bool manual, bool autoInstall) {
        if (busy_) return;
        busy_ = true;
        autoInstall_ = autoInstall;
        Set(State::Checking, AR_T("Checking for updates..."));
        if (!Get(AR_URL_API_LATEST, [this, manual](bool ok, const std::string& d, const tstring& e) { OnRelease(ok, d, e, manual); }))
            Fail(AR_T("Update check not available (AIMP's HTTP client is missing)."));
    }

    void Install() {
        if (busy_ || assetUrl_.empty()) return;
        busy_ = true;
        Set(State::Downloading, AR_T("Downloading ") + T(assetName_) + AR_T("..."));
        auto download = [this] {
            if (!Get(assetUrl_, [this](bool ok, const std::string& d, const tstring& e) {
                    if (!ok || d.empty()) { Fail(AR_T("Download failed") + (e.empty() ? tstring() : AR_T(": ") + e) + AR_T(".")); return; }
                    Verify(d);
                }))
                Fail(AR_T("Download not available."));
        };
        if (!digest_.empty() || checksumUrl_.empty()) { download(); return; }
        // Older GitHub releases have no digest: use the attached <package>.sha256 file
        if (!Get(checksumUrl_, [this, download](bool ok, const std::string& d, const tstring&) {
                if (ok && d.size() >= 64) digest_ = d.substr(0, 64);
                download();
            }))
            download();
    }

    // The new version is on disk: AIMP is restarted to load it
    void Installed() { Set(State::Opened, AR_T("Update installed - restarting AIMP...")); }

    bool CanInstall() const { return state == State::Available && !assetUrl_.empty() && !busy_; }

    void Cancel() {
        auto http = Service<IAIMPServiceHTTPClient>(IID_IAIMPServiceHTTPClient);
        if (http && task_) http->Cancel(task_, AIMP_SERVICE_HTTPCLIENT_FLAGS_WAITFOR);
        task_ = 0;
        *alive_ = false;
        onChanged = nullptr;
        onPackageOpened = nullptr;
    }
};

}  // namespace ar
