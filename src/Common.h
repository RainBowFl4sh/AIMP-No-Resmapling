// Platform-independent basics: strings, logging, time.
#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "apiTypes.h"

// String literals in AIMP's format: UTF-16 on Windows, UTF-8 on Linux
#ifdef _WIN32
#define AR_T2(x) L##x
#define AR_NL L"\r\n"
#define AR_SEP "\\"
#else
#define AR_T2(x) x
#define AR_NL "\n"
#define AR_SEP "/"
#endif
#define AR_T(x) AR_T2(x)  // two-level so that macros like AR_VERSION are expanded first

#define AR_PLUGIN_NAME "Prevent Resampling"
#define AR_AUTHOR "Fl4sh"
#ifndef AR_VERSION
#define AR_VERSION "2.5.4"
#endif

namespace ar {

typedef std::basic_string<TChar> tstring;

inline tstring Num(long long v) {
#ifdef _WIN32
    return std::to_wstring(v);
#else
    return std::to_string(v);
#endif
}

// 44100 -> "44.1 kHz", 192000 -> "192 kHz"
inline tstring Khz(uint32_t hz) {
    tstring s = Num(hz / 1000);
    uint32_t frac = hz % 1000;
    if (frac) {
        tstring f = Num(frac + 1000).substr(1);  // three digits with leading zeros
        while (!f.empty() && f.back() == AR_T('0')) f.pop_back();
        s += AR_T(".") + f;
    }
    return s + AR_T(" kHz");
}

inline TChar LowerChar(TChar c) {
#ifdef _WIN32
    return (TChar)towlower(c);
#else
    return (c >= 'A' && c <= 'Z') ? (TChar)(c - 'A' + 'a') : c;  // UTF-8: ASCII only
#endif
}

inline tstring Lower(tstring s) {
    for (auto& c : s) c = LowerChar(c);
    return s;
}

// Case-insensitive substring search
inline bool ContainsI(const tstring& hay, const tstring& needle) {
    if (needle.empty()) return false;
    return Lower(hay).find(Lower(needle)) != tstring::npos;
}

inline tstring Trim(const tstring& s) {
    size_t a = s.find_first_not_of(AR_T(" \t\r\n"));
    if (a == tstring::npos) return tstring();
    size_t b = s.find_last_not_of(AR_T(" \t\r\n"));
    return s.substr(a, b - a + 1);
}

inline std::vector<tstring> Split(const tstring& s, TChar sep) {
    std::vector<tstring> r;
    tstring cur;
    for (TChar c : s) {
        if (c == sep) {
            cur = Trim(cur);
            if (!cur.empty()) r.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    cur = Trim(cur);
    if (!cur.empty()) r.push_back(cur);
    return r;
}

inline void SleepMs(int ms) {
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

inline int64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32
inline std::string ToUtf8(const tstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &r[0], n, nullptr, nullptr);
    return r;
}
inline tstring FromAnsi(const char* s) {
    int n = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (n <= 1) return tstring();
    tstring r(n - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, &r[0], n);
    return r;
}
#else
inline std::string ToUtf8(const tstring& s) { return s; }
#endif

// Timestamp "hh:mm:ss"
inline tstring ClockString() {
    time_t t = time(nullptr);
    struct tm lt;
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
    tstring r;
    for (const char* p = buf; *p; ++p) r += (TChar)*p;
    return r;
}

// ---------------------------------------------------------------------------
// Logging to a file in the AIMP profile folder; the most recent lines are also kept
// in memory and shown on the settings page.
class Logger {
    std::mutex m_;
    tstring path_;
    std::vector<tstring> recent_;

public:
    static Logger& Get() {
        static Logger l;
        return l;
    }

    void SetFile(const tstring& path) {
        std::lock_guard<std::mutex> g(m_);
        path_ = path;
#ifdef _WIN32
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExW(path_.c_str(), GetFileExInfoStandard, &a) && a.nFileSizeLow > 1024 * 1024)
            DeleteFileW(path_.c_str());
#else
        FILE* f = fopen(path_.c_str(), "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fclose(f);
            if (sz > 1024 * 1024) remove(path_.c_str());
        }
#endif
    }

    void Write(const tstring& msg) {
        std::lock_guard<std::mutex> g(m_);
        tstring line = ClockString() + AR_T("  ") + msg;
        recent_.push_back(line);
        if (recent_.size() > 12) recent_.erase(recent_.begin());
        if (path_.empty()) return;
#ifdef _WIN32
        FILE* f = _wfopen(path_.c_str(), L"ab");
#else
        FILE* f = fopen(path_.c_str(), "ab");
#endif
        if (!f) return;
        std::string u = ToUtf8(line);
        u += "\n";
        fwrite(u.data(), 1, u.size(), f);
        fclose(f);
    }

    tstring Path() {
        std::lock_guard<std::mutex> g(m_);
        return path_;
    }

    std::vector<tstring> Recent() {
        std::lock_guard<std::mutex> g(m_);
        return recent_;
    }
};

inline void Log(const tstring& msg) { Logger::Get().Write(msg); }

}  // namespace ar
