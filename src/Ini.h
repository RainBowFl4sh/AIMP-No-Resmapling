// The plugin's own settings file, next to the other AIMP plugin settings:
//   Windows: %APPDATA%\AIMP\PreventResampling.ini
//   Linux:   $XDG_CONFIG_HOME/AIMP/PreventResampling.ini (usually ~/.config/AIMP)
// UTF-8 text, sections [PreventResampling], [Statistics] and [Updates]. Read and written on AIMP's
// main thread; the update checker thread only calls Save() through the main thread as well.
#pragma once
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include <cstdlib>
#include <cstring>
#include <utility>

#include "Common.h"

namespace ar {

#ifdef _WIN32
inline tstring FromUtf8(const std::string& s) {
    if (s.empty()) return tstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    tstring r(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &r[0], n);
    return r;
}
#else
inline tstring FromUtf8(const std::string& s) { return s; }
#endif

inline tstring EnvVar(const char* name) {
#ifdef _WIN32
    // GetEnvironmentVariable instead of getenv: sees changes made by the host process at run time
    std::wstring n(name, name + strlen(name));
    wchar_t buf[1024];
    DWORD len = GetEnvironmentVariableW(n.c_str(), buf, 1024);
    return (len > 0 && len < 1024) ? tstring(buf, len) : tstring();
#else
    const char* v = getenv(name);
    return v ? tstring(v) : tstring();
#endif
}

inline void MakeDir(const tstring& dir) {
#ifdef _WIN32
    CreateDirectoryW(dir.c_str(), nullptr);
#else
    mkdir(dir.c_str(), 0755);
#endif
}

// Folder of the settings file (created if missing)
inline tstring SettingsDir() {
#ifdef _WIN32
    tstring base = EnvVar("APPDATA");
    if (base.empty()) return tstring();
    tstring dir = base + L"\\AIMP";
#else
    tstring base = EnvVar("XDG_CONFIG_HOME");
    if (base.empty()) {
        tstring home = EnvVar("HOME");
        if (home.empty()) return tstring();
        base = home + "/.config";
        MakeDir(base);
    }
    tstring dir = base + "/AIMP";
#endif
    MakeDir(dir);
    return dir;
}

class Ini {
    typedef std::vector<std::pair<tstring, tstring>> Entries;
    std::vector<std::pair<tstring, Entries>> sections_;
    tstring path_;
    bool existed_ = false;

    Entries* Find(const tstring& section, bool create) {
        for (auto& s : sections_)
            if (EqualsI(s.first, section)) return &s.second;
        if (!create) return nullptr;
        sections_.push_back({section, Entries()});
        return &sections_.back().second;
    }

public:
    static Ini& Get() {
        static Ini i;
        return i;
    }

    const tstring& Path() const { return path_; }
    bool Existed() const { return existed_; }  // false = first start with this file

    void Open(const tstring& path) {
        path_ = path;
        sections_.clear();
        existed_ = false;
#ifdef _WIN32
        FILE* f = _wfopen(path.c_str(), L"rb");
#else
        FILE* f = fopen(path.c_str(), "rb");
#endif
        if (!f) return;
        existed_ = true;
        std::string data;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
        fclose(f);
        if (data.compare(0, 3, "\xEF\xBB\xBF") == 0) data.erase(0, 3);
        Entries* cur = nullptr;
        size_t pos = 0;
        while (pos <= data.size()) {
            size_t end = data.find('\n', pos);
            if (end == std::string::npos) end = data.size();
            tstring line = Trim(FromUtf8(data.substr(pos, end - pos)));
            pos = end + 1;
            if (line.empty() || line[0] == AR_T(';') || line[0] == AR_T('#')) continue;
            if (line[0] == AR_T('[') && line.back() == AR_T(']')) {
                cur = Find(Trim(line.substr(1, line.size() - 2)), true);
                continue;
            }
            size_t eq = line.find(AR_T('='));
            if (!cur || eq == tstring::npos) continue;
            cur->push_back({Trim(line.substr(0, eq)), Trim(line.substr(eq + 1))});
        }
    }

    bool Save() {
        if (path_.empty()) return false;
        std::string out;
        for (auto& s : sections_) {
            if (!out.empty()) out += "\r\n";
            out += "[" + ToUtf8(s.first) + "]\r\n";
            for (auto& e : s.second) out += ToUtf8(e.first) + "=" + ToUtf8(e.second) + "\r\n";
        }
        tstring tmp = path_ + AR_T(".tmp");
#ifdef _WIN32
        FILE* f = _wfopen(tmp.c_str(), L"wb");
#else
        FILE* f = fopen(tmp.c_str(), "wb");
#endif
        if (!f) return false;
        bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
        ok = fclose(f) == 0 && ok;
#ifdef _WIN32
        ok = ok && MoveFileExW(tmp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING);
#else
        ok = ok && rename(tmp.c_str(), path_.c_str()) == 0;
#endif
        if (ok) existed_ = true;
        return ok;
    }

    bool Has(const tstring& section, const tstring& key) {
        Entries* e = Find(section, false);
        if (!e) return false;
        for (auto& kv : *e)
            if (EqualsI(kv.first, key)) return true;
        return false;
    }

    tstring Str(const tstring& section, const tstring& key, const tstring& def = tstring()) {
        Entries* e = Find(section, false);
        if (e)
            for (auto& kv : *e)
                if (EqualsI(kv.first, key)) return kv.second;
        return def;
    }

    long long Int(const tstring& section, const tstring& key, long long def) {
        tstring v = Str(section, key);
        if (v.empty()) return def;
        std::string a = ToUtf8(v);
        char* end = nullptr;
        long long r = strtoll(a.c_str(), &end, 10);
        return end == a.c_str() ? def : r;
    }

    bool Bool(const tstring& section, const tstring& key, bool def) { return Int(section, key, def ? 1 : 0) != 0; }

    void Set(const tstring& section, const tstring& key, const tstring& value) {
        tstring clean;
        for (TChar c : value) clean += (c == AR_T('\r') || c == AR_T('\n')) ? AR_T(' ') : c;
        Entries* e = Find(section, true);
        for (auto& kv : *e)
            if (EqualsI(kv.first, key)) { kv.second = clean; return; }
        e->push_back({key, clean});
    }
    void Set(const tstring& section, const tstring& key, long long v) { Set(section, key, Num(v)); }
    void SetBool(const tstring& section, const tstring& key, bool v) { Set(section, key, Num(v ? 1 : 0)); }

    void Clear(const tstring& section) {
        Entries* e = Find(section, false);
        if (e) e->clear();
    }
};

}  // namespace ar
