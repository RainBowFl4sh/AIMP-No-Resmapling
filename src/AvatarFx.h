// About tab: the author's picture with rounded corners, as in the Discord Rich Presence plugin (preview.cpp
// there): a click on the head says "pat", one on the nose "boop!" - the word
// with a pink heart rises from the spot and fades. Windows draws with GDI+, Linux with cairo (the canvas
// AIMP for Linux passes is a cairo_t*). Coordinates are design units of the 64 x 64 picture.
#pragma once
#include "Common.h"
#include "apiObjects.h"

#ifdef _WIN32
#include <objidl.h>
using std::max;  // gdiplus.h expects these when NOMINMAX is defined
using std::min;
#include <gdiplus.h>
#endif

#include <cmath>

namespace ar {

// Areas as parts of the picture (head, nose with some room around it, mostly above; they do not overlap,
// anywhere else nothing happens). The word rises from (ax, ay) by 'rise' and fades.
struct AvatarSpot {
    float x0, y0, x1, y1;
    const TChar* text;
    float ax, ay, rise;
};
static const AvatarSpot kAvatarSpots[] = {{0.22f, 0.075f, 0.74f, 0.36f, AR_T("pat"), 0.48f, 0.30f, 10},
                                          {0.30f, 0.44f, 0.66f, 0.75f, AR_T("boop!"), 0.485f, 0.64f, 16}};
static const int64_t kAvatarFxMs = 1100;  // length of the animation

namespace fx {

const float kPi = 3.14159265f;
// a heart 1 unit wide around (0, 0): start point, then four Bezier curves (control, control, end)
const float kHeart[13][2] = {{0, -0.22f}, {0, -0.42f}, {-0.5f, -0.47f}, {-0.5f, -0.12f}, {-0.5f, 0.12f},
                             {-0.22f, 0.28f}, {0, 0.48f}, {0.22f, 0.28f}, {0.5f, 0.12f}, {0.5f, -0.12f},
                             {0.5f, -0.47f}, {0, -0.42f}, {0, -0.22f}};

#ifdef _WIN32

// GDI+ is started with the first picture and shut down in Finalize (never in DllMain)
inline ULONG_PTR& GdipToken() {
    static ULONG_PTR t = 0;
    return t;
}
inline bool EnsureGdip() {
    if (GdipToken()) return true;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&GdipToken(), &in, nullptr) != Gdiplus::Ok) GdipToken() = 0;
    return GdipToken() != 0;
}
inline void ShutdownGdip() {
    if (GdipToken()) Gdiplus::GdiplusShutdown(GdipToken());
    GdipToken() = 0;
}

inline void AddRound(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    float d = r * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

class Painter {
public:
    Painter(HCANVAS dc, const RECT& r, float designW) : g_(dc) {
        s_ = (r.right - r.left) / designW;
        ox_ = (float)r.left;
        oy_ = (float)r.top;
        g_.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g_.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g_.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
        ok_ = g_.GetLastStatus() == Gdiplus::Ok;
    }
    ~Painter() { delete family_; }
    bool Valid() const { return ok_; }
    void SetAlpha(float a) { alpha_ = (BYTE)lroundf(255 * (std::max)(0.0f, (std::min)(1.0f, a))); }  // for what follows
    void Smooth() {}  // smooth edges: already (see above)

    // bold text with an outline, and a heart: each one path, so they fade evenly with SetAlpha
    float BoldWidth(const tstring& t, float size) {
        Gdiplus::Font f(Family(), size * s_, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::RectF box;
        g_.MeasureString(t.c_str(), (INT)t.size(), &f, Gdiplus::PointF(0, 0), Gdiplus::StringFormat::GenericTypographic(), &box);
        return box.Width / s_;
    }
    void Outlined(const tstring& t, float x, float y, float size, uint32_t rgb, uint32_t line, float width) {
        Gdiplus::GraphicsPath p;
        p.AddString(t.c_str(), (INT)t.size(), Family(), Gdiplus::FontStyleBold, size * s_, Gdiplus::PointF(X(x), Y(y)),
                    Gdiplus::StringFormat::GenericTypographic());
        Gdiplus::Pen pen(Col(line), width * s_);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        g_.DrawPath(&pen, &p);
        Gdiplus::SolidBrush b(Col(rgb));
        g_.FillPath(&b, &p);
    }
    void Heart(float cx, float cy, float size, uint32_t rgb) {
        Gdiplus::PointF pt[13];
        for (int i = 0; i < 13; ++i) pt[i] = Gdiplus::PointF(X(cx + kHeart[i][0] * size), Y(cy + kHeart[i][1] * size));
        Gdiplus::GraphicsPath p;
        p.AddBeziers(pt, 13);
        Gdiplus::SolidBrush b(Col(rgb));
        g_.FillPath(&b, &p);
    }

private:
    Gdiplus::Color Col(uint32_t rgb) const { return Gdiplus::Color(alpha_, (BYTE)(rgb >> 16), (BYTE)(rgb >> 8), (BYTE)rgb); }
    float X(float x) const { return ox_ + x * s_; }
    float Y(float y) const { return oy_ + y * s_; }
    // by name (GDI+'s cached generic families would outlive a GdiplusShutdown in Finalize)
    const Gdiplus::FontFamily* Family() {
        for (const wchar_t* name : {L"Segoe UI", L"Tahoma", L"Arial"}) {
            if (family_ && family_->IsAvailable()) break;
            delete family_;
            family_ = new Gdiplus::FontFamily(name);
        }
        return family_;
    }

    Gdiplus::Graphics g_;
    float s_ = 1, ox_ = 0, oy_ = 0;
    BYTE alpha_ = 255;
    bool ok_ = false;
    Gdiplus::FontFamily* family_ = nullptr;
};

#else  // Linux: cairo

class Painter {
public:
    Painter(HCANVAS cr, const RECT& r, float designW) : cr_(cr) {
        s_ = (r.right - r.left) / designW;
        cairo_save(cr_);
        cairo_translate(cr_, r.left, r.top);
        cairo_scale(cr_, s_, s_);
    }
    ~Painter() { cairo_restore(cr_); }
    bool Valid() const { return cr_ != nullptr; }
    void SetAlpha(float a) { alpha_ = (std::max)(0.0f, (std::min)(1.0f, a)); }  // for what follows
    void Smooth() { cairo_set_antialias(cr_, CAIRO_ANTIALIAS_GRAY); }  // AIMP's canvas has hard edges (restored at the end)

    float BoldWidth(const tstring& t, float size) {
        Font(size);
        cairo_text_extents_t e;
        cairo_text_extents(cr_, t.c_str(), &e);
        return (float)e.x_advance;
    }
    void Outlined(const tstring& t, float x, float y, float size, uint32_t rgb, uint32_t line, float width) {
        Font(size);
        cairo_font_extents_t fe;
        cairo_font_extents(cr_, &fe);
        cairo_new_path(cr_);
        cairo_move_to(cr_, x, y + fe.ascent);
        cairo_text_path(cr_, t.c_str());
        Source(line);
        cairo_set_line_width(cr_, width);
        cairo_set_line_join(cr_, CAIRO_LINE_JOIN_ROUND);
        cairo_stroke_preserve(cr_);
        Fill(rgb);
    }
    void Heart(float cx, float cy, float size, uint32_t rgb) {
        cairo_new_path(cr_);
        cairo_move_to(cr_, cx + kHeart[0][0] * size, cy + kHeart[0][1] * size);
        for (int i = 1; i < 13; i += 3)
            cairo_curve_to(cr_, cx + kHeart[i][0] * size, cy + kHeart[i][1] * size, cx + kHeart[i + 1][0] * size,
                           cy + kHeart[i + 1][1] * size, cx + kHeart[i + 2][0] * size, cy + kHeart[i + 2][1] * size);
        cairo_close_path(cr_);
        Fill(rgb);
    }

private:
    void Source(uint32_t rgb) {
        cairo_set_source_rgba(cr_, ((rgb >> 16) & 255) / 255.0, ((rgb >> 8) & 255) / 255.0, (rgb & 255) / 255.0, alpha_);
    }
    void Fill(uint32_t rgb) {
        Source(rgb);
        cairo_fill(cr_);
    }
    void Font(float size) {
        cairo_select_font_face(cr_, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
        cairo_set_font_size(cr_, size);
    }

    cairo_t* cr_;
    float s_ = 1, alpha_ = 1;
};

#endif

}  // namespace fx

// The picture in r with rounded corners (radius in units of a 64 px wide picture)
inline void DrawRoundedImage(HCANVAS canvas, const RECT& r, IAIMPImage* img, float radius) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (!canvas || !img || w <= 0 || h <= 0) return;
    const float rad = radius * w / 64.0f;
#ifdef _WIN32
    if (!fx::EnsureGdip()) return;
    // AIMP draws the picture into a bitmap, GDI+ fills the rounded shape with it (smooth edges)
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(canvas);
    HBITMAP bmp = mem ? CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (bmp && bits) {
        HGDIOBJ old = SelectObject(mem, bmp);
        RECT rc = {0, 0, w, h};
        img->Draw(mem, rc, AIMP_IMAGE_DRAW_STRETCHMODE_FILL | AIMP_IMAGE_DRAW_QUALITY_HIGH, nullptr);
        GdiFlush();
        {
            Gdiplus::Bitmap pic(w, h, w * 4, PixelFormat32bppRGB, static_cast<BYTE*>(bits));
            Gdiplus::Graphics g(canvas);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            Gdiplus::TextureBrush brush(&pic);
            brush.TranslateTransform((Gdiplus::REAL)r.left, (Gdiplus::REAL)r.top);
            Gdiplus::GraphicsPath path;
            fx::AddRound(path, (Gdiplus::REAL)r.left, (Gdiplus::REAL)r.top, (Gdiplus::REAL)w, (Gdiplus::REAL)h, rad);
            g.FillPath(&brush, &path);
        }
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
#else
    cairo_t* cr = canvas;
    cairo_save(cr);
    cairo_new_path(cr);
    const double x = r.left, y = r.top;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - rad, y + rad, rad, -fx::kPi / 2, 0);
    cairo_arc(cr, x + w - rad, y + h - rad, rad, 0, fx::kPi / 2);
    cairo_arc(cr, x + rad, y + h - rad, rad, fx::kPi / 2, fx::kPi);
    cairo_arc(cr, x + rad, y + rad, rad, fx::kPi, 3 * fx::kPi / 2);
    cairo_close_path(cr);
    cairo_clip(cr);
    img->Draw(cr, r, AIMP_IMAGE_DRAW_STRETCHMODE_FILL | AIMP_IMAGE_DRAW_QUALITY_HIGH, nullptr);
    cairo_restore(cr);
#endif
}

// "boop!" / "pat" over the picture in r: t = 0..1 of the animation
inline void DrawAvatarFx(HCANVAS canvas, const RECT& r, const tstring& text, float ax, float ay, float rise, float t) {
#ifdef _WIN32
    if (!fx::EnsureGdip()) return;
#endif
    if (!canvas || r.right <= r.left || r.bottom <= r.top || t < 0 || t >= 1) return;
    fx::Painter p(canvas, r, 64);
    if (!p.Valid()) return;
    p.Smooth();
    const float size = 9, w = p.BoldWidth(text, size), heart = 5.5f, gap = 1.5f, all = w + gap + heart;
    const float lift = rise * (1 - (1 - t) * (1 - t));  // fast at first, then slower
    const float x = (std::max)(2.0f, (std::min)(64 - 2 - all, ax * 64 - all / 2));
    const float y = ay * 64 - size * 0.72f - lift;  // top of the line: the letters are about centred on ay
    p.SetAlpha(1 - t * t);                           // fades out towards the end
    p.Outlined(text, x, y, size, 0xffffff, 0x3c145a, 1.5f);
    p.Heart(x + w + gap + heart / 2, y + size * 0.3f, heart, 0xff5c96);
}

}  // namespace ar
