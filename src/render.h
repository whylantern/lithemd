// Direct2D/DirectWrite rendering helpers.
#pragma once
#include "common.h"
#include "docmodel.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <map>
#include <memory>
#include <vector>

using Microsoft::WRL::ComPtr;

class Engine {
public:
    static Engine& Instance();
    bool Init();
    ID2D1Factory* D2D() const { return d2d_.Get(); }
    IDWriteFactory* DWrite() const { return dwrite_.Get(); }
    IWICImagingFactory* WIC() const { return wic_.Get(); }
private:
    ComPtr<ID2D1Factory> d2d_;
    ComPtr<IDWriteFactory> dwrite_;
    ComPtr<IWICImagingFactory> wic_;
    bool init_ = false;
};

Color RoleColor(const Palette& p, InlineRole role);
D2D1_COLOR_F ToD2D(const Color& c);
bool ImagePixelSize(const ImageRef& img, float& w, float& h);

struct TextEffect : IUnknown {
    virtual ~TextEffect() = default;
    InlineRole role = InlineRole::Text;
    bool inlineCode = false;
    bool link = false;
    bool highlight = false;
    std::wstring url;
    ULONG refs = 1;

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return ++refs; }
    STDMETHOD_(ULONG, Release)() override {
        ULONG n = --refs;
        if (n == 0) delete this;
        return n;
    }
};

class Canvas;

struct TextStyleOptions {
    float fontSize = 16.0f;
    bool bold = false;
    bool italic = false;
    bool mono = false;
    bool wrap = true;
    bool underline = false;
    InlineRole role = InlineRole::Text;
    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING;
    DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR;
    float maxWidth = 800.0f;
    float lineHeight = 1.5f;
    float baseline = 1.16f;
};

struct LinkRange {
    size_t start = 0;
    size_t length = 0;
    std::wstring url;
};

class RichText {
public:
    // Declared in an order that keeps effects and inline objects alive until
    // after the text layout (which may hold raw references to them) dies.
    std::wstring text;
    float width = 0;
    float height = 0;
    float baseline = 0;
    std::vector<LinkRange> links;
    std::vector<ComPtr<IDWriteInlineObject>> math;
    std::vector<ComPtr<TextEffect>> effects;
    ComPtr<IDWriteTextLayout> layout;

    std::wstring LinkAt(size_t textPos) const;
    void Draw(Canvas& canvas, float x, float y) const;
};

bool BuildRichText(const std::vector<Inline>& inlines, const TextStyleOptions& opt, RichText& out);
bool BuildPlainRichText(const std::wstring& text, const TextStyleOptions& opt, RichText& out);

class Canvas {
public:
    ~Canvas();
    bool Begin(HWND hwnd, const Palette& palette, bool clear = true);
    bool BeginBitmap(UINT widthPx, UINT heightPx, float dpi, const Palette& palette, bool clear = true);
    bool EndBitmap(std::vector<uint8_t>& png);
    void End();
    void Discard();
    void Resize(UINT w, UINT h);
    bool valid() const { return target_ != nullptr; }
    float scale() const { return scale_; }
    float widthDip() const { return widthPx_ / scale_; }
    float heightDip() const { return heightPx_ / scale_; }
    const Palette& palette() const { return *palette_; }
    ID2D1RenderTarget* target() const { return target_.Get(); }
    ID2D1SolidColorBrush* Brush(const Color& color);
    ID2D1Bitmap* Image(const ImageRef& img);
    void ClearImageCache() { images_.clear(); }

    void FillRect(float x, float y, float w, float h, const Color& c);
    void FillPolygon(const D2D1_POINT_2F* points, size_t count, const Color& c);
    void FillRound(float x, float y, float w, float h, float r, const Color& c);
    void StrokeRect(float x, float y, float w, float h, const Color& c, float thickness = 1.0f);
    void StrokeRound(float x, float y, float w, float h, float r, const Color& c, float thickness = 1.0f);
    void DrawEllipse(float cx, float cy, float rx, float ry, const Color& c, float thickness = 1.0f);
    void DrawLine(float x1, float y1, float x2, float y2, const Color& c, float thickness = 1.0f);
    void DrawStrokePath(const D2D1_POINT_2F* points, size_t count, const Color& c,
                        float thickness, bool round = true);
    void FillCircle(float cx, float cy, float r, const Color& c);
    void DrawText(const std::wstring& text, float x, float y, float w, float h, float size,
                  const Color& color, bool bold = false, bool mono = false,
                  DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                  DWRITE_PARAGRAPH_ALIGNMENT valign = DWRITE_PARAGRAPH_ALIGNMENT_NEAR,
                  bool wrap = true);
    void DrawIcon(const std::wstring& glyph, float x, float y, float w, float h, float size, const Color& color);
    float MeasureText(const std::wstring& text, float size, bool bold, bool mono, float maxWidth, bool wrap = false);
    void PushClip(float x, float y, float w, float h);
    void PopClip();
    void SetTransform(float dx, float dy, float scale = 1.0f);
    void ResetTransform();
    void SetTranslateUi(bool on) { translateUi_ = on; }

private:
    HWND hwnd_ = nullptr;
    ComPtr<ID2D1RenderTarget> target_;
    ComPtr<ID2D1HwndRenderTarget> hwndTarget_;
    ComPtr<IWICBitmap> wicBitmap_;
    const Palette* palette_ = &Palette::Light();
    float scale_ = 1.0f;
    UINT widthPx_ = 0, heightPx_ = 0;
    std::map<uint32_t, ComPtr<ID2D1SolidColorBrush>> brushes_;
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> images_;
    bool drawing_ = false;
    bool translateUi_ = false;
};

std::wstring FontFamilyBody();
std::wstring FontFamilyMono();
std::wstring FontFamilyIcon();
