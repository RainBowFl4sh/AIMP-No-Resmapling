// Native settings page in AIMP (Preferences -> Plugins -> Prevent Resampling), built with AIMP's
// GUI API so it follows the skin and works on Windows and Linux.
#pragma once
#include "AvatarPng.h"
#include "ChangelogText.h"
#include "Engine.h"
#include "apiGUI.h"
#include "apiOptions.h"
#ifdef _WIN32
#include <commdlg.h>
#endif

#ifndef MB_ICONINFORMATION  // Linux: AIMP's message dialog takes the Windows flags
#define MB_OK 0
#define MB_ICONWARNING 0x30
#define MB_ICONINFORMATION 0x40
#endif

namespace ar {

// AIMP's own message box (follows the skin); false if this AIMP version has none
inline bool ShowMessage(const tstring& text, DWORD flags = MB_OK | MB_ICONINFORMATION, HWND owner = (HWND)0) {
    auto ui = Service<IAIMPServiceUI>(IID_IAIMPServiceUI);
    Ptr<IAIMPUIMessageDialog> dlg;
    auto caption = MakeString(AR_T(AR_PLUGIN_NAME));
    auto t = MakeString(text);
    if (!ui || Failed(ui->QueryInterface(IID_IAIMPUIMessageDialog, dlg.OutV())) || !dlg || !caption || !t) return false;
    dlg->Execute(owner, caption.Get(), t.Get(), flags);
    return true;
}

// Visible text of a control: a translation key (the English text, UTF-8), a fixed text (paths) or a
// function that puts the text together. Kept with the control, so a language change can set it again.
struct Tx {
    std::function<tstring()> fn;
    Tx(const char* key) : fn([key] { return Tr(key); }) {}
    Tx(const tstring& fixed) : fn([fixed] { return fixed; }) {}
    explicit Tx(std::function<tstring()> f) : fn(std::move(f)) {}
};

class ChangeHandler : public ComObject<IAIMPUIChangeEvents> {
    std::function<void(IUnknown*)> fn_;

protected:
    bool Supports(REFIID riid) override { return EqualGUID(riid, IID_IAIMPUIChangeEvents); }

public:
    explicit ChangeHandler(std::function<void(IUnknown*)> f) : fn_(std::move(f)) {}
    void WINAPI OnChanged(IUnknown* sender) override { if (fn_) fn_(sender); }
};

class OptionsFrame : public ComObject<IAIMPOptionsDialogFrame> {
    Engine* engine_;
    Ptr<IAIMPServiceUI> ui_;
    Ptr<IAIMPUIForm> form_;
    Ptr<IAIMPUIChangeEvents> events_;
    bool loading_ = false;
    bool aimpOutChanged_ = false;  // the user picked a different AIMP output on this page
    std::vector<Ptr<IAIMPUITabSheet>> containers_;  // tab sheets

    // Controls
    Ptr<IAIMPUIPageControl> pages_;
    Ptr<IAIMPUICheckBox> cbEnabled_, cbMultiples_, cbRestore_, cbAutoTiming_;
    Ptr<IAIMPUICheckBox> cbRestart_, cbSeamless_, cbAsioForce_;
    Ptr<IAIMPUICheckBox> cbBp_, cbBpVolume_, cbBpGapless_;  // Output tab: bit-perfect mode
    Ptr<IAIMPUILabel> lbBpStatus_;
    Ptr<IAIMPUIButton> btBpRestart_;
    Ptr<IAIMPUICheckBox> cbVm_, cbVmEngine_, cbVmAllBuses_, cbVmAsio_;
    Ptr<IAIMPUISpinEdit> spSettle_;
    Ptr<IAIMPUIComboBox> coAimpOut_, coMode_, coWinDev_, coAsio_, coVmA1_;
    Ptr<IAIMPUIEdit> edExtra_;
    Ptr<IAIMPUILabel> lbVmStatus_;
    Ptr<IAIMPUIMemo> memo_;
    Ptr<IAIMPUIButton> btRefresh_, btReset_, btTest_;
    tstring lastStats_;
    // Advanced tab: language and settings file
    Ptr<IAIMPUIComboBox> coLang_;
    Ptr<IAIMPUIButton> btExport_, btImport_;
    // About tab
    Ptr<IAIMPUICheckBox> cbUpdate_, cbAutoInstall_;
    Ptr<IAIMPUIComboBox> coInterval_;
    Ptr<IAIMPUIButton> btCheck_, btInstall_;
    Ptr<IAIMPUILabel> lbUpdate_, lnNotes_;
    Ptr<IAIMPUIMemo> memoChangelog_;
    tstring lastChangelog_;

    // Every translatable text on the page (control, property, text) - set again by Retranslate()
    struct BoundText {
        IAIMPPropertyList* control;  // owned by the form, valid until DestroyFrame
        int prop;
        std::function<tstring()> text;
    };
    std::vector<BoundText> texts_;

    // Combo box contents
    std::vector<tstring> aimpOutputs_, winDevices_, asioDrivers_;
    std::vector<OutputMode> modes_;
#ifdef _WIN32
    std::vector<win::VmDevice> vmDevices_;
#endif

protected:
    bool Supports(REFIID riid) override { return EqualGUID(riid, IID_IAIMPOptionsDialogFrame); }

public:
    explicit OptionsFrame(Engine* e) : engine_(e) {}

    // ------------------------------------------------------------------ layout helpers
    // Same grid as the Discord Rich Presence plugin: every control has a fixed position in a 465 x 420
    // area (96 DPI, AIMP scales it), labels at x = 10, controls of a "label: control" row at x = 210.
    static const int kRowH = 22;  // height of edits / combo boxes

    // Fixed position, anchored to the top-left corner. Without anchors AIMP's UI keeps the relative
    // position while the tab sheet grows to the dialog size - controls would drift or be cut off.
    static void Place(IAIMPUIControl* c, int x, int y, int w, int h) {
        if (!c) return;
        TAIMPUIControlPlacement p;
        memset(&p, 0, sizeof(p));
        p.Alignment = ualNone;
        p.Anchors.left = 1;
        p.Anchors.top = 1;
        p.Bounds.left = x;
        p.Bounds.top = y;
        p.Bounds.right = x + w;
        p.Bounds.bottom = y + h;
        c->SetPlacement(p);
    }

    template <typename T>
    Ptr<T> Make(IAIMPUIWinControl* parent, const GUID& iid, int x, int y, int w, int h, bool events = true) {
        Ptr<T> c;
        if (!parent || Failed(ui_->CreateControl(form_.Get(), parent, nullptr, events ? events_.Get() : nullptr, iid,
                                                 c.OutV())) || !c)
            return c;
        Place(c.Get(), x, y, w, h);
        return c;
    }

    static void SetText(IAIMPPropertyList* c, int prop, const tstring& text) {
        auto s = MakeString(text);
        if (c && s) c->SetValueAsObject(prop, s.Get());
    }
    void Bind(IAIMPPropertyList* c, int prop, const Tx& t) {
        if (!c) return;
        SetText(c, prop, t.fn());
        texts_.push_back({c, prop, t.fn});
    }
    static tstring GetText(IAIMPPropertyList* c, int prop) {
        Ptr<IAIMPString> s;
        if (c && Succeeded(c->GetValueAsObject(prop, IID_IAIMPString, s.OutV())) && s) return FromString(s.Get());
        return tstring();
    }
    static void Enable(IAIMPPropertyList* c, bool on) {
        if (c) c->SetValueAsInt32(AIMPUI_CONTROL_PROPID_ENABLED, on ? 1 : 0);
    }
    static void Show(IAIMPPropertyList* c, bool on) {
        if (c) c->SetValueAsInt32(AIMPUI_CONTROL_PROPID_VISIBLE, on ? 1 : 0);
    }

    // Label; taller than one line = word wrap. With url it is a link that AIMP opens on click.
    Ptr<IAIMPUILabel> Label(IAIMPUIWinControl* p, const Tx& text, int x, int y, int w, int h = 18,
                            bool bold = false, const tstring& url = tstring()) {
        auto l = Make<IAIMPUILabel>(p, IID_IAIMPUILabel, x, y, w, h, false);
        if (!l) return l;
        l->SetValueAsInt32(AIMPUI_LABEL_PROPID_AUTOSIZE, 0);
        l->SetValueAsInt32(AIMPUI_LABEL_PROPID_WORDWRAP, h > 20 ? 1 : 0);
        if (bold) l->SetValueAsInt32(AIMPUI_LABEL_PROPID_TEXTSTYLE, AIMPUI_FLAGS_FONT_BOLD);
        Bind(l.Get(), AIMPUI_LABEL_PROPID_TEXT, text);
        if (!url.empty()) SetText(l.Get(), AIMPUI_LABEL_PROPID_URL, url);
        return l;
    }
    Ptr<IAIMPUILabel> Link(IAIMPUIWinControl* p, const Tx& text, const tstring& url, int x, int y, int w) {
        return Label(p, text, x, y, w, 18, false, url);
    }
    Ptr<IAIMPUICheckBox> Check(IAIMPUIWinControl* p, const Tx& caption, int x, int y, int w) {
        auto c = Make<IAIMPUICheckBox>(p, IID_IAIMPUICheckBox, x, y, w, 20);
        if (!c) return c;
        c->SetValueAsInt32(AIMPUI_CHECKBOX_PROPID_AUTOSIZE, 0);
        Bind(c.Get(), AIMPUI_CHECKBOX_PROPID_CAPTION, caption);
        return c;
    }
    Ptr<IAIMPUIComboBox> Combo(IAIMPUIWinControl* p, int x, int y, int w) {
        auto c = Make<IAIMPUIComboBox>(p, IID_IAIMPUIComboBox, x, y, w, kRowH);
        if (c) c->SetValueAsInt32(AIMPUI_COMBOBOX_PROPID_STYLE, AIMPUI_COMBOBOX_STYLE_LIST);
        return c;
    }
    Ptr<IAIMPUIButton> Button(IAIMPUIWinControl* p, const Tx& caption, int x, int y, int w) {
        auto b = Make<IAIMPUIButton>(p, IID_IAIMPUIButton, x, y, w, 25);
        if (b) Bind(b.Get(), AIMPUI_BUTTON_PROPID_CAPTION, caption);
        return b;
    }
    Ptr<IAIMPUIMemo> Memo(IAIMPUIWinControl* p, int x, int y, int w, int h) {
        auto m = Make<IAIMPUIMemo>(p, IID_IAIMPUIMemo, x, y, w, h, false);
        if (m) m->SetValueAsInt32(AIMPUI_BASEEDIT_PROPID_READONLY, 1);
        return m;
    }

    static void Fill(IAIMPUIComboBox* c, const std::vector<tstring>& items) {
        if (!c) return;
        c->Clear();
        for (auto& i : items) {
            auto s = MakeString(i);
            if (s) c->Add(s.Get(), 0);
        }
    }
    static void SetChecked(IAIMPUICheckBox* c, bool v) {
        if (c) c->SetValueAsInt32(AIMPUI_CHECKBOX_PROPID_STATE, v ? AIMPUI_CHECKSTATE_CHECKED : AIMPUI_CHECKSTATE_UNCHECKED);
    }
    static bool Checked(IAIMPUICheckBox* c) {
        INT32 v = 0;
        return c && Succeeded(c->GetValueAsInt32(AIMPUI_CHECKBOX_PROPID_STATE, &v)) && v == AIMPUI_CHECKSTATE_CHECKED;
    }
    static int Index(IAIMPUIComboBox* c) {
        INT32 v = -1;
        return c && Succeeded(c->GetValueAsInt32(AIMPUI_COMBOBOX_PROPID_ITEMINDEX, &v)) ? v : -1;
    }
    static void Select(IAIMPUIComboBox* c, int i) {
        if (c) c->SetValueAsInt32(AIMPUI_COMBOBOX_PROPID_ITEMINDEX, i);
    }

    // Tab sheet. name: internal name (latin letters), caption: visible title
    IAIMPUIWinControl* Page(const tstring& name, const Tx& caption) {
        Ptr<IAIMPUITabSheet> p;
        auto s = MakeString(name);
        if (!pages_ || !s || Failed(pages_->Add(s.Get(), p.Out())) || !p) return nullptr;
        Bind(p.Get(), AIMPUI_TABSHEET_PROPID_CAPTION, caption);
        containers_.push_back(p);
        return p.Get();  // a tab sheet is a IAIMPUIWinControl
    }

    // The author's picture (embedded PNG with rounded corners, drawn at 64 x 64 like in Discord RP)
    void Avatar(IAIMPUIWinControl* parent, int x, int y, int size) {
        auto img = Make<IAIMPUIImage>(parent, IID_IAIMPUIImage, x, y, size, size, false);
        Ptr<IAIMPMemoryStream> ms;
        Ptr<IAIMPImage> pic;
        if (!img || Failed(Core()->CreateObject(IID_IAIMPMemoryStream, ms.OutV())) || !ms ||
            Failed(Core()->CreateObject(IID_IAIMPImage, pic.OutV())) || !pic)
            return;
        DWORD written = 0;
        ms->Write((void*)kAvatarPng, (DWORD)sizeof(kAvatarPng), &written);
        ms->Seek(0, AIMP_STREAM_SEEKMODE_FROM_BEGINNING);
        if (Failed(pic->LoadFromStream(ms.Get()))) return;
        img->SetValueAsInt32(AIMPUI_IMAGE_PROPID_IMAGESTRETCHMODE, AIMP_IMAGE_DRAW_STRETCHMODE_FIT | AIMP_IMAGE_DRAW_QUALITY_HIGH);
        img->SetValueAsObject(AIMPUI_IMAGE_PROPID_IMAGE, pic.Get());
    }

    static tstring PlatformName() {
#ifdef _WIN32
        return sizeof(void*) == 8 ? AR_T("Windows x64") : AR_T("Windows x86");
#else
        return AR_T("Linux x86_64");
#endif
    }

    // file:// URL of a folder (AIMP opens it in the file manager)
    static tstring FolderUrl(const tstring& file) {
        size_t sep = file.find_last_of(AR_T("\\/"));
        tstring dir = sep == tstring::npos ? file : file.substr(0, sep);
        for (auto& c : dir)
            if (c == AR_T('\\')) c = AR_T('/');
#ifdef _WIN32
        return AR_T("file:///") + dir;
#else
        return AR_T("file://") + dir;
#endif
    }

    // "  • text ..." -> lines of at most 'width' characters, continuation lines indented like the text.
    // The memos have no word wrap; this keeps every line visible without scrolling sideways.
    static tstring Wrap(const tstring& text, size_t width) {
        size_t indent = text.find_first_not_of(AR_T(" -"));
        if (indent == tstring::npos) return text + AR_NL;
#ifdef _WIN32
        if (text[indent] == 0x2022) indent = text.find_first_not_of(AR_T(' '), indent + 1);
#else
        if (text.compare(indent, 3, "\xE2\x80\xA2") == 0) indent = text.find_first_not_of(' ', indent + 3);
#endif
        if (indent == tstring::npos) return text + AR_NL;
        tstring out, line;
        auto emit = [&] {
            out += (out.empty() ? text.substr(0, indent) : tstring(indent, AR_T(' '))) + line + AR_NL;
            line.clear();
        };
        tstring word;
        tstring rest = text.substr(indent) + AR_T(" ");
        for (TChar c : rest) {
            if (c != AR_T(' ')) { word += c; continue; }
            if (word.empty()) continue;
            if (!line.empty() && indent + line.size() + 1 + word.size() > width) emit();
            line += (line.empty() ? tstring() : tstring(AR_T(" "))) + word;
            word.clear();
        }
        emit();
        return out;
    }

    // CHANGELOG.md as plain text for the About tab: "## 2.5.0" -> "Version 2.5.0", "- item" -> "  • item"
    static tstring ChangelogText(const std::string& extraVersion, const std::string& extraNotes) {
        tstring out;
        std::string item;
        auto flush = [&] {
            if (!item.empty()) out += Wrap(FromUtf8(item), 76);
            item.clear();
        };
        auto lines = [](const std::string& text) {
            std::vector<std::string> r;
            size_t pos = 0;
            while (pos <= text.size()) {
                size_t end = text.find('\n', pos);
                if (end == std::string::npos) end = text.size();
                std::string l = text.substr(pos, end - pos);
                if (!l.empty() && l.back() == '\r') l.pop_back();
                r.push_back(l);
                pos = end + 1;
            }
            return r;
        };
        auto add = [&](const std::string& line) {
            std::string t = line;
            t.erase(std::remove(t.begin(), t.end(), '`'), t.end());
            t.erase(std::remove(t.begin(), t.end(), '*'), t.end());
            size_t a = t.find_first_not_of(' ');
            t = a == std::string::npos ? std::string() : t.substr(a);
            if (t.compare(0, 2, "# ") == 0) return;
            if (t.compare(0, 3, "## ") == 0) {
                flush();
                std::string v = t.substr(3);
                if (!out.empty()) out += AR_NL;
                out += FromUtf8("Version " + v + (v == AR_VERSION ? "  (installed)" : "")) + AR_NL;
            } else if (t.compare(0, 4, "### ") == 0) {
                flush();
                out += FromUtf8(t.substr(4) + ":") + AR_NL;
            } else if (t.compare(0, 2, "- ") == 0) {
                flush();
                item = "  \xE2\x80\xA2 " + t.substr(2);
            } else if (!t.empty() && !item.empty()) {
                item += " " + t;  // continuation line of a list item
            } else {
                flush();
                if (!t.empty()) out += Wrap(FromUtf8(t), 76);  // paragraphs are wrapped like list items
            }
        };
        if (!extraVersion.empty()) {
            add("## " + extraVersion + "  (available)");
            for (auto& l : lines(extraNotes)) add(l);
            flush();
        }
        for (auto& l : lines(AR_CHANGELOG_TEXT)) add(l);
        flush();
        return out;
    }

    // "Detected: <cpu>, 8 threads, 3.6 GHz, 16 GB RAM → fast profile (150 ms after each switch)."
    tstring DetectedText() const {
        const Hardware& hw = engine_->hw;
        Timing tm = Timing::For(hw, true, 0);
        tstring d = hw.cpu.empty() ? tstring() : hw.cpu + AR_T(", ");
        d += Trf("%1 threads", {Num(hw.threads)});
        if (hw.mhz) d += AR_T(", ") + Num(hw.mhz / 1000) + AR_T(".") + Num((hw.mhz % 1000) / 100) + AR_T(" GHz");
        if (hw.ramGB) d += AR_T(", ") + Num(hw.ramGB) + AR_T(" GB RAM");
        const char* speed = hw.speed == Speed::Fast ? "fast" : hw.speed == Speed::Normal ? "normal" : "conservative";
        return Trf("Detected: %1 → %2 profile (%3 ms after each switch).", {d, Tr(speed), Num(tm.settleMs)});
    }

    // ------------------------------------------------------------------ building the page
    void Build() {
        IAIMPUIWinControl* root = form_.Get();  // the form is a IAIMPUIWinControl
        ui_->CreateControl(form_.Get(), root, nullptr, events_.Get(), IID_IAIMPUIPageControl, pages_.OutV());
        if (!pages_) return;
        TAIMPUIControlPlacement p;
        memset(&p, 0, sizeof(p));
        p.Alignment = ualClient;
        pages_->SetPlacement(p);
        int y;

        // --- General
        IAIMPUIWinControl* g = Page(AR_T("General"), "General");
        y = 8;
        Label(g, "Switches your audio output to the sample rate of each track, so nothing gets resampled "
                 "(bit-perfect playback).", 10, y, 455, 34);                                                y += 40;
        Label(g, "Warning: only use this plugin while you are listening to music and nothing else. It changes "
                 "the sample rate of your audio devices (and Voicemeeter) with every track. If other "
                 "applications play sound at the same time (games, videos, calls), disable the plugin and "
                 "let Windows/AIMP resample instead.", 10, y, 455, 82, true);                                y += 90;
        cbEnabled_ = Check(g, "Enable Prevent Resampling", 10, y, 455);                                      y += 26;
        cbMultiples_ = Check(g, "Fall back to integer multiples if needed (44.1 → 88.2 kHz)", 10, y, 455); y += 24;
        cbRestore_ = Check(g, "Restore the original sample rates when AIMP closes", 10, y, 455);             y += 24;
        cbAutoTiming_ = Check(g, "Automatic timing based on your hardware", 10, y, 455);                     y += 22;
        Label(g, Tx([this] { return DetectedText(); }), 28, y, 437, 34);                                     y += 40;
        Label(g, "Settle time without automatic timing (ms):", 10, y + 3, 295);
        spSettle_ = Make<IAIMPUISpinEdit>(g, IID_IAIMPUISpinEdit, 315, y, 150, kRowH);
        if (spSettle_) {
            spSettle_->SetValueAsInt32(AIMPUI_SPINEDIT_PROPID_VALUETYPE, AIMPUI_SPINEDIT_VALUETYPE_INTEGER);
            spSettle_->SetValueAsInt32(AIMPUI_SPINEDIT_PROPID_MINVALUE, 0);
            spSettle_->SetValueAsInt32(AIMPUI_SPINEDIT_PROPID_MAXVALUE, 5000);
            spSettle_->SetValueAsInt32(AIMPUI_SPINEDIT_PROPID_INCREMENT, 50);
        }                                                                                                    y += 34;

        // --- Output
        IAIMPUIWinControl* o = Page(AR_T("Output"), "Output");
        y = 8;
        Label(o, "AIMP output device:", 10, y + 3, 195);
        coAimpOut_ = Combo(o, 210, y, 255);                                                                  y += 28;
        Label(o, "Same setting as AIMP → Playback → Output. Only changed if you pick another device here.",
              10, y, 455, 34);                                                                               y += 36;
        Label(o, "Output type:", 10, y + 3, 195);
        coMode_ = Combo(o, 210, y, 255);                                                                     y += 32;
#ifdef _WIN32
        Label(o, "Windows device:", 10, y + 3, 195);
        coWinDev_ = Combo(o, 210, y, 255);                                                                   y += 28;
        Label(o, "Used for WASAPI shared and DirectSound. Automatic = the device of AIMP's output, otherwise the "
                 "Windows default device.", 10, y, 455, 34);                                                 y += 40;
        Label(o, "Additional devices to switch:", 10, y + 3, 195);
        edExtra_ = Make<IAIMPUIEdit>(o, IID_IAIMPUIEdit, 210, y, 255, kRowH);                                y += 28;
        Label(o, "Parts of device names, separated by ';' (e.g. FiiO; Realtek Digital).", 10, y, 455);      y += 30;
#else
        Label(o, "AIMP for Linux resamples every track to its own rate (Sound Output -> Parameters). "
                 "PipeWire is set to that rate ('pw-metadata clock.force-rate'), so nothing is resampled a "
                 "second time. Following each track needs an AIMP restart - planned for a later version.",
              10, y, 455, 66);                                                                               y += 76;
#endif
        // Bit-perfect mode (every platform): switches off AIMP's own sound processing, restores it when off
        cbBp_ = Check(o, "Bit-perfect mode (switch off AIMP's sound processing)", 10, y, 455);              y += 22;
        Label(o, "Turns off crossfade, equalizer, effects and ReplayGain. Unticking brings your old settings back.",
              28, y, 437, 34);                                                                               y += 36;
        cbBpVolume_ = Check(o, "Set AIMP's volume to 100 %", 28, y, 437);                                     y += 22;
        cbBpGapless_ = Check(o, "No gapless playback (short pause between tracks)", 28, y, 437);              y += 24;
#ifdef _WIN32
        lbBpStatus_ = Label(o, tstring(), 28, y, 297, 34);
        btBpRestart_ = Button(o, "Restart AIMP now", 335, y + 4, 130);                                       y += 38;
        Label(o, "Voicemeeter: for 100 % bit-perfect output, do not use its equalizer or other effects on "
                 "the bus and keep the fader at 0 dB.", 10, y, 455, 34);
#else
        lbBpStatus_ = Label(o, tstring(), 28, y, 437, 34);
#endif

        // --- ASIO / Exclusive (experimental restart)
#ifdef _WIN32
        IAIMPUIWinControl* x = Page(AR_T("Exclusive"), "ASIO / Exclusive");
        y = 8;
        Label(x, "EXPERIMENTAL – please read before enabling", 10, y, 455, 18, true);                   y += 24;
        Label(x, "With ASIO, WASAPI exclusive (event/push) and DirectSound, AIMP plays with its own fixed "
                 "rate (Preferences → Sound Output) and reads it only at start-up. The only way to follow "
                 "the track is a workaround: stop playback, close AIMP, write the new rate into AIMP.ini, "
                 "start AIMP again and continue at the same position.", 10, y, 455, 82);                     y += 88;
        Label(x, "Expect a short interruption on every rate change. In rare cases AIMP may not come back by "
                 "itself or settings may be lost – keep a backup of AIMP.ini. Use at your own risk.",
              10, y, 455, 50, true);                                                                         y += 58;
        cbRestart_ = Check(x, "Restart AIMP to apply the track's sample rate", 10, y, 455);                  y += 24;
        cbSeamless_ = Check(x, "Seamless restart (show a still image of AIMP meanwhile)", 28, y, 437);       y += 26;
        Label(x, "While this is off, the plugin does not follow the track on these outputs; it only keeps "
                 "Voicemeeter at AIMP's own rate. WASAPI shared never needs a restart.", 10, y, 455, 50);    y += 56;
        Label(x, "ASIO driver:", 10, y + 3, 195);
        coAsio_ = Combo(x, 210, y, 255);                                                                     y += 30;
        cbAsioForce_ = Check(x, "Set the ASIO driver's rate directly (only if AIMP does not)", 10, y, 455);
#endif

        // --- Voicemeeter (the same page on every platform; on Linux everything is shown but disabled)
        IAIMPUIWinControl* v = Page(AR_T("Voicemeeter"), "Voicemeeter");
        y = 8;
#ifdef _WIN32
        Label(v, "Only relevant if you use Voicemeeter (VB-Audio). Voicemeeter exists for Windows only, so "
                 "this tab has no function on Linux.", 10, y, 455, 34);                                      y += 40;
        lbVmStatus_ = Label(v, tstring(), 10, y, 455, 34, true);                                             y += 40;
#else
        Label(v, "Linux detected – this tab is disabled. Voicemeeter (VB-Audio) exists for Windows only; "
                 "on Linux the plugin switches PipeWire directly (Output tab). Linux mixers such as "
                 "Pulsemeeter may be supported in a later version.", 10, y, 455, 66, true);                  y += 72;
        lbVmStatus_ = Label(v, "Voicemeeter: not available on Linux", 10, y, 455, 34, true);                y += 40;
#endif
        cbVm_ = Check(v, "Follow Voicemeeter when AIMP plays into one of its virtual devices", 10, y, 455); y += 24;
        cbVmEngine_ = Check(v, "Switch the engine sample rate (Option.sr)", 28, y, 437);                     y += 24;
        cbVmAllBuses_ = Check(v, "Switch all hardware outputs A1–A5, not only A1", 28, y, 437);         y += 24;
        cbVmAsio_ = Check(v, "ASIO device on A1 follows the engine rate (Option.ASIOsr)", 28, y, 437);       y += 32;
        Label(v, "A1 output device:", 10, y + 3, 195);
        coVmA1_ = Combo(v, 210, y, 255);                                                                     y += 28;
        Label(v, "Driver type WDM / KS / MME / ASIO. Sent to Voicemeeter when you click Apply; for "
                 "bit-perfect playback run A1 via ASIO or WDM.", 10, y, 455, 34);                            y += 42;
#ifdef _WIN32
        Label(v, "Required: tick “Auto restart audio engine (all devices)” in Voicemeeter's settings. "
                 "Without it, outputs on WDM (e.g. A2) can turn red after a rate change or when AIMP closes. "
                 "The Voicemeeter API offers no way to set this option from the plugin.", 10, y, 455, 66, true);
#else
        for (IAIMPPropertyList* c : std::initializer_list<IAIMPPropertyList*>{
                 cbVm_.Get(), cbVmEngine_.Get(), cbVmAllBuses_.Get(), cbVmAsio_.Get(), coVmA1_.Get()})
            Enable(c, false);
#endif

        // --- Advanced: language, settings file, statistics and the latest log entries
        IAIMPUIWinControl* st = Page(AR_T("Advanced"), "Advanced");
        y = 8;
        Label(st, "Language:", 10, y + 3, 135);
        coLang_ = Combo(st, 150, y, 315);                                                                    y += 30;
        Label(st, "Settings file:", 10, y + 4, 135);
        btExport_ = Button(st, "Export...", 150, y, 100);
        btImport_ = Button(st, "Import...", 256, y, 100);
        Link(st, "Open folder", FolderUrl(Ini::Get().Path()), 366, y + 4, 99);                             y += 33;
        memo_ = Memo(st, 10, y, 455, 370 - y);
        btRefresh_ = Button(st, "Refresh", 10, 378, 110);
        btReset_ = Button(st, "Reset counters", 128, 378, 130);
#ifdef _WIN32
        btTest_ = Button(st, "Test format", 266, 378, 130);
        if (btTest_) Bind(btTest_.Get(), AIMPUI_CONTROL_PROPID_HINT,
                          "Switches the Windows default format back and forth once (stop playback first)");
#endif

        // --- About (same layout as the Discord Rich Presence plugin)
        IAIMPUIWinControl* a = Page(AR_T("About"), "About");
        Avatar(a, 10, 8, 64);
        auto title = Label(a, Tx([] { return Trf("%1 for AIMP", {AR_T(AR_PLUGIN_NAME)}); }), 86, 8, 379, 26, true);
        if (title) {
            title->SetValueAsInt32(AIMPUI_LABEL_PROPID_WORDWRAP, 0);
            title->SetValueAsInt32(AIMPUI_LABEL_PROPID_TEXTSIZE, 13);
        }
        Label(a, Tx([] { return Trf("Version %1", {AR_T(AR_VERSION)}) + AR_T(" – ") + PlatformName(); }), 86, 36, 379);
        Link(a, Tx([] { return Trf("by %1 (RainBowFl4sh on GitHub)", {AR_T(AR_AUTHOR)}); }), AR_T(AR_URL_AUTHOR), 86, 56, 379);
        y = 84;
        Link(a, "GitHub page", AR_T(AR_URL_GITHUB), 10, y, 140);
        Link(a, "All releases", AR_T(AR_URL_RELEASES), 160, y, 140);
        Link(a, "Report a problem", AR_T(AR_URL_ISSUES), 310, y, 155);                                      y += 28;
        cbUpdate_ = Check(a, "Check for updates:", 10, y, 185);
        coInterval_ = Combo(a, 200, y, 160);
        btCheck_ = Button(a, "Check now", 368, y - 1, 97);                                                   y += 30;
        cbAutoInstall_ = Check(a, "Install updates automatically", 10, y, 455);                             y += 26;
        lbUpdate_ = Label(a, tstring(), 10, y, 335, 34);
        btInstall_ = Button(a, tstring(), 355, y, 110);
        lnNotes_ = Link(a, "What's new?", AR_T(AR_URL_RELEASES "/latest"), 355, y + 28, 110);                y += 52;
        Label(a, "Changelog:", 10, y, 455);                                                                  y += 18;
        memoChangelog_ = Memo(a, 10, y, 455, 412 - y);
        Show(btInstall_.Get(), false);
        Show(lnNotes_.Get(), false);
    }

    // Status line, buttons and changelog of the About tab
    void RefreshAbout() {
        if (!form_) return;
        Updater& u = engine_->updater;
        tstring status = u.Message(), version = FromUtf8(u.latest);
        bool install = u.CanInstall();
        if (u.state == Updater::State::Idle) {
            tstring latest = Ini::Get().Str(AR_T("Updates"), AR_T("LatestVersion"));
            if (!latest.empty() && CompareVersions(ToUtf8(latest), AR_VERSION) > 0) {
                version = latest;
                status = Trf("Version %1 is available (last check: %2).", {latest, Updater::LastCheckText()});
            } else if (Ini::Get().Bool(AR_T("Updates"), AR_T("LastCheckFailed"), false)) {
                status = Trf("The last update check failed (%1). Click \"Check now\" to try again.", {Updater::LastCheckText()});
            } else {
                status = Ini::Get().Int(AR_T("Updates"), AR_T("LastCheck"), 0) > 0
                             ? Trf("You have the latest version (last check: %1).", {Updater::LastCheckText()})
                             : Tr("Not checked for updates yet.");
            }
        }
        SetText(lbUpdate_.Get(), AIMPUI_LABEL_PROPID_TEXT, status);
        Enable(btCheck_.Get(), !u.Busy());
        SetText(btInstall_.Get(), AIMPUI_BUTTON_PROPID_CAPTION, Trf("Install %1", {version}));
        Show(btInstall_.Get(), install);
        Show(lnNotes_.Get(), install);
        bool newer = !u.latest.empty() && CompareVersions(u.latest, AR_VERSION) > 0;
        tstring log = ChangelogText(newer ? u.latest : std::string(), newer ? u.notes : std::string());
        if (log != lastChangelog_) {
            lastChangelog_ = log;
            SetText(memoChangelog_.Get(), AIMPUI_BASEEDIT_PROPID_TEXT, log);
        }
    }

    // Reads AIMP's output list and shows the output currently selected in AIMP
    void SyncAimpOutput() {
        aimpOutputs_.clear();
        Ptr<IAIMPPropertyList> pl;
        tstring current;
        if (engine_->player && Succeeded(engine_->player->QueryInterface(IID_IAIMPPropertyList, pl.OutV())) && pl) {
            Ptr<IAIMPObjectList> list;
            if (Succeeded(pl->GetValueAsObject(AIMP_PLAYER_PROPID_OUTPUT, IID_IAIMPObjectList, list.OutV())) && list) {
                for (INT32 i = 0; i < list->GetCount(); i++) {
                    Ptr<IAIMPString> s;
                    if (Succeeded(list->GetObject(i, IID_IAIMPString, s.OutV())) && s) aimpOutputs_.push_back(FromString(s.Get()));
                }
            }
            current = GetText(pl.Get(), AIMP_PLAYER_PROPID_OUTPUT);
        }
        // AIMP 4 offers no output list for plugins: only show its current output (read-only)
        bool selectable = !aimpOutputs_.empty();
        if (current.empty()) current = ConfiguredAimpOutput();
        if (!current.empty() && std::find(aimpOutputs_.begin(), aimpOutputs_.end(), current) == aimpOutputs_.end())
            aimpOutputs_.insert(aimpOutputs_.begin(), current);
        Fill(coAimpOut_.Get(), aimpOutputs_);
        for (size_t i = 0; i < aimpOutputs_.size(); i++)
            if (aimpOutputs_[i] == current) Select(coAimpOut_.Get(), (int)i);
        Enable(coAimpOut_.Get(), selectable);
        aimpOutChanged_ = false;
    }

    // Fills the lists (devices may have changed since the last time)
    void Populate() {
#ifdef _WIN32
        // device lists need COM; AIMP's main thread normally has it already (then this only counts up)
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        modes_ = {OutputMode::Auto, OutputMode::Shared, OutputMode::Exclusive, OutputMode::Asio, OutputMode::DirectSound};
        Fill(coMode_.Get(), {Tr("Automatic (from AIMP's output device)"), Tr("WASAPI shared"),
                             Tr("WASAPI exclusive (event / push)"), AR_T("ASIO"), AR_T("DirectSound")});

        winDevices_.clear();
        std::vector<tstring> items{Tr("Automatic (from AIMP's output)")};
        for (auto& e : win::ListRender()) { winDevices_.push_back(e.name); items.push_back(e.name); }
        Fill(coWinDev_.Get(), items);

        asioDrivers_.clear();
        items = {Tr("Automatic (from AIMP's output)")};
        for (auto& d : win::ListAsioDrivers()) { asioDrivers_.push_back(d.name); items.push_back(d.name); }
        Fill(coAsio_.Get(), items);

        auto& vm = win::Voicemeeter::Get();
        vmDevices_.clear();
        if (vm.Installed()) {
            long t = vm.Type();
            tstring status = t ? Trf("%1 is running", {tstring(win::Voicemeeter::TypeName(t))})
                               : Tr("Voicemeeter is installed but not running");
            if (t) {
                uint32_t eng = vm.EngineRate();
                status += AR_T(" \u00B7 ") + Trf("engine %1", {eng ? Khz(eng) : tstring(AR_T("?"))});
                for (auto& b : vm.BusDevices(true)) status += AR_T(" · A") + Num(b.first + 1) + AR_T(": ") + b.second;
            }
            SetText(lbVmStatus_.Get(), AIMPUI_LABEL_PROPID_TEXT, status);
            vmDevices_ = vm.OutputDevices();
        } else {
            SetText(lbVmStatus_.Get(), AIMPUI_LABEL_PROPID_TEXT, Tr("Voicemeeter is not installed \u2013 this tab has no effect."));
        }
        items = {Tr("(keep the current device)")};
        for (auto& d : vmDevices_) items.push_back(d.Label());
        Fill(coVmA1_.Get(), items);
        Select(coVmA1_.Get(), 0);
        if (SUCCEEDED(co)) CoUninitialize();
#else
        modes_ = {OutputMode::Auto, OutputMode::PipeWire, OutputMode::Direct};
        Fill(coMode_.Get(), {Tr("Automatic (PipeWire if available)"), Tr("PipeWire (clock.force-rate)"),
                             Tr("ALSA direct (nothing to switch)")});
        Fill(coVmA1_.Get(), {Tr("(Voicemeeter is Windows only)")});
        Select(coVmA1_.Get(), 0);
#endif
        Fill(coInterval_.Get(), {Tr("at every AIMP start"), Tr("once a day"), Tr("once a week"), Tr("once a month")});
        tstring aimpLang;
        Language detected = AimpLanguage(&aimpLang);
        Fill(coLang_.Get(), {Trf("Automatic (AIMP: %1)", {FromUtf8(LanguageName(detected))}), FromUtf8(LanguageName(LangEn)),
                             FromUtf8(LanguageName(LangDe)), FromUtf8(LanguageName(LangRu)), FromUtf8(LanguageName(LangUk))});
    }

    // Selects a stored name in a combo box; unknown names are added
    void SelectName(IAIMPUIComboBox* c, std::vector<tstring>& list, const tstring& name) {
        if (!c) return;
        if (name.empty()) { Select(c, 0); return; }
        for (size_t i = 0; i < list.size(); i++)
            if (list[i] == name) { Select(c, (int)i + 1); return; }
        list.push_back(name);
        auto s = MakeString(name + AR_T(" (") + Tr("not connected") + AR_T(")"));
        if (s) c->Add(s.Get(), 0);
        Select(c, (int)list.size());
    }

    // Enables/disables dependent controls
    void UpdateDependencies() {
        Enable(spSettle_.Get(), !Checked(cbAutoTiming_.Get()));
        Enable(cbSeamless_.Get(), Checked(cbRestart_.Get()));
        bool bp = Checked(cbBp_.Get());  // the options appear once the mode is ticked
        Show(cbBpVolume_.Get(), bp);
        Show(cbBpGapless_.Get(), bp);
#ifdef _WIN32
        bool vm = Checked(cbVm_.Get());
        Enable(cbVmEngine_.Get(), vm);
        Enable(cbVmAllBuses_.Get(), vm);
        Enable(cbVmAsio_.Get(), vm && Checked(cbVmEngine_.Get()));
#endif
        bool upd = Checked(cbUpdate_.Get());
        Enable(coInterval_.Get(), upd);
        Enable(cbAutoInstall_.Get(), upd);
    }

    void ToControls(const Config& c) {
        loading_ = true;
        SyncAimpOutput();
        SetChecked(cbEnabled_.Get(), c.enabled);
        SetChecked(cbMultiples_.Get(), c.allowMultiples);
        SetChecked(cbRestore_.Get(), c.restoreOnExit);
        SetChecked(cbAutoTiming_.Get(), c.autoTiming);
        SetChecked(cbBp_.Get(), c.bitPerfect);
        SetChecked(cbBpVolume_.Get(), c.bpVolume100);
        SetChecked(cbBpGapless_.Get(), c.bpNoGapless);
        if (spSettle_) spSettle_->SetValueAsInt32(AIMPUI_SPINEDIT_PROPID_VALUE, c.settleMs);
        Select(coMode_.Get(), 0);
        for (size_t i = 0; i < modes_.size(); i++)
            if (modes_[i] == c.mode) Select(coMode_.Get(), (int)i);
#ifdef _WIN32
        SelectName(coWinDev_.Get(), winDevices_, c.windowsDevice);
        SelectName(coAsio_.Get(), asioDrivers_, c.asioDriver);
        SetText(edExtra_.Get(), AIMPUI_BASEEDIT_PROPID_TEXT, c.extraDevices);
        SetChecked(cbRestart_.Get(), c.restartAimp);
        SetChecked(cbSeamless_.Get(), c.seamlessRestart);
        SetChecked(cbAsioForce_.Get(), c.asioForce);
        SetChecked(cbVm_.Get(), c.vmEnabled);
        SetChecked(cbVmEngine_.Get(), c.vmEngineRate);
        SetChecked(cbVmAllBuses_.Get(), c.vmAllBuses);
        SetChecked(cbVmAsio_.Get(), c.vmAsioRate);
#else
        SetChecked(cbVm_.Get(), c.vmEnabled);  // shown for reference only (disabled on Linux)
        SetChecked(cbVmEngine_.Get(), c.vmEngineRate);
        SetChecked(cbVmAllBuses_.Get(), c.vmAllBuses);
        SetChecked(cbVmAsio_.Get(), c.vmAsioRate);
#endif
        SetChecked(cbUpdate_.Get(), c.updateCheck);
        Select(coInterval_.Get(), c.updateInterval);
        SetChecked(cbAutoInstall_.Get(), c.updateAutoInstall);
        Select(coLang_.Get(), (int)c.language);
        UpdateDependencies();
        loading_ = false;
    }

    Config FromControls() const {
        Config c = engine_->cfg;
        c.enabled = Checked(cbEnabled_.Get());
        c.allowMultiples = Checked(cbMultiples_.Get());
        c.restoreOnExit = Checked(cbRestore_.Get());
        c.autoTiming = Checked(cbAutoTiming_.Get());
        c.bitPerfect = Checked(cbBp_.Get());
        c.bpVolume100 = Checked(cbBpVolume_.Get());
        c.bpNoGapless = Checked(cbBpGapless_.Get());
        INT32 v = c.settleMs;
        if (spSettle_ && Succeeded(spSettle_->GetValueAsInt32(AIMPUI_SPINEDIT_PROPID_VALUE, &v)))
            c.settleMs = (std::max)(0, (std::min)((int)v, 5000));
        int m = Index(coMode_.Get());
        if (m >= 0 && m < (int)modes_.size()) c.mode = modes_[m];
#ifdef _WIN32
        int i = Index(coWinDev_.Get());
        c.windowsDevice = (i > 0 && i <= (int)winDevices_.size()) ? winDevices_[i - 1] : tstring();
        i = Index(coAsio_.Get());
        c.asioDriver = (i > 0 && i <= (int)asioDrivers_.size()) ? asioDrivers_[i - 1] : tstring();
        c.extraDevices = Trim(GetText(edExtra_.Get(), AIMPUI_BASEEDIT_PROPID_TEXT));
        c.restartAimp = Checked(cbRestart_.Get());
        c.seamlessRestart = Checked(cbSeamless_.Get());
        c.asioForce = Checked(cbAsioForce_.Get());
        c.vmEnabled = Checked(cbVm_.Get());
        c.vmEngineRate = Checked(cbVmEngine_.Get());
        c.vmAllBuses = Checked(cbVmAllBuses_.Get());
        c.vmAsioRate = Checked(cbVmAsio_.Get());
#endif
        c.updateCheck = Checked(cbUpdate_.Get());
        int iv = Index(coInterval_.Get());
        if (iv >= 0 && iv <= 3) c.updateInterval = iv;
        c.updateAutoInstall = Checked(cbAutoInstall_.Get());
        int l = Index(coLang_.Get());
        if (l >= 0 && l <= 4) c.language = (LanguageSetting)l;
        return c;
    }

    void Save() {
        Config c = FromControls();
        bool wasEnabled = engine_->cfg.enabled;
        engine_->cfg = c;
        c.Save();

        // Change AIMP's output only if a different device was picked on this page
        int i = aimpOutChanged_ ? Index(coAimpOut_.Get()) : -1;
        aimpOutChanged_ = false;
        Ptr<IAIMPPropertyList> pl;
        if (i >= 0 && i < (int)aimpOutputs_.size() && engine_->player &&
            Succeeded(engine_->player->QueryInterface(IID_IAIMPPropertyList, pl.OutV())) && pl &&
            GetText(pl.Get(), AIMP_PLAYER_PROPID_OUTPUT) != aimpOutputs_[i]) {
            auto s = MakeString(aimpOutputs_[i]);
            HRESULT hr = s ? pl->SetValueAsObject(AIMP_PLAYER_PROPID_OUTPUT, s.Get()) : E_FAIL;
            Log(AR_T("AIMP output -> ") + aimpOutputs_[i] + (Succeeded(hr) ? AR_T("") : AR_T("  (rejected)")));
        }
#ifdef _WIN32
        // Voicemeeter: set the A1 device
        i = Index(coVmA1_.Get());
        if (i > 0 && i <= (int)vmDevices_.size()) {
            auto& vm = win::Voicemeeter::Get();
            const win::VmDevice& d = vmDevices_[i - 1];
            if (vm.SetBusDevice(0, d)) {
                if (c.vmAsioRate && d.type == 5) vm.SetFloat("Option.ASIOsr", 1.0f);
                Log(AR_T("Voicemeeter A1 -> ") + d.Label());
            } else {
                Log(AR_T("Voicemeeter A1 could not be set (is Voicemeeter running?)"));
            }
            Select(coVmA1_.Get(), 0);
        }
#endif
        engine_->bitPerfect.Apply(c);
        RefreshBitPerfect();
        if (wasEnabled != c.enabled) Log(c.enabled ? AR_T("Plugin enabled") : AR_T("Plugin disabled"));
        if (!wasEnabled && c.enabled) engine_->Recheck();  // switch to the rate of the track that is playing now
        Log(AR_T("Settings saved"));
    }

    // Bit-perfect mode: which settings still wait for a restart of AIMP
    void RefreshBitPerfect() {
        if (!lbBpStatus_) return;
        tstring names;
        for (const char* n : engine_->bitPerfect.PendingNames(engine_->cfg)) names += (names.empty() ? tstring() : tstring(AR_T(", "))) + Tr(n);
        tstring text;
#ifdef _WIN32
        if (!names.empty()) text = Trf("Changed when AIMP restarts: %1.", {names});
        Show(btBpRestart_.Get(), !names.empty());
#else
        if (!names.empty()) text = Trf("Please switch off in AIMP's sound effects: %1.", {names});
#endif
        SetText(lbBpStatus_.Get(), AIMPUI_LABEL_PROPID_TEXT, text);
    }

    void RefreshStats() {
        if (!memo_) return;
        tstring text = engine_->stats.Snapshot().Format(engine_->cfg.enabled);
        text += AR_NL;
        text += Tr("RECENT EVENTS") + AR_NL;
        auto recent = Logger::Get().Recent();
        for (auto it = recent.rbegin(); it != recent.rend(); ++it) text += AR_T("  ") + *it + AR_NL;
        // wrap long lines (device chains, log entries) so nothing is hidden behind a horizontal scroll bar
        tstring wrapped, line;
        for (TChar c : text) {
            if (c == AR_T('\r')) continue;
            if (c == AR_T('\n')) { wrapped += line.empty() ? tstring(AR_NL) : Wrap(line, 76); line.clear(); }
            else line += c;
        }
        if (!line.empty()) wrapped += Wrap(line, 76);
        text = wrapped;
        if (text == lastStats_) return;
        lastStats_ = text;
        SetText(memo_.Get(), AIMPUI_BASEEDIT_PROPID_TEXT, text);
    }

    bool IsOpen() const { return (bool)form_; }

    // AIMP's language was changed while the page is open
    void OnAimpLanguageChanged() {
        if (form_) ApplyLanguage(FromControls().language, true);
        else CurrentLanguage() = ResolveLanguage(engine_->cfg.language);
    }

    // Diagnostics: switches the Windows default format of the device once and back,
    // so the log shows whether and why Windows refuses the change.
    void TestWindowsFormat() {
#ifdef _WIN32
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        Log(AR_T("=== Windows format test ==="));
        auto all = win::ListRender();
        win::Endpoint e;
        Ptr<IAIMPPropertyList> pl;
        tstring out;
        if (engine_->player && Succeeded(engine_->player->QueryInterface(IID_IAIMPPropertyList, pl.OutV())) && pl)
            out = GetText(pl.Get(), AIMP_PLAYER_PROPID_OUTPUT);
        bool found = !engine_->cfg.windowsDevice.empty() ? win::FindByName(all, engine_->cfg.windowsDevice, e)
                                                         : (win::FindInText(all, out, e) || win::DefaultRender(e));
        if (!found) {
            Log(AR_T("No Windows device found (") + Num((long long)all.size()) + AR_T(" active playback devices)"));
        } else {
            WAVEFORMATEXTENSIBLE f;
            if (!win::GetDeviceFormat(e.id, f)) {
                Log(e.name + AR_T(": default format not readable"));
            } else {
                uint32_t orig = f.Format.nSamplesPerSec, alt = orig == 44100 ? 48000 : 44100;
                Log(e.name + AR_T(": currently ") + Khz(orig) + AR_T(", ") + Num(f.Format.wBitsPerSample) + AR_T(" bit, ") +
                    Num(f.Format.nChannels) + AR_T(" ch"));
                int sup = win::SupportsExclusive(e.id, win::WithRate(f, alt));
                Log(AR_T("  driver reports ") + Khz(alt) + AR_T(": ") +
                    (sup == 1 ? tstring(AR_T("supported")) : sup == 0 ? tstring(AR_T("NOT supported")) : tstring(AR_T("unknown"))));
                bool ok = win::SetDeviceFormat(e.id, win::WithRate(f, alt));
                Log(AR_T("  switch to ") + Khz(alt) + (ok ? AR_T(": OK") : AR_T(": FAILED")));
                if (ok) {
                    bool back = win::SetDeviceFormat(e.id, f);
                    Log(AR_T("  back to ") + Khz(orig) + (back ? AR_T(": OK") : AR_T(": FAILED")));
                }
            }
        }
        if (SUCCEEDED(co)) CoUninitialize();
#endif
    }

    // Sets every text of the page again in the current language; selections stay as they are
    void Retranslate() {
        if (!form_) return;
        bool wasLoading = loading_;
        loading_ = true;
        Config cur = FromControls();
        int vmA1 = Index(coVmA1_.Get());
        for (auto& t : texts_) SetText(t.control, t.prop, t.text());
        RefreshBitPerfect();
        Populate();
        Select(coMode_.Get(), 0);
        for (size_t i = 0; i < modes_.size(); i++)
            if (modes_[i] == cur.mode) Select(coMode_.Get(), (int)i);
#ifdef _WIN32
        SelectName(coWinDev_.Get(), winDevices_, cur.windowsDevice);
        SelectName(coAsio_.Get(), asioDrivers_, cur.asioDriver);
#endif
        if (vmA1 > 0) Select(coVmA1_.Get(), vmA1);
        Select(coInterval_.Get(), cur.updateInterval);
        Select(coLang_.Get(), (int)cur.language);
        lastStats_.clear();
        RefreshStats();
        lastChangelog_.clear();
        RefreshAbout();
        loading_ = wasLoading;
    }

    // Switches the page to the language of a setting (Automatic = AIMP's language)
    void ApplyLanguage(LanguageSetting s, bool force = false) {
        int l = (int)ResolveLanguage(s);
        if (l == CurrentLanguage() && !force) return;
        CurrentLanguage() = l;
        Retranslate();
    }

#ifdef _WIN32
    // Windows' own dialog, for AIMP versions without IAIMPUIFileDialogs
    bool AskFileWindows(bool save, const tstring& caption, const tstring& filterName, tstring& path) {
        wchar_t file[MAX_PATH] = L"PreventResampling-settings.ini";
        if (!save) file[0] = 0;
        std::wstring filter = filterName + L" (*.ini)";
        filter += L'\0';
        filter += L"*.ini";
        filter += L'\0';
        filter += L'\0';
        OPENFILENAMEW ofn;
        memset(&ofn, 0, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = form_ ? form_->GetHandle() : nullptr;
        ofn.lpstrFilter = filter.c_str();
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = caption.c_str();
        ofn.lpstrDefExt = L"ini";
        ofn.Flags = OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        if (!(save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn))) return false;
        path = file;
        return !path.empty();
    }
#endif

    // Open / save dialog of AIMP; false if cancelled or not available
    bool AskFile(bool save, tstring& path) {
        tstring captionText = Tr(save ? "Export settings" : "Import settings");
        tstring filterName = Trf("%1 settings", {AR_T(AR_PLUGIN_NAME)});
        // AIMP hands out the file dialogs through the UI service (like the message dialog)
        Ptr<IAIMPUIFileDialogs> dlg;
        if (ui_ && (Failed(ui_->QueryInterface(IID_IAIMPUIFileDialogs, dlg.OutV())) || !dlg))
            ui_->CreateObject(form_.Get(), nullptr, IID_IAIMPUIFileDialogs, dlg.OutV());
        if (!dlg) {
#ifdef _WIN32
            return AskFileWindows(save, captionText, filterName, path);
#else
            Log(AR_T("No file dialog in this AIMP version"));
            return false;
#endif
        }
        auto caption = MakeString(captionText);
        auto filter = MakeString(filterName + AR_T(" (*.ini)|*.ini"));
        HWND owner = form_->GetHandle();
        Ptr<IAIMPString> name;
        HRESULT hr;
        if (save) {
            IAIMPString* raw = nullptr;  // in: proposed file name, out: the chosen file (AIMP replaces it)
            auto proposal = MakeString(AR_T("PreventResampling-settings.ini"));
            if (proposal) { raw = proposal.Get(); raw->AddRef(); }
            INT32 filterIndex = 0;
            hr = dlg->ExecuteSaveDialog(owner, caption.Get(), filter.Get(), &raw, &filterIndex);
            *name.Out() = raw;
        } else {
            hr = dlg->ExecuteOpenDialog(owner, caption.Get(), filter.Get(), name.Out());
        }
        if (hr != S_OK || !name) return false;
        path = FromString(name.Get());
        if (save && !path.empty()) {
            // AIMP adds the filter's extension even if the name has it already ("x.ini.ini")
            while (path.size() > 8 && EqualsI(path.substr(path.size() - 8), AR_T(".ini.ini"))) path.erase(path.size() - 4);
            size_t dot = path.find_last_of(AR_T('.')), sep = path.find_last_of(AR_T("\\/"));
            if (dot == tstring::npos || (sep != tstring::npos && dot < sep)) path += AR_T(".ini");
        }
        return !path.empty();
    }

    // Writes the settings shown on the page to a file of the user's choice
    void ExportSettings() {
        tstring path;
        if (!AskFile(true, path)) return;
        Ini out;
        out.Create(path);
        bool ok = FromControls().Save(out);
        Log((ok ? AR_T("Settings exported to ") : AR_T("Settings could not be exported to ")) + path);
        ShowMessage(ok ? Trf("Settings exported to %1.", {path}) : Trf("Could not write %1.", {path}),
                    MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING), form_->GetHandle());
    }

    // Reads exported settings, shows and saves them (like OK in the dialog)
    void ImportSettings() {
        tstring path;
        if (!AskFile(false, path)) return;
        Ini in;
        in.Open(path);
        if (!in.Existed() || !in.HasSection(Config::Section())) {
            Log(AR_T("Settings import: no Prevent Resampling settings in ") + path);
            ShowMessage(Trf("%1 contains no Prevent Resampling settings.", {path}), MB_OK | MB_ICONWARNING, form_->GetHandle());
            return;
        }
        Config c = engine_->cfg;
        c.Load(in);
        ToControls(c);
        Save();
        ApplyLanguage(c.language, true);
        Log(AR_T("Settings imported from ") + path);
        ShowMessage(Trf("Settings imported from %1.", {path}), MB_OK | MB_ICONINFORMATION, form_->GetHandle());
    }

    // Compares COM identity (the sender may be a different interface pointer)
    static bool Same(IUnknown* a, IUnknown* b) {
        if (!a || !b) return false;
        if (a == b) return true;
        Ptr<IUnknown> ua, ub;
        a->QueryInterface(IID_IUnknown, ua.OutV());
        b->QueryInterface(IID_IUnknown, ub.OutV());
        return ua && ua.Get() == ub.Get();
    }

    void OnChanged(IUnknown* sender) {
        if (Same(sender, pages_.Get())) return;  // another tab was opened - nothing changed
        if (!loading_ && Same(sender, coAimpOut_.Get())) aimpOutChanged_ = true;
        if (Same(sender, btRefresh_.Get())) { RefreshStats(); return; }
        if (Same(sender, btTest_.Get())) { TestWindowsFormat(); RefreshStats(); return; }
        if (Same(sender, btCheck_.Get())) { engine_->updater.Check(true, false); return; }
        if (Same(sender, btInstall_.Get())) { engine_->updater.Install(); return; }
        if (Same(sender, btExport_.Get())) { ExportSettings(); return; }
        if (Same(sender, btImport_.Get())) { ImportSettings(); return; }
        if (Same(sender, btBpRestart_.Get())) { engine_->RestartForSettings(); return; }
        if (Same(sender, btReset_.Get())) {
            engine_->stats.ResetCounters();
            engine_->SaveStats();
            RefreshStats();
            return;
        }
        if (loading_) return;
        if (Same(sender, coLang_.Get())) {  // shown right away; stored with OK / Apply
            int l = Index(coLang_.Get());
            if (l >= 0 && l <= 4) ApplyLanguage((LanguageSetting)l);
        }
        UpdateDependencies();
        auto svc = Service<IAIMPServiceOptionsDialog>(IID_IAIMPServiceOptionsDialog);
        if (svc) svc->FrameModified(this);
    }

    // ------------------------------------------------------------------ IAIMPOptionsDialogFrame
    HRESULT WINAPI GetName(IAIMPString** S) override {
        auto s = MakeString(AR_T(AR_PLUGIN_NAME));
        if (!s) return E_FAIL;
        s->AddRef();
        *S = s.Get();
        return S_OK;
    }

    HWND WINAPI CreateFrame(HWND parent) override {
        ui_ = Service<IAIMPServiceUI>(IID_IAIMPServiceUI);
        if (!ui_) return (HWND)0;
        Ptr<IAIMPUIChangeEvents> ev;
        ChangeHandler* h = new ChangeHandler([this](IUnknown* s) { OnChanged(s); });
        h->AddRef();
        *ev.Out() = h;
        events_ = ev;
        auto name = MakeString(AR_T("PreventResamplingFrame"));
        if (Failed(ui_->CreateForm(parent, AIMPUI_SERVICE_CREATEFORM_FLAGS_CHILD, name.Get(), nullptr, form_.Out())) || !form_)
            return (HWND)0;
        form_->SetValueAsInt32(AIMPUI_FORM_PROPID_BORDERSTYLE, AIMPUI_FLAGS_BORDERSTYLE_NONE);
#ifdef _WIN32
        if (parent) {  // fill AIMP's frame area; AIMP resizes it afterwards together with the dialog
            RECT rc;
            GetClientRect(parent, &rc);
            TAIMPUIControlPlacement pl;
            memset(&pl, 0, sizeof(pl));
            pl.Alignment = ualNone;
            pl.Bounds = rc;
            form_->SetPlacement(pl);
        }
#endif
        CurrentLanguage() = ResolveLanguage(engine_->cfg.language);  // AIMP's language may have changed
        Build();
        Populate();
        ToControls(engine_->cfg);
        RefreshBitPerfect();
        RefreshStats();
        RefreshAbout();
        return form_->GetHandle();
    }

    void WINAPI DestroyFrame() override {
        // Release all references to controls first, then destroy the form
        cbEnabled_.Reset(); cbMultiples_.Reset(); cbRestore_.Reset(); cbAutoTiming_.Reset();
        cbRestart_.Reset(); cbSeamless_.Reset(); cbAsioForce_.Reset();
        cbBp_.Reset(); cbBpVolume_.Reset(); cbBpGapless_.Reset(); lbBpStatus_.Reset(); btBpRestart_.Reset();
        cbVm_.Reset(); cbVmEngine_.Reset(); cbVmAllBuses_.Reset(); cbVmAsio_.Reset();
        spSettle_.Reset(); coAimpOut_.Reset(); coMode_.Reset(); coWinDev_.Reset(); coAsio_.Reset(); coVmA1_.Reset();
        edExtra_.Reset(); lbVmStatus_.Reset(); memo_.Reset(); btRefresh_.Reset(); btReset_.Reset(); btTest_.Reset();
        cbUpdate_.Reset(); cbAutoInstall_.Reset(); coInterval_.Reset(); btCheck_.Reset(); btInstall_.Reset();
        lbUpdate_.Reset(); lnNotes_.Reset(); memoChangelog_.Reset();
        coLang_.Reset(); btExport_.Reset(); btImport_.Reset();
        texts_.clear();
        lastChangelog_.clear();
        pages_.Reset();
        containers_.clear();
        lastStats_.clear();
        if (form_) form_->Destroy(0);
        form_.Reset();
        events_.Reset();
        ui_.Reset();
    }

    void WINAPI Notification(INT32 id) override {
        if (!form_) return;
        switch (id) {
            case AIMP_SERVICE_OPTIONSDIALOG_NOTIFICATION_LOAD: ToControls(engine_->cfg); ApplyLanguage(engine_->cfg.language); break;
            case AIMP_SERVICE_OPTIONSDIALOG_NOTIFICATION_SAVE: Save(); break;
            case AIMP_SERVICE_OPTIONSDIALOG_NOTIFICATION_RESET: ToControls(Config()); ApplyLanguage(Config().language); break;
        }
    }
};

}  // namespace ar
