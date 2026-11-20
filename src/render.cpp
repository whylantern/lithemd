#include "render.h"
#include "latex.h"
#include "i18n.h"
#include <d2d1helper.h>
#include <shlwapi.h>
#include <objbase.h>
#include <cmath>
#include <algorithm>
#include <atomic>

using Microsoft::WRL::ComPtr;

Engine& Engine::Instance() {
    static Engine e;
    return e;
}

bool Engine::Init() {
    if (init_) return true;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())))) return false;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic_)))) return false;
    init_ = true;
    return true;
}

Color RoleColor(const Palette& p, InlineRole role) {
    switch (role) {
    case InlineRole::Muted: return p.muted;
    case InlineRole::Accent: return p.accent;
    case InlineRole::Danger: return p.danger;
    case InlineRole::CodeKeyword: return p.codeKeyword;
    case InlineRole::CodeString: return p.codeString;
    case InlineRole::CodeNumber: return p.codeNumber;
    case InlineRole::CodeComment: return p.codeComment;
    case InlineRole::CodeType: return p.codeType;
    case InlineRole::CodePunct: return p.codePunct;
    case InlineRole::Success: return p.dark ? HexColor(L"#3FB950") : HexColor(L"#1A7F37");
    default: return p.fg;
    }
}

D2D1_COLOR_F ToD2D(const Color& c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }

std::wstring FontFamilyBody() { return L"Microsoft YaHei UI"; }
std::wstring FontFamilyMono() { return L"Consolas"; }
std::wstring FontFamilyIcon() { return L"Segoe MDL2 Assets"; }

namespace {

ComPtr<IDWriteTextFormat> MakeFormat(float size, bool bold, bool italic, bool mono,
                                     DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT valign,
                                     bool wrap) {
    auto& eng = Engine::Instance();
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(eng.DWrite()->CreateTextFormat(mono ? FontFamilyMono().c_str() : FontFamilyBody().c_str(),
            nullptr, bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
            italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN", &fmt))) return nullptr;
    fmt->SetTextAlignment(align);
    fmt->SetParagraphAlignment(valign);
    fmt->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    return fmt;
}

struct MathInline : IDWriteInlineObject {
    virtual ~MathInline() = default;
    std::shared_ptr<MathNode> node;
    float fontSize = 16.0f;
    InlineRole role = InlineRole::Text;
    MathBox box;
    bool hasBox = false;

    std::atomic<ULONG> refs{ 1 };

    MathInline(std::shared_ptr<MathNode> n, float fs, InlineRole r)
        : node(std::move(n)), fontSize(fs), role(r) {}

    IFACEMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteInlineObject)) {
            *ppv = static_cast<IDWriteInlineObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHOD_(ULONG, AddRef)() override { return ++refs; }
    IFACEMETHOD_(ULONG, Release)() override {
        ULONG n = --refs;
        if (n == 0) delete this;
        return n;
    }

    IFACEMETHOD(Draw)(void* clientDrawingContext, IDWriteTextRenderer* renderer,
                      FLOAT originX, FLOAT originY, BOOL isSideways, BOOL isRightToLeft,
                      IUnknown* clientDrawingEffect) override;
    IFACEMETHOD(GetMetrics)(DWRITE_INLINE_OBJECT_METRICS* metrics) override {
        if (!metrics) return E_POINTER;
        if (!hasBox) { box = MeasureMath(*node, Engine::Instance().DWrite(), fontSize); hasBox = true; }
        metrics->width = box.width;
        metrics->height = box.height();
        metrics->baseline = box.ascent;
        metrics->supportsSideways = FALSE;
        return S_OK;
    }
    IFACEMETHOD(GetOverhangMetrics)(DWRITE_OVERHANG_METRICS* overhangs) override {
        if (!overhangs) return E_POINTER;
        overhangs->left = overhangs->right = overhangs->top = overhangs->bottom = 0;
        return S_OK;
    }
    IFACEMETHOD(GetBreakConditions)(DWRITE_BREAK_CONDITION* before, DWRITE_BREAK_CONDITION* after) override {
        if (before) *before = DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;
        if (after) *after = DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;
        return S_OK;
    }
};

} // namespace

struct DocTextRenderer : IDWriteTextRenderer {
    Canvas* canvas = nullptr;
    float ox = 0, oy = 0;
    std::wstring defaultUrl;

    IFACEMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteTextRenderer) ||
            riid == __uuidof(IDWritePixelSnapping)) {
            *ppv = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHOD_(ULONG, AddRef)() override { return 1; }
    IFACEMETHOD_(ULONG, Release)() override { return 1; }

    IFACEMETHOD(IsPixelSnappingDisabled)(void*, BOOL* disabled) override {
        if (disabled) *disabled = FALSE;
        return S_OK;
    }
    IFACEMETHOD(GetCurrentTransform)(void*, DWRITE_MATRIX* matrix) override {
        if (!matrix) return E_POINTER;
        memset(matrix, 0, sizeof(*matrix));
        matrix->m11 = 1; matrix->m22 = 1;
        return S_OK;
    }
    IFACEMETHOD(GetPixelsPerDip)(void*, FLOAT* ppd) override {
        if (ppd) *ppd = canvas ? canvas->scale() : 1.0f;
        return S_OK;
    }

    Color EffectColor(const IUnknown* effect) const {
        auto* e = static_cast<const TextEffect*>(effect);
        return RoleColor(canvas->palette(), e ? e->role : InlineRole::Text);
    }

    IFACEMETHOD(DrawGlyphRun)(void* clientDrawingContext, FLOAT baselineOriginX, FLOAT baselineOriginY,
                              DWRITE_MEASURING_MODE measuringMode, const DWRITE_GLYPH_RUN* glyphRun,
                              const DWRITE_GLYPH_RUN_DESCRIPTION* glyphRunDescription,
                              IUnknown* clientDrawingEffect) override {
        (void)clientDrawingContext;
        (void)glyphRunDescription;
        if (!canvas || !canvas->valid()) return S_OK;
        auto* eff = static_cast<const TextEffect*>(clientDrawingEffect);
        const Color& color = EffectColor(clientDrawingEffect);
        if (eff && (eff->inlineCode || eff->highlight)) {
            float width = 0;
            if (glyphRun->glyphAdvances) for (UINT32 i = 0; i < glyphRun->glyphCount; i++) width += glyphRun->glyphAdvances[i];
            else width = glyphRun->fontEmSize * 0.55f * glyphRun->glyphCount;
            float top = baselineOriginY + oy - glyphRun->fontEmSize * 0.92f;
            float bottom = baselineOriginY + oy + glyphRun->fontEmSize * 0.24f;
            Color bg = eff->inlineCode ? canvas->palette().codeBg : HexColor(L"#F2C94C");
            if (eff->highlight) bg.a = canvas->palette().dark ? 0.34f : 0.42f;
            canvas->FillRound(baselineOriginX + ox - 2, top, width + 4, bottom - top, 3.0f, bg);
        }
        ComPtr<ID2D1SolidColorBrush> brush = canvas->Brush(color);
        if (brush)
            canvas->target()->DrawGlyphRun(D2D1::Point2F(baselineOriginX + ox, baselineOriginY + oy),
                                           glyphRun, brush.Get(), measuringMode);
        return S_OK;
    }

    IFACEMETHOD(DrawUnderline)(void*, FLOAT baselineOriginX, FLOAT baselineOriginY,
                               const DWRITE_UNDERLINE* underline, IUnknown* clientDrawingEffect) override {
        if (!canvas || !canvas->valid() || !underline) return S_OK;
        ComPtr<ID2D1SolidColorBrush> brush = canvas->Brush(EffectColor(clientDrawingEffect));
        if (brush) {
            float y = baselineOriginY + oy + underline->offset;
            canvas->target()->DrawLine(D2D1::Point2F(baselineOriginX + ox, y),
                                       D2D1::Point2F(baselineOriginX + ox + underline->width, y),
                                       brush.Get(), underline->thickness > 0 ? underline->thickness : 1.0f);
        }
        return S_OK;
    }
    IFACEMETHOD(DrawStrikethrough)(void*, FLOAT baselineOriginX, FLOAT baselineOriginY,
                                   const DWRITE_STRIKETHROUGH* strike, IUnknown* clientDrawingEffect) override {
        if (!canvas || !canvas->valid() || !strike) return S_OK;
        ComPtr<ID2D1SolidColorBrush> brush = canvas->Brush(EffectColor(clientDrawingEffect));
        if (brush) {
            float y = baselineOriginY + oy + strike->offset;
            canvas->target()->DrawLine(D2D1::Point2F(baselineOriginX + ox, y),
                                       D2D1::Point2F(baselineOriginX + ox + strike->width, y),
                                       brush.Get(), strike->thickness > 0 ? strike->thickness : 1.0f);
        }
        return S_OK;
    }
    IFACEMETHOD(DrawInlineObject)(void* clientDrawingContext, FLOAT originX, FLOAT originY,
                                  IDWriteInlineObject* inlineObject, BOOL isSideways, BOOL isRightToLeft,
                                  IUnknown* clientDrawingEffect) override {
        if (!inlineObject) return S_OK;
        return inlineObject->Draw(clientDrawingContext, this, originX, originY, isSideways,
                                  isRightToLeft, clientDrawingEffect);
    }
};

IFACEMETHODIMP MathInline::Draw(void* clientDrawingContext, IDWriteTextRenderer* renderer,
                                FLOAT originX, FLOAT originY, BOOL, BOOL, IUnknown*) {
    auto* r = static_cast<DocTextRenderer*>(clientDrawingContext);
    if (!r || !r->canvas || !r->canvas->valid()) return S_OK;
    if (!hasBox) { box = MeasureMath(*node, Engine::Instance().DWrite(), fontSize); hasBox = true; }
    const Color& c = RoleColor(r->canvas->palette(), role);
    DrawMath(*node, r->canvas->target(), Engine::Instance().DWrite(),
             originX + r->ox, originY + r->oy + box.ascent, fontSize, c);
    (void)renderer;
    return S_OK;
}

std::wstring RichText::LinkAt(size_t textPos) const {
    for (auto& l : links) {
        if (textPos >= l.start && textPos < l.start + l.length) return l.url;
    }
    return L"";
}

void RichText::Draw(Canvas& canvas, float x, float y) const {
    if (!layout || !canvas.valid()) return;
    DocTextRenderer renderer;
    renderer.canvas = &canvas;
    renderer.ox = x;
    renderer.oy = y;
    layout->Draw(&renderer, &renderer, 0, 0);
}

bool BuildRichText(const std::vector<Inline>& inlines, const TextStyleOptions& opt, RichText& out) {
    out = RichText{};
    auto& eng = Engine::Instance();
    std::wstring text;
    struct Range {
        UINT32 start, len;
        bool bold, italic, underline, strike, code, link, highlight;
        InlineRole role;
        std::wstring url;
        std::shared_ptr<MathNode> math;
    };
    std::vector<Range> ranges;
    for (auto& in : inlines) {
        Range r;
        r.start = (UINT32)text.size();
        r.bold = in.bold || opt.bold;
        r.italic = in.italic || opt.italic;
        r.underline = in.underline || opt.underline;
        r.strike = in.strike;
        r.code = in.code;
        r.link = in.link;
        r.highlight = in.highlight;
        r.role = in.role;
        r.url = in.url;
        r.math = in.math;
        if (in.math) {
            text += (wchar_t)0xFFFC;
            r.len = 1;
        } else {
            text += in.text;
            r.len = (UINT32)in.text.size();
        }
        if (r.len) ranges.push_back(std::move(r));
    }
    if (text.empty()) { out = RichText{}; return false; }
    float maxW = opt.wrap ? std::max(1.0f, opt.maxWidth) : 1000000.0f;
    auto fmt = MakeFormat(opt.fontSize, opt.bold, opt.italic, opt.mono, opt.align, opt.valign, opt.wrap);
    if (!fmt) return false;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(eng.DWrite()->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt.Get(), maxW, 1000000.0f, &layout)))
        return false;
    DWRITE_LINE_SPACING_METHOD method = DWRITE_LINE_SPACING_METHOD_UNIFORM;
    float line = opt.fontSize * std::max(1.0f, opt.lineHeight);
    float base = opt.fontSize * opt.baseline;
    if (base > line * 0.8f) base = line * 0.8f;
    layout->SetLineSpacing(method, line, base);
    layout->SetTextAlignment(opt.align);

    std::wstring mono = FontFamilyMono();
    std::wstring body = FontFamilyBody();
    for (auto& r : ranges) {
        DWRITE_TEXT_RANGE tr{r.start, r.len};
        const std::wstring& fam = (r.code ? mono : body);
        layout->SetFontFamilyName(fam.c_str(), tr);
        layout->SetFontSize(opt.fontSize * (r.code ? 0.92f : 1.0f), tr);
        layout->SetFontWeight(r.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, tr);
        layout->SetFontStyle(r.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, tr);
        if (r.underline) layout->SetUnderline(TRUE, tr);
        if (r.strike) layout->SetStrikethrough(TRUE, tr);
        if (r.math) {
            ComPtr<IDWriteInlineObject> obj;
            obj.Attach(new MathInline(r.math, opt.fontSize * 1.05f, r.role));
            layout->SetInlineObject(obj.Get(), tr);
            out.math.push_back(obj);
        }
        auto* eff = new TextEffect();
        eff->role = r.role == InlineRole::Text ? opt.role : r.role;
        if (r.link) {
            eff->role = InlineRole::Accent;
            eff->link = true;
            eff->url = r.url;
            out.links.push_back({ (size_t)r.start, (size_t)r.len, r.url });
        }
        eff->inlineCode = r.code;
        eff->highlight = r.highlight;
        layout->SetDrawingEffect(eff, tr);
        ComPtr<TextEffect> holder;
        holder.Attach(eff);
        out.effects.push_back(holder);
    }
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    out.layout = layout;
    out.text = std::move(text);
    out.width = opt.wrap ? tm.width : tm.widthIncludingTrailingWhitespace;
    out.height = tm.height;
    out.baseline = tm.height;
    return true;
}

bool BuildPlainRichText(const std::wstring& text, const TextStyleOptions& opt, RichText& out) {
    Inline in;
    in.text = text;
    return BuildRichText({ in }, opt, out);
}

Canvas::~Canvas() = default;

namespace {

bool EncodePng(IWICImagingFactory* wic, IWICBitmap* bmp, std::vector<uint8_t>& out) {
    if (!wic || !bmp) return false;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return false;
    if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(encoder->CreateNewFrame(&frame, &props))) return false;
    if (FAILED(frame->Initialize(props.Get()))) return false;
    UINT w = 0, h = 0;
    if (FAILED(bmp->GetSize(&w, &h)) || !w || !h) return false;
    if (FAILED(frame->SetSize(w, h))) return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&fmt))) return false;
    if (FAILED(frame->WriteSource(bmp, nullptr))) return false;
    if (FAILED(frame->Commit())) return false;
    if (FAILED(encoder->Commit())) return false;
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME))) return false;
    LARGE_INTEGER zero{};
    if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr))) return false;
    size_t size = (size_t)stat.cbSize.QuadPart;
    out.resize(size);
    size_t done = 0;
    while (done < size) {
        ULONG got = 0;
        HRESULT hr = stream->Read(out.data() + done, (ULONG)std::min<size_t>(size - done, 1u << 20), &got);
        if (FAILED(hr) || got == 0) break;
        done += got;
    }
    out.resize(done);
    return !out.empty();
}

} // namespace

bool Canvas::Begin(HWND hwnd, const Palette& palette, bool clear) {
    if (!Engine::Instance().Init()) return false;
    hwnd_ = hwnd;
    palette_ = &palette;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    UINT w = (UINT)std::max(0L, rc.right - rc.left);
    UINT h = (UINT)std::max(0L, rc.bottom - rc.top);
    if (w == 0 || h == 0) return false;
    UINT dpi = GetDpiForWindow(hwnd);
    if (!dpi) dpi = 96;
    float newScale = dpi / 96.0f;
    if (hwndTarget_ && (w != widthPx_ || h != heightPx_)) {
        hwndTarget_->Resize(D2D1::SizeU(w, h));
        widthPx_ = w;
        heightPx_ = h;
    }
    if (!hwndTarget_ || std::abs(newScale - scale_) > 0.001f) {
        if (target_ || hwndTarget_ || wicBitmap_) Discard();
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi, dpi,
            D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
        D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(
            hwnd, D2D1::SizeU(w, h), D2D1_PRESENT_OPTIONS_NONE);
        if (FAILED(Engine::Instance().D2D()->CreateHwndRenderTarget(props, hp, &hwndTarget_))) {
            hwndTarget_.Reset();
            target_.Reset();
            return false;
        }
        target_ = hwndTarget_;
        scale_ = newScale;
    }
    widthPx_ = w;
    heightPx_ = h;
    target_->SetDpi(dpi, dpi);
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    if (clear) target_->Clear(ToD2D(palette.bg));
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    drawing_ = true;
    return true;
}

bool Canvas::BeginBitmap(UINT widthPx, UINT heightPx, float dpi, const Palette& palette, bool clear) {
    if (!Engine::Instance().Init() || !widthPx || !heightPx) return false;
    if (dpi <= 1.0f) dpi = 96.0f;
    if (target_ || hwndTarget_ || wicBitmap_) Discard();
    ComPtr<IWICBitmap> bmp;
    if (FAILED(Engine::Instance().WIC()->CreateBitmap(
            widthPx, heightPx, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp)))
        return false;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        dpi, dpi, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    if (FAILED(Engine::Instance().D2D()->CreateWicBitmapRenderTarget(bmp.Get(), props, &target_)))
        return false;
    hwnd_ = nullptr;
    wicBitmap_ = bmp;
    palette_ = &palette;
    scale_ = dpi / 96.0f;
    widthPx_ = widthPx;
    heightPx_ = heightPx;
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    if (clear) target_->Clear(ToD2D(palette.bg));
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    drawing_ = true;
    return true;
}

bool Canvas::EndBitmap(std::vector<uint8_t>& png) {
    if (!target_ || !wicBitmap_) return false;
    End();
    bool ok = EncodePng(Engine::Instance().WIC(), wicBitmap_.Get(), png);
    Discard();
    return ok;
}

void Canvas::End() {
    if (!target_ || !drawing_) return;
    drawing_ = false;
    HRESULT hr = target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) Discard();
}

void Canvas::Discard() {
    images_.clear();
    brushes_.clear();
    target_.Reset();
    hwndTarget_.Reset();
    wicBitmap_.Reset();
    drawing_ = false;
}

void Canvas::Resize(UINT w, UINT h) {
    if (hwndTarget_ && w && h) {
        hwndTarget_->Resize(D2D1::SizeU(w, h));
        widthPx_ = w;
        heightPx_ = h;
    }
}

ID2D1SolidColorBrush* Canvas::Brush(const Color& color) {
    if (!target_) return nullptr;
    uint32_t key = ((uint32_t)(color.r * 255) << 24) | ((uint32_t)(color.g * 255) << 16) |
                   ((uint32_t)(color.b * 255) << 8) | (uint32_t)(color.a * 255);
    auto it = brushes_.find(key);
    if (it != brushes_.end() && it->second) return it->second.Get();
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(target_->CreateSolidColorBrush(ToD2D(color), &brush))) return nullptr;
    brushes_[key] = brush;
    return brush.Get();
}

namespace {

constexpr uint64_t kMaxImagePixels = 100ull * 1000ull * 1000ull;

ComPtr<IWICFormatConverter> DecodeWic(const ImageRef& img) {
    auto& eng = Engine::Instance();
    if (!eng.Init()) return nullptr;
    ComPtr<IStream> stream;
    if (!img.bytes.empty()) {
        stream.Attach(SHCreateMemStream((const BYTE*)img.bytes.data(), (UINT)img.bytes.size()));
    } else if (!img.path.empty() && !IsWebUrl(img.path)) {
        SHCreateStreamOnFileEx(img.path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE,
                               FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, stream.GetAddressOf());
    }
    if (!stream) return nullptr;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(eng.WIC()->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)))
        return nullptr;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(eng.WIC()->CreateFormatConverter(&conv))) return nullptr;
    if (FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                nullptr, 0.0, WICBitmapPaletteTypeCustom))) return nullptr;
    return conv;
}

std::map<std::wstring, std::pair<float, float>>& SizeCache() {
    static std::map<std::wstring, std::pair<float, float>> cache;
    return cache;
}

} // namespace

bool ImagePixelSize(const ImageRef& img, float& w, float& h) {
    std::wstring key = img.cacheKey.empty() ? img.path : img.cacheKey;
    auto it = SizeCache().find(key);
    if (it != SizeCache().end()) { w = it->second.first; h = it->second.second; return w > 0 && h > 0; }
    auto conv = DecodeWic(img);
    if (!conv) return false;
    UINT pw = 0, ph = 0;
    if (FAILED(conv->GetSize(&pw, &ph)) || !pw || !ph) return false;
    if ((uint64_t)pw * (uint64_t)ph > kMaxImagePixels) return false;
    w = (float)pw;
    h = (float)ph;
    SizeCache()[key] = { w, h };
    return true;
}

ID2D1Bitmap* Canvas::Image(const ImageRef& img) {
    if (!target_) return nullptr;
    std::wstring key = img.cacheKey.empty() ? img.path : img.cacheKey;
    if (key.empty()) key = L"data:" + std::to_wstring(img.bytes.size());
    auto it = images_.find(key);
    if (it != images_.end() && it->second) return it->second.Get();
    auto conv = DecodeWic(img);
    if (!conv) return nullptr;
    UINT pw = 0, ph = 0;
    if (FAILED(conv->GetSize(&pw, &ph)) || !pw || !ph) return nullptr;
    if ((uint64_t)pw * (uint64_t)ph > kMaxImagePixels) return nullptr;
    ComPtr<ID2D1Bitmap> bmp;
    if (FAILED(target_->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp))) return nullptr;
    images_[key] = bmp;
    return bmp.Get();
}

void Canvas::FillRect(float x, float y, float w, float h, const Color& c) {
    if (!target_ || w <= 0 || h <= 0) return;
    auto* b = Brush(c);
    if (b) target_->FillRectangle(D2D1::RectF(x, y, x + w, y + h), b);
}
void Canvas::FillPolygon(const D2D1_POINT_2F* points, size_t count, const Color& c) {
    if (!target_ || !points || count < 3) return;
    auto* b = Brush(c);
    if (!b) return;
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(Engine::Instance().D2D()->CreatePathGeometry(&geometry))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink))) return;
    sink->BeginFigure(points[0], D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(points + 1, (UINT32)(count - 1));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    sink->Close();
    target_->FillGeometry(geometry.Get(), b);
}
void Canvas::FillRound(float x, float y, float w, float h, float r, const Color& c) {
    if (!target_ || w <= 0 || h <= 0) return;
    auto* b = Brush(c);
    if (b) target_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), b);
}
void Canvas::StrokeRect(float x, float y, float w, float h, const Color& c, float t) {
    if (!target_) return;
    auto* b = Brush(c);
    if (b) target_->DrawRectangle(D2D1::RectF(x, y, x + w, y + h), b, t);
}
void Canvas::StrokeRound(float x, float y, float w, float h, float r, const Color& c, float t) {
    if (!target_) return;
    auto* b = Brush(c);
    if (b) target_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), b, t);
}
void Canvas::DrawEllipse(float cx, float cy, float rx, float ry, const Color& c, float t) {
    if (!target_ || rx <= 0 || ry <= 0) return;
    auto* b = Brush(c);
    if (b) target_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), b, t);
}
void Canvas::DrawLine(float x1, float y1, float x2, float y2, const Color& c, float t) {
    if (!target_) return;
    auto* b = Brush(c);
    if (b) target_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), b, t);
}
void Canvas::DrawStrokePath(const D2D1_POINT_2F* points, size_t count, const Color& c,
                            float thickness, bool round) {
    if (!target_ || !points || count == 0) return;
    auto* b = Brush(c);
    if (!b) return;
    if (count == 1) {
        target_->FillEllipse(D2D1::Ellipse(points[0], thickness / 2, thickness / 2), b);
        return;
    }
    ComPtr<ID2D1PathGeometry> geometry;
    if (FAILED(Engine::Instance().D2D()->CreatePathGeometry(&geometry))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink))) return;
    sink->BeginFigure(points[0], D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLines(points + 1, (UINT32)(count - 1));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    ComPtr<ID2D1StrokeStyle> style;
    if (round) {
        D2D1_STROKE_STYLE_PROPERTIES props{};
        props.startCap = D2D1_CAP_STYLE_ROUND;
        props.endCap = D2D1_CAP_STYLE_ROUND;
        props.dashCap = D2D1_CAP_STYLE_ROUND;
        props.lineJoin = D2D1_LINE_JOIN_ROUND;
        props.miterLimit = 1.0f;
        props.dashStyle = D2D1_DASH_STYLE_SOLID;
        Engine::Instance().D2D()->CreateStrokeStyle(props, nullptr, 0, &style);
    }
    target_->DrawGeometry(geometry.Get(), b, thickness, style.Get());
}
void Canvas::FillCircle(float cx, float cy, float r, const Color& c) {
    if (!target_) return;
    auto* b = Brush(c);
    if (b) target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), b);
}

void Canvas::DrawText(const std::wstring& text, float x, float y, float w, float h, float size,
                      const Color& color, bool bold, bool mono, DWRITE_TEXT_ALIGNMENT align,
                      DWRITE_PARAGRAPH_ALIGNMENT valign, bool wrap) {
    if (!target_ || text.empty()) return;
    const std::wstring shown = translateUi_ ? TranslateUi(text) : text;
    if (shown.empty()) return;
    auto fmt = MakeFormat(size, bold, false, mono, align, valign, wrap);
    if (!fmt) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(Engine::Instance().DWrite()->CreateTextLayout(shown.c_str(), (UINT32)shown.size(), fmt.Get(),
            std::max(1.0f, w), std::max(1.0f, h), &layout))) return;
    auto* b = Brush(color);
    if (b) target_->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), b);
}

void Canvas::DrawIcon(const std::wstring& glyph, float x, float y, float w, float h, float size, const Color& color) {
    if (!target_ || glyph.empty()) return;
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(Engine::Instance().DWrite()->CreateTextFormat(FontFamilyIcon().c_str(), nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            size, L"zh-CN", &fmt))) return;
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(Engine::Instance().DWrite()->CreateTextLayout(glyph.c_str(), (UINT32)glyph.size(), fmt.Get(),
            std::max(1.0f, w), std::max(1.0f, h), &layout))) return;
    auto* b = Brush(color);
    if (b) target_->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), b);
}

float Canvas::MeasureText(const std::wstring& text, float size, bool bold, bool mono, float maxWidth, bool wrap) {
    if (text.empty()) return 0;
    const std::wstring shown = translateUi_ ? TranslateUi(text) : text;
    if (shown.empty()) return 0;
    auto fmt = MakeFormat(size, bold, false, mono, DWRITE_TEXT_ALIGNMENT_LEADING,
                          DWRITE_PARAGRAPH_ALIGNMENT_NEAR, wrap);
    if (!fmt) return 0;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(Engine::Instance().DWrite()->CreateTextLayout(shown.c_str(), (UINT32)shown.size(), fmt.Get(),
            wrap ? std::max(1.0f, maxWidth) : 1000000.0f, 1000000.0f, &layout))) return 0;
    DWRITE_TEXT_METRICS tm{};
    layout->GetMetrics(&tm);
    return wrap ? tm.width : tm.widthIncludingTrailingWhitespace;
}

void Canvas::PushClip(float x, float y, float w, float h) {
    if (!target_) return;
    target_->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
}
void Canvas::PopClip() {
    if (target_) target_->PopAxisAlignedClip();
}
void Canvas::SetTransform(float dx, float dy, float scale) {
    if (!target_) return;
    target_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(dx, dy));
}
void Canvas::ResetTransform() {
    if (target_) target_->SetTransform(D2D1::Matrix3x2F::Identity());
}
