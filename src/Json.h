// Minimal JSON reader for the GitHub release API (objects, arrays, strings, numbers, literals).
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ar {

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    std::string str;  // String (UTF-8) and the raw text of Number / Bool
    std::vector<Json> items;
    std::map<std::string, Json> fields;

    const Json& operator[](const std::string& key) const {
        static const Json none;
        auto it = fields.find(key);
        return it == fields.end() ? none : it->second;
    }
    const std::string& Str() const { return str; }

    // Returns false on invalid input
    static bool Parse(const std::string& text, Json& out) {
        size_t p = 0;
        if (!Value(text, p, out, 0)) return false;
        Ws(text, p);
        return p == text.size();
    }

private:
    static void Ws(const std::string& s, size_t& p) {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) p++;
    }
    static void Utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
    }
    static bool Hex4(const std::string& s, size_t p, unsigned& v) {
        if (p + 4 > s.size()) return false;
        v = 0;
        for (size_t i = p; i < p + 4; i++) {
            char c = s[i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return false;
        }
        return true;
    }
    static bool ParseStr(const std::string& s, size_t& p, std::string& o) {
        if (p >= s.size() || s[p] != '"') return false;
        p++;
        while (p < s.size()) {
            char c = s[p++];
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (p >= s.size()) return false;
            char e = s[p++];
            switch (e) {
                case 'n': o += '\n'; break;
                case 't': o += '\t'; break;
                case 'r': o += '\r'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'u': {
                    unsigned cp;
                    if (!Hex4(s, p, cp)) return false;
                    p += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= s.size() && s[p] == '\\' && s[p + 1] == 'u') {
                        unsigned lo;
                        if (Hex4(s, p + 2, lo) && lo >= 0xDC00 && lo < 0xE000) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p += 6;
                        }
                    }
                    Utf8(o, cp);
                    break;
                }
                default: o += e;  // \" \\ \/
            }
        }
        return false;
    }
    static bool Value(const std::string& s, size_t& p, Json& v, int depth) {
        if (depth > 64) return false;
        Ws(s, p);
        if (p >= s.size()) return false;
        char c = s[p];
        if (c == '{') {
            v.type = Object;
            p++;
            Ws(s, p);
            if (p < s.size() && s[p] == '}') { p++; return true; }
            for (;;) {
                std::string key;
                Ws(s, p);
                if (!ParseStr(s, p, key)) return false;
                Ws(s, p);
                if (p >= s.size() || s[p++] != ':') return false;
                Json child;
                if (!Value(s, p, child, depth + 1)) return false;
                v.fields[key] = std::move(child);
                Ws(s, p);
                if (p >= s.size()) return false;
                if (s[p] == ',') { p++; continue; }
                if (s[p] == '}') { p++; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.type = Array;
            p++;
            Ws(s, p);
            if (p < s.size() && s[p] == ']') { p++; return true; }
            for (;;) {
                Json child;
                if (!Value(s, p, child, depth + 1)) return false;
                v.items.push_back(std::move(child));
                Ws(s, p);
                if (p >= s.size()) return false;
                if (s[p] == ',') { p++; continue; }
                if (s[p] == ']') { p++; return true; }
                return false;
            }
        }
        if (c == '"') { v.type = String; return ParseStr(s, p, v.str); }
        size_t start = p;
        while (p < s.size() && std::string(",}] \t\r\n").find(s[p]) == std::string::npos) p++;
        v.str = s.substr(start, p - start);
        if (v.str == "null") { v.type = Null; return true; }
        if (v.str == "true" || v.str == "false") { v.type = Bool; return true; }
        v.type = Number;
        return !v.str.empty();
    }
};

}  // namespace ar
