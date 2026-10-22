// Languages of the settings page, the statistics and the update messages: English, German, Russian and
// Ukrainian. The English text is the key (UTF-8); the translations are in LangTable.h. A text without a
// translation is shown in English. The log file stays English.
#pragma once
#include <initializer_list>
#include <map>
#include <string>

#include "AimpUtil.h"
#include "Ini.h"
#include "apiMUI.h"

namespace ar {

enum Language : int { LangEn = 0, LangDe = 1, LangRu = 2, LangUk = 3, LangCount = 4 };

// Setting "Language" in the settings file: auto = follow AIMP's language
enum class LanguageSetting : int { Auto = 0, En = 1, De = 2, Ru = 3, Uk = 4 };

inline const char* LanguageCode(LanguageSetting s) {
    static const char* codes[] = {"auto", "en", "de", "ru", "uk"};
    return codes[(int)s];
}
inline LanguageSetting LanguageFromCode(const std::string& code) {
    for (int i = 0; i <= 4; i++)
        if (code == LanguageCode((LanguageSetting)i)) return (LanguageSetting)i;
    return LanguageSetting::Auto;
}

// Name of a language in that language (shown untranslated in the drop-down list)
inline const char* LanguageName(Language l) {
    static const char* names[] = {"English", "Deutsch", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9",
                                  "\xD0\xA3\xD0\xBA\xD1\x80\xD0\xB0\xD1\x97\xD0\xBD\xD1\x81\xD1\x8C\xD0\xBA\xD0\xB0"};
    return names[(int)l];
}

inline int& CurrentLanguage() {
    static int l = LangEn;
    return l;
}

struct Translation {
    const char* text[LangCount];  // en, de, ru, uk
};

}  // namespace ar

#include "LangTable.h"

namespace ar {

// Texts that were asked for but have no translation (for the tests: logged once each)
inline std::map<std::string, bool>& MissingTranslations() {
    static std::map<std::string, bool> m;
    return m;
}

inline const char* TrUtf8(const char* en) {
    static const std::map<std::string, const Translation*> table = [] {
        std::map<std::string, const Translation*> m;
        for (const Translation& t : kTranslations) m[t.text[LangEn]] = &t;
        return m;
    }();
    int l = CurrentLanguage();
    if (l == LangEn || !en || !*en) return en ? en : "";
    auto it = table.find(en);
    if (it == table.end() || !it->second->text[l] || !*it->second->text[l]) {
        auto& missing = MissingTranslations()[en];
        if (!missing) {
            missing = true;
            Log(tstring(AR_T("Missing translation: ")) + FromUtf8(en));
        }
        return en;
    }
    return it->second->text[l];
}

inline tstring Tr(const char* en) { return FromUtf8(TrUtf8(en)); }

// Translation with placeholders %1 .. %9
inline tstring Trf(const char* en, std::initializer_list<tstring> args) {
    tstring t = Tr(en), out;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == AR_T('%') && i + 1 < t.size() && t[i + 1] >= AR_T('1') && t[i + 1] <= AR_T('9')) {
            size_t n = (size_t)(t[i + 1] - AR_T('1'));
            if (n < args.size()) out += *(args.begin() + n);
            i++;
        } else {
            out += t[i];
        }
    }
    return out;
}

// Number of characters (UTF-8 on Linux counts bytes otherwise) - for aligned columns
inline size_t TextLength(const tstring& s) {
#ifdef _WIN32
    return s.size();
#else
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) n++;
    return n;
#endif
}

// AIMP's interface language: the language file's LangId (Windows language id), otherwise its name
inline Language AimpLanguage(tstring* name = nullptr) {
    auto mui = Service<IAIMPServiceMUI>(IID_IAIMPServiceMUI);
    if (!mui) return LangEn;
    tstring n, id;
    Ptr<IAIMPString> s;
    if (Succeeded(mui->GetName(s.Out())) && s) n = FromString(s.Get());
    auto key = MakeString(AR_T("FILE\\LangId"));
    Ptr<IAIMPString> v;
    if (key && Succeeded(mui->GetValue(key.Get(), v.Out())) && v) id = Trim(FromString(v.Get()));
    if (name) *name = n;
    long lid = 0;
    for (TChar c : id) {
        if (c < AR_T('0') || c > AR_T('9')) { lid = 0; break; }
        lid = lid * 10 + (c - AR_T('0'));
    }
    switch (lid & 0x3FF) {  // primary language
        case 0x07: return LangDe;
        case 0x19: return LangRu;
        case 0x22: return LangUk;
        case 0x09: return LangEn;
    }
    std::string u = ToUtf8(Lower(n));
    auto has = [&](const char* part) { return u.find(part) != std::string::npos; };
    if (has("deutsch") || has("german")) return LangDe;
    if (has("ukrain") || has("\xD1\x83\xD0\xBA\xD1\x80\xD0\xB0\xD1\x97\xD0\xBD") ||
        has("\xD0\xA3\xD0\xBA\xD1\x80\xD0\xB0\xD1\x97\xD0\xBD"))  // "україн", "Україн"
        return LangUk;
    if (has("russian") || has("\xD1\x80\xD1\x83\xD1\x81\xD1\x81") || has("\xD0\xA0\xD1\x83\xD1\x81\xD1\x81"))  // "русс", "Русс"
        return LangRu;
    return LangEn;
}

inline Language ResolveLanguage(LanguageSetting s) {
    return s == LanguageSetting::Auto ? AimpLanguage() : (Language)((int)s - 1);
}

}  // namespace ar
