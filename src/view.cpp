#include "view.h"
#include "view_internal.h"
#include "render.h"
#include "latex.h"
#include "pdf.h"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <map>
#include <functional>
#include <shellapi.h>
#include <commctrl.h>
#include <windowsx.h>
#include <mmsystem.h>

namespace {

enum class Mode { Welcome, Document, Source, Json, Grid, Pdf, Image, Error };

constexpr float kMargin = 44.0f;
constexpr float kBaseBody = 16.0f;
constexpr float kWheelLine = 34.0f;
constexpr int kWelcomeRecentPage = 8;
constexpr float kWelcomeRowH = 41.0f;
constexpr float kHomeMaxW = 920.0f;
constexpr float kHomeGap = 18.0f;
constexpr float kHomeHeroTop = 32.0f;
constexpr float kHomeHeroBottom = 254.0f;
constexpr float kHomeCardsTop = 270.0f;
constexpr float kHomeCardHeaderH = 40.0f;
constexpr float kHomeCardPad = 12.0f;
constexpr float kHomeCardRowH = 41.0f;
constexpr float kHomeListViewH = 150.0f;

std::wstring PlainInlines(const std::vector<Inline>& inlines) {
    std::wstring out;
    for (auto& in : inlines) {
        if (in.math) out += MathPlainText(*in.math);
        else out += in.text;
    }
    return out;
}

DWRITE_TEXT_ALIGNMENT AlignOf(int align) {
    if (align == 1) return DWRITE_TEXT_ALIGNMENT_CENTER;
    if (align == 2) return DWRITE_TEXT_ALIGNMENT_TRAILING;
    return DWRITE_TEXT_ALIGNMENT_LEADING;
}

TextStyleOptions BodyOpts(float size, float width, bool bold = false, InlineRole role = InlineRole::Text) {
    TextStyleOptions o;
    o.fontSize = size;
    o.bold = bold;
    o.role = role;
    o.maxWidth = width;
    o.lineHeight = 1.6f;
    o.baseline = 1.18f;
    return o;
}

TextStyleOptions CodeOpts(float size, float width, InlineRole role = InlineRole::Text) {
    TextStyleOptions o;
    o.fontSize = size;
    o.mono = true;
    o.role = role;
    o.maxWidth = width;
    o.lineHeight = 1.52f;
    o.baseline = 1.2f;
    return o;
}

// Inline formulas can be much taller than a text line (fractions, matrices).
// Give the line enough vertical room so DirectWrite does not clip them.
void TuneForMath(TextStyleOptions& o, const std::vector<Inline>& inlines) {
    IDWriteFactory* dw = Engine::Instance().DWrite();
    float maxHeight = 0;
    for (auto& in : inlines) {
        if (!in.math) continue;
        MathBox b = MeasureMath(*in.math, dw, o.fontSize * 1.05f);
        maxHeight = std::max(maxHeight, b.height());
    }
    if (maxHeight > 0) {
        float ratio = (maxHeight + o.fontSize * 0.35f) / std::max(1.0f, o.fontSize);
        o.lineHeight = std::max(o.lineHeight, ratio);
        o.baseline = std::min(o.baseline, o.lineHeight * 0.72f);
    }
}

std::wstring CellString(const GridSheet& sheet, int r, int c) {
    if (r < 0 || r >= (int)sheet.rows.size()) return L"";
    auto& row = sheet.rows[(size_t)r];
    if (c < 0 || c >= (int)row.size()) return L"";
    return row[(size_t)c];
}

int MaxCols(const GridSheet& sheet) {
    int n = sheet.maxCols;
    for (auto& r : sheet.rows) n = std::max(n, (int)r.size());
    return n;
}

} // namespace

// ================================================================ layout
struct DocView::Layout {
    struct Cell {
        RichText rt;
        float x = 0, y = 0, w = 0, h = 0;
        bool header = false;
    };
    struct Item {
        const Block* block = nullptr;
        float x = 0, y = 0, w = 0, h = 0;
        float contentX = 0, contentY = 0, contentW = 0;
        RichText main;
        std::vector<Cell> cells;
        std::wstring label;
        std::wstring badge;
        bool imageOk = false;
        std::shared_ptr<ImageRef> image;
        float imageX = 0, imageY = 0, imageW = 0, imageH = 0;
        float mathX = 0, mathBaseline = 0, mathSize = 0;
        bool quote = false;
        bool chevron = false;
        bool labelRight = false;
    };
    struct Group {
        float x = 0, y = 0, w = 0, h = 0;
        int role = 0;
    };
    std::vector<Item> items;
    std::vector<Group> groups;
    float height = 0;
    float width = 0;
};

// ================================================================ DocView
DocView::~DocView() {
    if (panTimerResolution_) { timeEndPeriod(1); panTimerResolution_ = false; }
    if (hwnd_) KillTimer(hwnd_, 3);
    if (inkEdit_) DestroyWindow(inkEdit_);
    if (inkEditFont_) DeleteObject(inkEditFont_);
    if (inkEditBrush_) DeleteObject(inkEditBrush_);
}

void DocView::Register(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"LitheMdDocView";
    RegisterClassExW(&wc);
}

bool DocView::Create(HWND parent, int id) {
    parent_ = parent;
    hwnd_ = CreateWindowExW(0, L"LitheMdDocView", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                            WS_CLIPCHILDREN | WS_TABSTOP, 0, 0, 100, 100, parent, (HMENU)(INT_PTR)id,
                            (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), this);
    return hwnd_ != nullptr;
}

void DocView::SetSettings(Settings* settings) {
    if (settings) settings->NormalizeInkPresets();
    settings_ = settings;
    inkColors_.clear();
    if (settings) {
        for (int index : settings->inkToolbarPresets) {
            if (index >= 0 && index < (int)settings->inkPresets.size() && inkColors_.size() < 5)
                inkColors_.push_back(settings->inkPresets[(size_t)index]);
        }
    }
    if (inkColors_.empty()) inkColors_ = DefaultInkPresets();
    if (inkColors_.size() > 5) inkColors_.resize(5);
    if (inkColor_ < 0 || inkColor_ >= (int)inkColors_.size()) inkColor_ = 0;
    if (settings_) {
        theme_ = settings_->theme;
        dark_ = theme_ == L"dark";
        zoom_ = settings_->zoom;
        wheelStep_ = settings_->wheelStep;
        recent_ = settings_->recent;
        WelcomeResetRecentPaging();
        int opacity = std::clamp(settings_->backgroundOpacity,
                                 Settings::MinBackgroundOpacity, Settings::MaxBackgroundOpacity);
        int fit = std::clamp(settings_->backgroundFit,
                             Settings::MinBackgroundFit, Settings::MaxBackgroundFit);
        int anchor = std::clamp(settings_->backgroundAnchor,
                                Settings::MinBackgroundAnchor, Settings::MaxBackgroundAnchor);
        if (settings_->backgroundImage != backgroundPath_ ||
            opacity != (int)std::lround(backgroundOpacity_ * 100.0f) ||
            fit != backgroundFit_ || anchor != backgroundAnchor_) {
            bool pathChanged = settings_->backgroundImage != backgroundPath_;
            backgroundPath_ = settings_->backgroundImage;
            backgroundOpacity_ = opacity / 100.0f;
            backgroundFit_ = fit;
            backgroundAnchor_ = anchor;
            if (pathChanged && canvas_) canvas_->ClearImageCache();
            InvalidateAll();
        }
    }
}

void DocView::ApplyTheme(const std::wstring& theme) {
    theme_ = theme.empty() ? L"blue" : theme;
    dark_ = theme_ == L"dark";
    ApplyDarkWindowTheme(hwnd_, dark_);
    if (inkEditBrush_) {
        DeleteObject(inkEditBrush_);
        inkEditBrush_ = nullptr;
    }
    InvalidateAll();
}

void DocView::SetZoom(double zoom) {
    float oldZoom = (float)zoom_;
    zoom_ = Clampf((float)zoom, 0.6f, 2.5f);
    if (oldZoom > 0.001f && std::abs(zoom_ - oldZoom) > 0.0005f)
        InkScaleForZoom(zoom_ / oldZoom);
    layoutDirty_ = true;
    source_.wrapWidth = -1.0f;
    source_.wrapZoom = -1.0f;
    json_.built = false;
    pdfState_.pages.clear();
    pdfState_.renderedWidth.clear();
    UpdateScrollBars();
    InvalidateAll();
    if (events_.statusChanged) events_.statusChanged();
}

void DocView::SetFontScale(double scale) {
    double next = std::clamp(scale, Settings::MinFontScale, Settings::MaxFontScale);
    if (std::abs(next - fontScale_) < 0.001) return;
    fontScale_ = next;
    layoutDirty_ = true;
    UpdateScrollBars();
    InvalidateAll();
}

void DocView::SetContentWidth(int width) {
    int next = std::clamp(width, Settings::MinContentWidth, Settings::MaxContentWidth);
    if (next == contentWidth_) return;
    contentWidth_ = next;
    layoutDirty_ = true;
    UpdateScrollBars();
    InvalidateAll();
}

void DocView::SetWheelStep(int step) {
    wheelStep_ = step < 1 ? 1 : (step > 10 ? 10 : step);
}

void DocView::SetRecent(const std::vector<std::wstring>& recent) {
    recent_ = recent;
    WelcomeResetRecentPaging();
    InvalidateAll();
}

void DocView::SetWorkspaceHistory(const std::vector<std::wstring>& history) {
    workspaces_ = history;
    welcomeWorkspaceScroll_ = 0;
    InvalidateAll();
}

void DocView::SetBackground(const std::wstring& path, int opacity, int fit, int anchor) {
    int nextOpacity = std::clamp(opacity, Settings::MinBackgroundOpacity, Settings::MaxBackgroundOpacity);
    int nextFit = std::clamp(fit, Settings::MinBackgroundFit, Settings::MaxBackgroundFit);
    int nextAnchor = std::clamp(anchor, Settings::MinBackgroundAnchor, Settings::MaxBackgroundAnchor);
    if (path == backgroundPath_ && nextOpacity == (int)std::lround(backgroundOpacity_ * 100.0f) &&
        nextFit == backgroundFit_ && nextAnchor == backgroundAnchor_)
        return;
    bool pathChanged = path != backgroundPath_;
    backgroundPath_ = path;
    backgroundOpacity_ = nextOpacity / 100.0f;
    backgroundFit_ = nextFit;
    backgroundAnchor_ = nextAnchor;
    if (pathChanged && canvas_) canvas_->ClearImageCache();
    InvalidateAll();
}

void DocView::ClearDocument() {
    if (middlePan_) EndMiddlePan();
    CancelInkAutoShape();
    scrollMetricsValid_ = false;
    doc_.reset();
    if (canvas_) canvas_->ClearImageCache();
    copyHover_ = CopyTarget{};
    ClearCopyFeedback();
    pdf_.reset();
    errorMessage_.clear();
    errorPath_.clear();
    errorSource_.clear();
    sourceMode_ = false;
    scrollX_ = scrollY_ = 0;
    layoutDirty_ = true;
    pdfState_ = PdfState{};
    grid_ = GridState{};
    hoverGridColSep_ = hoverGridRowSep_ = -1;
    json_ = JsonState{};
    source_ = SourceState{};
    if (inkEdit_) InkCancelText();
    inkLayers_.clear();
    inkCurrent_ = InkStroke{};
    inkCurrentShape_ = InkShape{};
    inkDrawing_ = inkErasing_ = inkShapeDrawing_ = inkFreehandActive_ = false;
    inkErasePushed_ = false;
    inkMode_ = false;
    inkMoreOpen_ = false;
    inkMorePanel_ = DRect{};
    inkTool_ = INK_TOOL_PEN;
    inkTextAnchor_ = {};
    inkTextSize_ = 20.0f;
    inkButtons_.clear();
    inkBarRect_ = DRect{};
    inkHover_ = 0;
    inkSelection_.clear();
    inkDragMode_ = 0;
    inkSelectionBarRect_ = DRect{};
    pdfHighlights_.clear();
    textHighlights_.clear();
    textHighlightPending_.clear();
    textHighlighting_ = false;
    pdfTextCache_.clear();
    pdfHighlighting_ = false;
    pdfHighlightPage_ = -1;
    notesDirty_ = false;
    notesError_.clear();
    UpdateScrollBars();
    InvalidateAll();
    if (events_.outlineChanged) events_.outlineChanged();
    if (events_.statusChanged) events_.statusChanged();
}

void DocView::SetDocument(std::unique_ptr<Document> doc, bool preserveScroll, bool preserveInk) {
    if (middlePan_) EndMiddlePan();
    CancelInkAutoShape();
    scrollMetricsValid_ = false;
    float oldFrac = -1;
    if (preserveScroll && layout_ && layout_->height > 1) {
        float viewH = heightDip();
        float maxScroll = std::max(1.0f, layout_->height - viewH);
        oldFrac = scrollY_ / maxScroll;
    }
    std::wstring oldPath = doc_ ? AbsolutePath(doc_->path) : L"";
    std::wstring newPath = doc ? AbsolutePath(doc->path) : L"";
    bool keepInk = preserveInk && !oldPath.empty() && !newPath.empty() &&
                   _wcsicmp(oldPath.c_str(), newPath.c_str()) == 0;
    std::map<int, InkLayer> savedLayers;
    std::map<int, std::vector<PdfHighlight>> savedHighlights;
    std::vector<TextHighlight> savedTextHighlights;
    bool savedDirty = false;
    if (keepInk) {
        savedLayers = std::move(inkLayers_);
        savedHighlights = std::move(pdfHighlights_);
        savedTextHighlights = std::move(textHighlights_);
        savedDirty = notesDirty_;
    }
    doc_ = std::move(doc);
    if (canvas_) canvas_->ClearImageCache();
    pdf_.reset();
    errorMessage_.clear();
    errorPath_.clear();
    errorSource_.clear();
    scrollX_ = scrollY_ = 0;
    sourceMode_ = false;
    layout_.reset();
    layoutDirty_ = true;
    source_.built = false;
    source_.lines.clear();
    json_.built = false;
    json_.rows.clear();
    json_.collapsed.clear();
    pdfState_ = PdfState{};
    grid_ = GridState{};
    hoverGridColSep_ = hoverGridRowSep_ = -1;
    selectedRow_ = selectedCol_ = -1;
    hoverJsonRow_ = -1;
    hoverLinkBlock_ = -1;
    hoverLink_.clear();
    copyHover_ = CopyTarget{};
    ClearCopyFeedback();
    if (inkEdit_) InkCancelText();
    inkLayers_.clear();
    inkCurrent_ = InkStroke{};
    inkCurrentShape_ = InkShape{};
    inkDrawing_ = inkErasing_ = inkShapeDrawing_ = inkFreehandActive_ = false;
    inkErasePushed_ = false;
    inkMode_ = false;
    inkMoreOpen_ = false;
    inkMorePanel_ = DRect{};
    inkTool_ = INK_TOOL_PEN;
    inkTextAnchor_ = {};
    inkTextSize_ = 20.0f;
    inkButtons_.clear();
    inkBarRect_ = DRect{};
    inkHover_ = 0;
    inkSelection_.clear();
    inkDragMode_ = 0;
    inkSelectionBarRect_ = DRect{};
    pdfHighlights_.clear();
    textHighlights_.clear();
    textHighlightPending_.clear();
    textHighlighting_ = false;
    pdfTextCache_.clear();
    pdfHighlighting_ = false;
    pdfHighlightPage_ = -1;
    pdfHighlightPageRect_ = DRect{};
    pdfHighlightDragRect_ = DRect{};
    notesDirty_ = false;
    notesError_.clear();

    if (!doc_) { ClearDocument(); return; }
    if (doc_->sourceDefault && doc_->sourceAllowed) sourceMode_ = true;
    if (doc_->kind == DocKind::Pdf) {
        pdf_ = std::make_shared<PdfDocument>();
        std::wstring err;
        if (!pdf_->Load(doc_->path, err)) {
            errorMessage_ = err.empty() ? L"无法打开 PDF 文件" : err;
            errorPath_ = doc_->path;
            pdfState_.error = errorMessage_;
            pdfState_.loaded = true;
        } else {
            doc_->pdfPages = pdf_->PageCount();
            pdfState_.loaded = true;
        }
    }
    if (doc_->kind == DocKind::Csv || doc_->kind == DocKind::Xlsx || doc_->kind == DocKind::Xls)
        RebuildGridWidths();
    if (doc_->kind == DocKind::Image && doc_->image && doc_->imageW <= 0) {
        float iw = 0, ih = 0;
        if (ImagePixelSize(*doc_->image, iw, ih)) {
            doc_->imageW = (int)iw;
            doc_->imageH = (int)ih;
        }
    }
    if (keepInk) {
        inkLayers_ = std::move(savedLayers);
        pdfHighlights_ = std::move(savedHighlights);
        textHighlights_ = std::move(savedTextHighlights);
        notesDirty_ = savedDirty;
    } else {
        std::wstring noteError;
        LoadNotes(&noteError);
    }
    pendingScrollFrac_ = oldFrac;
    UpdateScrollBars();
    InvalidateAll();
    if (events_.sourceModeChanged) events_.sourceModeChanged();
    if (events_.outlineChanged) events_.outlineChanged();
    if (events_.statusChanged) events_.statusChanged();
}

void DocView::SetError(const std::wstring& message, const std::wstring& path, const std::wstring& sourceText) {
    if (middlePan_) EndMiddlePan();
    CancelInkAutoShape();
    scrollMetricsValid_ = false;
    doc_.reset();
    if (canvas_) canvas_->ClearImageCache();
    pdf_.reset();
    errorMessage_ = message;
    errorPath_ = path;
    errorSource_ = sourceText;
    sourceMode_ = false;
    layoutDirty_ = true;
    scrollX_ = scrollY_ = 0;
    if (inkEdit_) InkCancelText();
    inkLayers_.clear();
    inkCurrent_ = InkStroke{};
    inkCurrentShape_ = InkShape{};
    inkDrawing_ = inkErasing_ = inkShapeDrawing_ = inkFreehandActive_ = false;
    inkErasePushed_ = false;
    inkMode_ = false;
    inkMoreOpen_ = false;
    inkMorePanel_ = DRect{};
    inkTool_ = INK_TOOL_PEN;
    inkTextAnchor_ = {};
    inkTextSize_ = 20.0f;
    inkButtons_.clear();
    inkBarRect_ = DRect{};
    inkHover_ = 0;
    inkSelection_.clear();
    inkDragMode_ = 0;
    inkSelectionBarRect_ = DRect{};
    pdfHighlights_.clear();
    textHighlights_.clear();
    textHighlightPending_.clear();
    textHighlighting_ = false;
    pdfTextCache_.clear();
    pdfHighlighting_ = false;
    pdfHighlightPage_ = -1;
    notesDirty_ = false;
    notesError_.clear();
    UpdateScrollBars();
    InvalidateAll();
    if (events_.outlineChanged) events_.outlineChanged();
    if (events_.statusChanged) events_.statusChanged();
}

bool DocView::CanSource() const {
    return doc_ && doc_->sourceAllowed && !doc_->text.empty();
}

bool DocView::CanOutline() const {
    return doc_ && !sourceMode_ && !doc_->outline.empty() &&
           (doc_->kind == DocKind::Markdown || doc_->kind == DocKind::Notebook ||
            doc_->kind == DocKind::Chat || doc_->kind == DocKind::Docx || doc_->kind == DocKind::Doc);
}

void DocView::SetSource(bool source) {
    if (!doc_ || !CanSource() || sourceMode_ == source) return;
    if (inkEdit_) InkCommitText();
    if (inkDrawing_ || inkFreehandActive_ || inkErasing_ || inkShapeDrawing_ ||
        textHighlighting_) InkEnd();
    // Plain text and source-code documents only have a source view. Without
    // this guard Ctrl+1 would switch to an empty rendered view.
    if (!source && doc_->kind == DocKind::Text) return;
    inkHover_ = 0;
    inkButtons_.clear();
    inkSelection_.clear();
    inkDragMode_ = 0;
    sourceMode_ = source;
    if (source) BuildSource();
    scrollX_ = scrollY_ = 0;
    layoutDirty_ = true;
    UpdateScrollBars();
    InvalidateAll();
    if (events_.sourceModeChanged) events_.sourceModeChanged();
    if (events_.outlineChanged) events_.outlineChanged();
    if (events_.statusChanged) events_.statusChanged();
}

void DocView::RebuildGridWidths() {
    if (!doc_) return;
    int oldSheet = grid_.sheet;
    grid_ = GridState{};
    grid_.colWidths.resize(doc_->grid.sheets.size());
    grid_.rowHeights.resize(doc_->grid.sheets.size());
    for (size_t s = 0; s < doc_->grid.sheets.size(); s++) {
        auto& sheet = doc_->grid.sheets[s];
        int cols = MaxCols(sheet);
        grid_.colWidths[s].assign((size_t)cols, 84.0f);
        for (int c = 0; c < cols; c++) {
            size_t maxLen = 4;
            int sample = std::min<int>((int)sheet.rows.size(), 120);
            for (int r = 0; r < sample; r++)
                maxLen = std::max(maxLen, std::min<size_t>(CellString(sheet, r, c).size(), 80));
            float w = 22.0f + maxLen * 7.4f;
            grid_.colWidths[s][(size_t)c] = Clampf(w, 66.0f, 380.0f);
        }
        grid_.rowHeights[s].assign(sheet.rows.size(), 27.0f);
        for (size_t r = 0; r < sheet.rows.size() && r < 300; r++) {
            int lines = 1;
            for (auto& cell : sheet.rows[r]) {
                int n = 1;
                for (wchar_t ch : cell) if (ch == L'\n') n++;
                lines = std::max(lines, n);
            }
            if (lines > 1) grid_.rowHeights[s][r] = std::min(90.0f, 27.0f * lines);
        }
    }
    grid_.sheet = std::min(oldSheet, (int)doc_->grid.sheets.size() - 1);
    if (grid_.sheet < 0) grid_.sheet = 0;
}

float DocView::PageWidth() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    float scale = GetDpiForWindow(hwnd_) / 96.0f;
    return (rc.right - rc.left) / scale;
}

int DocView::VisibleHeadingBlock() const {
    if (!doc_ || doc_->outline.empty()) return -1;
    if (layoutDirty_ || !layout_) const_cast<DocView*>(this)->RelayoutIfNeeded();
    if (!layout_) return -1;
    float top = scrollY_ + heightDip() * 0.12f;
    int current = doc_->outline.front().block;
    for (auto& heading : doc_->outline) {
        const Block* target = &doc_->blocks[(size_t)heading.block];
        float y = -1;
        for (auto& item : layout_->items) {
            if (item.block == target) { y = item.y; break; }
        }
        if (y < 0) continue;
        if (y <= top) current = heading.block;
        else break;
    }
    return current;
}

double DocView::ScrollFraction() const {
    if (!doc_ || lastContentHeight_ <= 0) return -1.0;
    float viewH = heightDip();
    float maxScroll = lastContentHeight_ - viewH;
    if (maxScroll <= 1.0f) return 0.0;
    return Clampf(scrollY_ / maxScroll, 0.0f, 1.0f);
}

void DocView::RestoreScrollFraction(double fraction) {
    if (!doc_ || !std::isfinite(fraction) || fraction < 0) return;
    pendingScrollFrac_ = Clampf((float)fraction, 0.0f, 1.0f);
    layoutDirty_ = true;
    source_.wrapWidth = -1.0f;
    UpdateScrollBars();
    InvalidateAll();
}

float DocView::heightDip() const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    float scale = GetDpiForWindow(hwnd_) / 96.0f;
    return (rc.bottom - rc.top) / scale;
}

void DocView::ImageDisplaySize(float viewW, float viewH, float& outW, float& outH) const {
    outW = outH = 0;
    if (!doc_ || !doc_->image || doc_->imageW <= 0 || doc_->imageH <= 0) return;
    float iw = (float)doc_->imageW, ih = (float)doc_->imageH;
    float fit = std::min((viewW - 80.0f) / iw, (viewH - 80.0f) / ih);
    if (fit > 1.0f) fit = 1.0f;
    if (fit < 0.02f) fit = 0.02f;
    outW = iw * fit * (float)zoom_;
    outH = ih * fit * (float)zoom_;
}

bool DocView::ImageDisplayRect(float& x, float& y, float& w, float& h) const {
    float viewW = PageWidth(), viewH = heightDip();
    ImageDisplaySize(viewW, viewH, w, h);
    if (w <= 0 || h <= 0) return false;
    x = (viewW - w) / 2;
    y = (viewH - h) / 2;
    if (w > viewW - 64) x = 32 - scrollX_;
    if (h > viewH - 64) y = 32 - scrollY_;
    return true;
}

void DocView::BuildSource() {
    if (source_.built || !doc_) return;
    source_.lines = SplitLines(doc_->text);
    if (source_.lines.size() > 200000) {
        source_.lines.resize(200000);
        source_.lines.push_back(L"…（源码过长，已截断显示）");
    }
    source_.rows.clear();
    source_.rowLines.clear();
    source_.wrapWidth = -1.0f;
    source_.wrapZoom = -1.0f;
    source_.built = true;
}

void DocView::BuildSourceRows(float width) {
    if (!source_.built) return;
    if (std::abs(source_.wrapWidth - width) < 0.5f &&
        std::abs(source_.wrapZoom - (float)zoom_) < 0.001f)
        return;
    source_.wrapWidth = width;
    source_.wrapZoom = (float)zoom_;
    source_.rows.clear();
    source_.rowLines.clear();
    source_.rows.reserve(source_.lines.size());
    source_.rowLines.reserve(source_.lines.size());
    float font = 13.0f * (float)zoom_;
    float cell = std::max(1.0f, font * 0.62f);
    int cells = std::max(16, (int)std::floor(std::max(1.0f, width - 12.0f) / cell));
    size_t maxRows = 500000;
    for (size_t li = 0; li < source_.lines.size(); li++) {
        std::wstring line;
        line.reserve(source_.lines[li].size());
        for (wchar_t ch : source_.lines[li]) {
            if (ch == L'\t') line += L"    ";
            else if (ch != L'\r') line += ch;
        }
        if (line.empty()) {
            source_.rows.push_back(L"");
            source_.rowLines.push_back((int)li);
            if (source_.rows.size() >= maxRows) break;
            continue;
        }
        size_t pos = 0;
        while (pos < line.size()) {
            size_t start = pos;
            float used = 0;
            size_t lastBreak = std::wstring::npos;
            while (pos < line.size()) {
                wchar_t ch = line[pos];
                if (ch == L' ' || ch == L'\t' || ch == L',' || ch == L';' ||
                    ch == L'(' || ch == L')' || ch == L'{' || ch == L'}')
                    lastBreak = pos + 1;
                float cw = ch > 0x7F ? 2.0f : 1.0f;
                if (used + cw > (float)cells && pos > start) break;
                used += cw;
                pos++;
            }
            if (pos < line.size() && lastBreak != std::wstring::npos &&
                lastBreak > start + (size_t)cells / 3) {
                pos = lastBreak;
            }
            if (pos <= start) pos = start + 1;
            source_.rows.push_back(line.substr(start, pos - start));
            source_.rowLines.push_back((int)li);
            if (source_.rows.size() >= maxRows) break;
        }
        if (source_.rows.size() >= maxRows) break;
    }
    if (source_.rows.empty()) {
        source_.rows.push_back(L"");
        source_.rowLines.push_back(0);
    }
}

void DocView::BuildJson() {
    if (json_.built || !doc_) return;
    json_.rows.clear();
    if (doc_->kind != DocKind::Json) { json_.built = true; return; }
    if (json_.collapsed.empty() && doc_->jsonNodes > 900 && doc_->jsonNodes <= 30000) {
        std::function<void(const JNode&, int)> collapseDeep = [&](const JNode& n, int depth) {
            if (n.kind == JKind::Object || n.kind == JKind::Array) {
                if (depth >= 2) json_.collapsed.insert(&n);
                for (auto& m : n.members) collapseDeep(m.second, depth + 1);
                for (auto& i : n.items) collapseDeep(i, depth + 1);
            }
        };
        collapseDeep(doc_->json, 0);
    }
    size_t maxChars = 20;
    std::function<void(const JNode&, const std::wstring&, int, bool)> add =
        [&](const JNode& n, const std::wstring& key, int depth, bool comma) {
            JsonRow row;
            row.node = &n;
            row.key = key;
            row.depth = depth;
            row.hasComma = comma;
            row.container = (n.kind == JKind::Object || n.kind == JKind::Array);
            row.expanded = row.container && !json_.collapsed.count(&n);
            if (n.kind == JKind::Object) { row.valueType = 4; row.value = L"{"; }
            else if (n.kind == JKind::Array) { row.valueType = 5; row.value = L"["; }
            else if (n.kind == JKind::String) { row.valueType = 3; row.value = JsonEscape(n.str); }
            else if (n.kind == JKind::Number) { row.valueType = 2; row.value = n.raw; }
            else if (n.kind == JKind::Bool) { row.valueType = 1; row.value = n.boolean ? L"true" : L"false"; }
            else { row.valueType = 0; row.value = L"null"; }
            json_.rows.push_back(row);
            maxChars = std::max(maxChars, key.size() + row.value.size() + (size_t)depth * 3 + 4);
            if (row.container && row.expanded) {
                if (n.kind == JKind::Object) {
                    for (size_t i = 0; i < n.members.size(); i++)
                        add(n.members[i].second, n.members[i].first, depth + 1, i + 1 < n.members.size());
                } else {
                    for (size_t i = 0; i < n.items.size(); i++)
                        add(n.items[i], L"[" + std::to_wstring(i) + L"]", depth + 1, i + 1 < n.items.size());
                }
                JsonRow close;
                close.node = &n;
                close.depth = depth;
                close.closing = true;
                close.valueType = (n.kind == JKind::Object) ? 4 : 5;
                close.hasComma = comma;
                json_.rows.push_back(close);
            }
        };
    add(doc_->json, L"", 0, false);
    json_.contentWidth = 60.0f + std::min<size_t>(maxChars, 2000) * 13.0f * 0.62f * (float)zoom_;
    json_.built = true;
}

void DocView::RelayoutIfNeeded() {
    if (!doc_ || rebuilding_) return;
    float viewW = PageWidth();
    if (!layoutDirty_ && layout_ && std::abs(lastWidth_ - viewW) < 0.5f) return;
    rebuilding_ = true;
    lastWidth_ = viewW;
    layoutDirty_ = false;
    auto lay = std::make_shared<Layout>();
    float contentW = std::min(viewW - 2 * kMargin, (float)contentWidth_);
    if (contentW < 220) contentW = std::max(120.0f, viewW - 24.0f);
    float left = (viewW - contentW) / 2;
    if (left < 16) left = 16;
    float z = (float)zoom_;
    float fs = (float)fontScale_;
    float y = 26.0f * z;
    float bottomPad = 40.0f * z;
    int previousChatGroup = -1;
    bool previousWasHeading = false;

    for (size_t i = 0; i < doc_->blocks.size(); i++) {
        Block& b = doc_->blocks[i];
        Layout::Item it;
        it.block = &b;
        float x = left, w = contentW;
        if (b.chatRole == 1) {
            w = contentW * 0.76f;
            x = left + contentW - w;
        }
        it.x = x;
        it.w = w;
        it.quote = b.quote;
        bool isChat = b.chatRole != 0;
        // Chat spacing belongs before the first block of a group, not after
        // it. Otherwise a new message header starts with only the intra-group
        // gap and its card background can collide with the previous card.
        if (i > 0) {
            if (isChat && b.group != previousChatGroup) {
                y += (previousChatGroup < 0 ? 10.0f : 24.0f) * z;
            } else if (previousWasHeading) {
                y += 2.0f * z;
            } else if (isChat) {
                y += 8.0f * z;
            } else {
                y += 10.0f * z;
            }
        }
        switch (b.kind) {
        case BlockKind::Heading: {
            float size = (b.level <= 1 ? 30.0f : b.level == 2 ? 24.0f : b.level == 3 ? 20.0f :
                          b.level == 4 ? 18.0f : 16.0f) * z * fs;
            y += (b.level <= 1 ? 26.0f : 18.0f) * z;
            TextStyleOptions o = BodyOpts(size, w, true);
            o.lineHeight = 1.3f;
            o.baseline = 1.05f;
            o.align = AlignOf(b.align);
            TuneForMath(o, b.inlines);
            BuildRichText(b.inlines, o, it.main);
            it.contentX = x;
            it.contentY = y;
            it.h = it.main.height + 8 * z;
        } break;
        case BlockKind::Paragraph:
        case BlockKind::Quote: {
            float indent = b.quote ? 16.0f * z : 0.0f;
            TextStyleOptions o = BodyOpts(kBaseBody * z * fs, w - indent);
            o.align = AlignOf(b.align);
            TuneForMath(o, b.inlines);
            BuildRichText(b.inlines, o, it.main);
            it.contentX = x + indent;
            it.contentY = y + (b.quote ? 10 * z : 0);
            it.h = it.main.height + (b.quote ? 20 * z : 0);
        } break;
        case BlockKind::ListItem: {
            float indent = std::min(6, b.indent) * 22.0f * z;
            float markerW = 24.0f * z + (b.task ? 6.0f * z : 0);
            TextStyleOptions o = BodyOpts(kBaseBody * z * fs, w - indent - markerW);
            TuneForMath(o, b.inlines);
            BuildRichText(b.inlines, o, it.main);
            it.contentX = x + indent + markerW;
            it.contentY = y;
            it.h = std::max(it.main.height, 22.0f * z) + 3 * z;
        } break;
        case BlockKind::Code:
        case BlockKind::OutputText: {
            bool out = b.kind == BlockKind::OutputText;
            float pad = (out ? 12.0f : 15.0f) * z;
            float labelH = 0;
            if (out) {
                it.label = b.label.empty() ? (b.error ? L"错误" : L"输出") : b.label;
                labelH = 18 * z;
            } else if (!b.badge.empty() || !b.lang.empty()) {
                it.label = b.badge.empty() ? b.lang : b.badge;
                labelH = 20 * z;
            }
            float innerW = w - pad * 2;
            TextStyleOptions o = CodeOpts((out ? 13.0f : 13.5f) * z * fs, innerW,
                                          b.error ? InlineRole::Danger : InlineRole::Text);
            const std::vector<Inline>* inlines = &b.inlines;
            std::vector<Inline> fallback;
            if (inlines->empty()) {
                Inline in;
                in.text = b.text;
                fallback.push_back(in);
                inlines = &fallback;
            }
            BuildRichText(*inlines, o, it.main);
            it.contentX = x + pad;
            it.contentY = y + pad + labelH;
            it.h = it.main.height + pad * 2 + labelH;
        } break;
        case BlockKind::Math: {
            float fs = 22.0f * z;
            if (b.math) {
                MathBox box = MeasureMath(*b.math, Engine::Instance().DWrite(), fs);
                it.mathSize = fs;
                it.mathBaseline = y + 10 * z + box.ascent;
                it.mathX = x + (w - box.width) / 2;
                it.h = box.height() + 24 * z;
                it.contentX = x;
                it.contentY = y;
            } else {
                it.h = 10 * z;
            }
            if (!b.label.empty()) it.label = b.label;
        } break;
        case BlockKind::Table: {
            int cols = (int)b.table.aligns.size();
            for (auto& row : b.table.rows) cols = std::max(cols, (int)row.size());
            if (cols > 0) {
                std::vector<float> pref((size_t)cols, 70.0f * z);
                for (int c = 0; c < cols; c++) {
                    size_t maxLen = 3;
                    for (auto& row : b.table.rows) {
                        if (c < (int)row.size())
                            maxLen = std::max(maxLen, std::min<size_t>(PlainInlines(row[(size_t)c].inlines).size(), 80));
                    }
                    pref[(size_t)c] = Clampf(24.0f * z + maxLen * 7.8f * z, 56.0f * z, 420.0f * z);
                }
                float total = 0;
                for (float p : pref) total += p;
                float avail = w - 2 * z;
                if (total > avail) {
                    float scale = avail / total;
                    for (auto& p : pref) p = std::max(46.0f * z, p * scale);
                    total = 0;
                    for (float p : pref) total += p;
                    if (total > avail) {
                        float extra = (total - avail) / cols;
                        for (auto& p : pref) p = std::max(36.0f * z, p - extra);
                    }
                } else if (total < avail) {
                    float extra = (avail - total) / std::max(1, cols);
                    for (auto& p : pref) p += extra;
                }
                float tableTop = y;
                float rowHeightTotal = 0;
                for (auto& row : b.table.rows) {
                    float rowH = 30.0f * z;
                    std::vector<Layout::Cell> cells;
                    float cx = x;
                    for (int c = 0; c < cols; c++) {
                        Layout::Cell cell;
                        cell.x = cx;
                        cell.y = tableTop + rowHeightTotal;
                        cell.w = pref[(size_t)c];
                        cell.header = row.empty() ? false : row[0].header;
                        if (c < (int)row.size()) {
                            bool hdr = row[(size_t)c].header || cell.header;
                            TextStyleOptions o = BodyOpts(14.0f * z * fs, cell.w - 16 * z, hdr);
                            o.lineHeight = 1.45f;
                            o.baseline = 1.2f;
                            int align = c < (int)b.table.aligns.size() ? b.table.aligns[(size_t)c] : row[(size_t)c].align;
                            o.align = AlignOf(align);
                            TuneForMath(o, row[(size_t)c].inlines);
                            BuildRichText(row[(size_t)c].inlines, o, cell.rt);
                        }
                        rowH = std::max(rowH, cell.rt.height + 14 * z);
                        cx += cell.w;
                        cells.push_back(std::move(cell));
                    }
                    for (auto& cell : cells) cell.h = rowH;
                    for (auto& cell : cells) it.cells.push_back(std::move(cell));
                    rowHeightTotal += rowH;
                }
                it.h = rowHeightTotal + 12 * z;
                it.contentX = x;
                it.contentY = tableTop;
            }
        } break;
        case BlockKind::Image:
        case BlockKind::OutputImage: {
            float labelH = b.kind == BlockKind::OutputImage ? 22 * z : 0;
            it.label = b.kind == BlockKind::OutputImage ? (b.label.empty() ? L"输出图片" : b.label) : L"";
            float iw = 0, ih = 0;
            if (b.image && ImagePixelSize(*b.image, iw, ih) && iw > 1 && ih > 1) {
                float maxW = w - (b.kind == BlockKind::OutputImage ? 28 * z : 0);
                float maxH = 900.0f * z;
                float scale = std::min(maxW / iw, maxH / ih);
                if (scale > 1.0f) scale = 1.0f;
                it.imageW = iw * scale;
                it.imageH = ih * scale;
                it.image = b.image;
                it.imageOk = true;
                it.imageX = x + (w - it.imageW) / 2;
                it.imageY = y + labelH;
                it.h = it.imageH + labelH + 14 * z;
            } else {
                it.image = b.image;
                it.imageOk = false;
                it.h = (b.kind == BlockKind::OutputImage ? 58.0f : 44.0f) * z + labelH;
                it.contentX = x;
                it.contentY = y + labelH;
            }
        } break;
        case BlockKind::Hr:
            it.h = 26.0f * z;
            break;
        case BlockKind::Session: {
            float pad = 18.0f * z;
            float labelH = 18.0f * z;
            TextStyleOptions o = BodyOpts(23.0f * z * fs, w - pad * 2, true);
            o.lineHeight = 1.25f;
            o.baseline = 1.0f;
            BuildPlainRichText(b.text.empty() ? L"LitheMD" : b.text, o, it.main);
            it.label = b.label.empty() ? L"会话" : b.label;
            it.badge = b.badge;
            it.contentX = x + pad;
            it.contentY = y + pad + labelH;
            float badgeH = b.badge.empty() ? 0 : 20.0f * z;
            it.h = pad * 2 + labelH + it.main.height + badgeH + 8 * z;
        } break;
        case BlockKind::ChatHeader: {
            it.label = b.label;
            it.badge = b.badge;
            it.h = 25.0f * z;
            it.labelRight = b.chatRole == 1;
            if (it.labelRight) {
                if (!it.badge.empty()) it.label += L"  ·  " + it.badge;
                it.badge.clear();
            }
            it.contentX = x;
            it.contentY = y;
        } break;
        case BlockKind::Reasoning:
        case BlockKind::Tool: {
            float pad = 10.0f * z;
            float headerH = 25.0f * z;
            it.label = b.summary.empty() ? b.label : b.summary;
            it.chevron = b.collapsible;
            it.contentX = x + pad + (b.collapsible ? 16 * z : 0);
            it.contentY = y + pad + headerH;
            if (b.expanded && !b.text.empty()) {
                TextStyleOptions o = (b.kind == BlockKind::Tool ? CodeOpts(12.5f * z * fs, w - pad * 2 - 16 * z)
                                                                : BodyOpts(13.5f * z * fs, w - pad * 2 - 16 * z));
                o.lineHeight = 1.5f;
                BuildPlainRichText(b.text, o, it.main);
                it.h = pad * 2 + headerH + it.main.height + 6 * z;
            } else {
                it.h = pad * 2 + headerH;
            }
        } break;
        case BlockKind::Banner: {
            float pad = 14.0f * z;
            TextStyleOptions o = BodyOpts(15.0f * z * fs, w - pad * 2);
            BuildPlainRichText(b.text, o, it.main);
            it.label = b.label;
            it.badge = b.badge;
            it.contentX = x + pad;
            it.contentY = y + pad + (b.label.empty() ? 0 : 18 * z);
            it.h = pad * 2 + it.main.height + (b.label.empty() ? 0 : 18 * z);
        } break;
        default:
            it.h = 0;
            break;
        }
        it.y = y;
        y += it.h;
        if (isChat) previousChatGroup = b.group;
        previousWasHeading = b.kind == BlockKind::Heading;
        lay->items.push_back(std::move(it));
    }

    std::map<int, Layout::Group> groups;
    for (auto& it : lay->items) {
        if (it.block->group < 0 || it.block->chatRole == 0) continue;
        auto& g = groups[it.block->group];
        if (g.w <= 0) {
            g.x = it.x;
            g.y = it.y;
            g.w = it.w;
            g.h = it.h;
            g.role = it.block->chatRole;
        } else {
            float x2 = std::max(g.x + g.w, it.x + it.w);
            g.x = std::min(g.x, it.x);
            g.w = x2 - g.x;
            g.h = std::max(g.h, it.y + it.h - g.y);
        }
    }
    for (auto& kv : groups) {
        auto& g = kv.second;
        g.x -= 10 * z;
        g.y -= 6 * z;
        g.w += 20 * z;
        g.h += 12 * z;
        lay->groups.push_back(g);
    }
    lay->height = y + bottomPad + 10.0f * z;
    lay->width = viewW;
    layout_ = std::move(lay);
    if (pendingScrollFrac_ >= 0) {
        float viewH = heightDip();
        float maxScroll = std::max(0.0f, layout_->height - viewH);
        scrollY_ = pendingScrollFrac_ * maxScroll;
        pendingScrollFrac_ = -1;
    }
    rebuilding_ = false;
    UpdateScrollBars();
}

void DocView::RefreshLayout() {
    layoutDirty_ = true;
    RelayoutIfNeeded();
    UpdateScrollBars();
    InvalidateAll();
}

void DocView::UpdateScrollBars() {
    if (!hwnd_ || updatingScrollbars_) return;
    updatingScrollbars_ = true;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    float scale = GetDpiForWindow(hwnd_) / 96.0f;
    float viewW = (rc.right - rc.left) / scale;
    float viewH = (rc.bottom - rc.top) / scale;
    if (viewW < 1) viewW = 1;
    if (viewH < 1) viewH = 1;
    Mode m = Mode::Welcome;
    if (!errorMessage_.empty()) m = Mode::Error;
    else if (!doc_) m = Mode::Welcome;
    else if (sourceMode_) m = Mode::Source;
    else if (doc_->kind == DocKind::Json) m = Mode::Json;
    else if (doc_->kind == DocKind::Csv || doc_->kind == DocKind::Xlsx || doc_->kind == DocKind::Xls) m = Mode::Grid;
    else if (doc_->kind == DocKind::Pdf) m = Mode::Pdf;
    else if (doc_->kind == DocKind::Image) m = Mode::Image;
    else m = Mode::Document;

    float totalH = viewH, totalW = viewW;
    if (m == Mode::Document) {
        if (layoutDirty_ || !layout_) RelayoutIfNeeded();
        if (layout_) {
            totalH = layout_->height;
            totalW = layout_->width;
        }
    } else if (m == Mode::Source) {
        if (!source_.built) BuildSource();
        float lineH = 19.0f * (float)zoom_;
        float gutter = 60.0f * (float)zoom_;
        BuildSourceRows(std::max(32.0f, viewW - gutter - 24.0f));
        totalH = 16 + source_.rows.size() * lineH + 30;
        totalW = viewW;
    } else if (m == Mode::Json) {
        if (!json_.built) BuildJson();
        float rowH = 24.0f * (float)zoom_;
        totalH = 24 + json_.rows.size() * rowH + 20;
        totalW = std::max(viewW, json_.contentWidth);
    } else if (m == Mode::Grid) {
        if (doc_ && !doc_->grid.sheets.empty()) {
            int s = std::min(grid_.sheet, (int)doc_->grid.sheets.size() - 1);
            auto& sheet = doc_->grid.sheets[(size_t)s];
            float z = (float)zoom_;
            float numW = 52 * z;
            float tw = numW;
            int cols = MaxCols(sheet);
            for (int c = 0; c < cols; c++) {
                float w = c < (int)grid_.colWidths[s].size() ? grid_.colWidths[s][c] : 84;
                tw += w * z;
            }
            float th = 30 * z;
            if (!grid_.rowHeights[s].empty())
                for (size_t r = 1; r < grid_.rowHeights[s].size(); r++) th += grid_.rowHeights[s][r] * z;
            if (doc_->grid.sheets.size() > 1) th += 30 * z;
            totalH = th;
            totalW = tw;
        } else {
            totalH = viewH;
            totalW = viewW;
        }
    } else if (m == Mode::Pdf) {
        if (pdf_ && pdfState_.error.empty()) {
            float pageBase = std::min(viewW - 96.0f, 900.0f);
            if (pageBase < 260) pageBase = std::max(180.0f, viewW - 40.0f);
            float pageW = pageBase * (float)zoom_;
            float gap = 20.0f;
            float h = 26.0f;
            for (int i = 0; i < pdf_->PageCount(); i++) {
                float pw = 1, ph = 1;
                pdf_->PageSize(i, pw, ph);
                h += pageW * (ph / std::max(1.0f, pw)) + gap;
            }
            totalH = h;
            totalW = std::max(viewW, pageW + 80.0f);
        }
    } else if (m == Mode::Image) {
        float dw = 0, dh = 0;
        ImageDisplaySize(viewW, viewH, dw, dh);
        if (dw > 0 && dh > 0) {
            totalW = std::max(viewW, dw + 64.0f);
            totalH = std::max(viewH, dh + 64.0f);
        }
    } else if (m == Mode::Welcome) {
        WelcomeMetrics wm = ComputeWelcomeMetrics(viewW, viewH);
        totalH = wm.contentH;
        totalW = viewW;
    }
    float maxY = std::max(0.0f, totalH - viewH);
    float maxX = std::max(0.0f, totalW - viewW);
    lastContentHeight_ = totalH;
    scrollMaxX_ = maxX;
    scrollMaxY_ = maxY;
    scrollMetricsValid_ = true;
    if (pendingScrollFrac_ >= 0) {
        scrollY_ = pendingScrollFrac_ * maxY;
        pendingScrollFrac_ = -1;
    }
    scrollY_ = Clampf(scrollY_, 0, maxY);
    scrollX_ = Clampf(scrollX_, 0, maxX);
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = (int)std::ceil(std::max(totalH, viewH));
    si.nPage = (int)std::ceil(viewH);
    si.nPos = (int)std::lround(scrollY_);
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
    si.nMax = (int)std::ceil(std::max(totalW, viewW));
    si.nPage = (int)std::ceil(viewW);
    si.nPos = (int)std::lround(scrollX_);
    SetScrollInfo(hwnd_, SB_HORZ, &si, TRUE);
    bool showV = maxY > 0.5f;
    bool showH = maxX > 0.5f;
    if (showV != vertScrollShown_) {
        vertScrollShown_ = showV;
        ShowScrollBar(hwnd_, SB_VERT, showV);
    }
    if (showH != horzScrollShown_) {
        horzScrollShown_ = showH;
        ShowScrollBar(hwnd_, SB_HORZ, showH);
    }
    if (events_.statusChanged) events_.statusChanged();
    lastScrollStatusTick_ = GetTickCount64();
    // The home page itself never scrolls; the recent-file list has its own
    // embedded scroll region.
    if (m == Mode::Welcome) {
        ShowScrollBar(hwnd_, SB_VERT, FALSE);
        ShowScrollBar(hwnd_, SB_HORZ, FALSE);
        vertScrollShown_ = horzScrollShown_ = false;
    }
    updatingScrollbars_ = false;
}

void DocView::UpdateScrollPosition() {
    if (!hwnd_) return;
    if (!scrollMetricsValid_) {
        UpdateScrollBars();
        InvalidateAll();
        return;
    }
    scrollY_ = Clampf(scrollY_, 0, scrollMaxY_);
    scrollX_ = Clampf(scrollX_, 0, scrollMaxX_);
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_POS;
    si.nPos = (int)std::lround(scrollY_);
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
    si.nPos = (int)std::lround(scrollX_);
    SetScrollInfo(hwnd_, SB_HORZ, &si, TRUE);
    // The outline/status only need a readable refresh rate; doing this on
    // every animation tick forces a second full chrome repaint.
    ULONGLONG now = GetTickCount64();
    if (events_.statusChanged && now - lastScrollStatusTick_ >= 120) {
        lastScrollStatusTick_ = now;
        events_.statusChanged();
    }
    InvalidateAll();
}

void DocView::InvalidateAll() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

// ---------------------------------------------------------------- paint
void DocView::Paint() {
    if (!hwnd_) return;
    if (!canvas_) canvas_ = std::make_unique<Canvas>();
    Canvas& c = *canvas_;
    if (!c.Begin(hwnd_, Palette::Get(theme_))) return;
    PaintBackground(c);
    PaintContent(c);
    c.End();
}

void DocView::PaintBackground(Canvas& c) {
    if (backgroundPath_.empty() || backgroundOpacity_ <= 0.001f) return;
    ImageRef img;
    img.path = backgroundPath_;
    img.cacheKey = L"background:" + backgroundPath_ + L":" +
                   std::to_wstring(FileWriteTimeUtc(backgroundPath_));
    ID2D1Bitmap* bmp = c.Image(img);
    if (!bmp) return;
    D2D1_SIZE_F size = bmp->GetSize();
    if (size.width < 1.0f || size.height < 1.0f) return;
    float w = c.widthDip(), h = c.heightDip();
    c.PushClip(0, 0, w, h);
    auto draw = [&](float x, float y, float dw, float dh) {
        c.target()->DrawBitmap(bmp, D2D1::RectF(x, y, x + dw, y + dh), backgroundOpacity_,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    };
    if (backgroundFit_ == 1) {
        // Contain: show the whole image, letterbox with the theme background.
        float scale = std::min(w / size.width, h / size.height);
        float dw = size.width * scale, dh = size.height * scale;
        draw((w - dw) * 0.5f, (h - dh) * 0.5f, dw, dh);
    } else if (backgroundFit_ == 2) {
        // Stretch: fill the viewport, accepting aspect-ratio distortion.
        draw(0, 0, w, h);
    } else if (backgroundFit_ == 3) {
        // Tile: keep the native size where possible, but enlarge tiny images so
        // a 16x16 icon does not create thousands of draw calls.
        float scale = 1.0f;
        const float minTile = 128.0f;
        if (size.width < minTile || size.height < minTile)
            scale = std::max(minTile / size.width, minTile / size.height);
        float tw = size.width * scale, th = size.height * scale;
        int cols = std::max(1, (int)std::ceil(w / tw));
        int rows = std::max(1, (int)std::ceil(h / th));
        while ((int64_t)cols * rows > 400) {
            scale *= 1.5f;
            tw = size.width * scale;
            th = size.height * scale;
            cols = std::max(1, (int)std::ceil(w / tw));
            rows = std::max(1, (int)std::ceil(h / th));
        }
        for (int row = 0; row < rows; row++)
            for (int col = 0; col < cols; col++)
                draw(col * tw, row * th, tw, th);
    } else {
        // Cover: fill the viewport and crop around the selected anchor.
        float scale = std::max(w / size.width, h / size.height);
        float dw = size.width * scale, dh = size.height * scale;
        float fx = (backgroundAnchor_ % 3) / 2.0f;
        float fy = (backgroundAnchor_ / 3) / 2.0f;
        draw((w - dw) * fx, (h - dh) * fy, dw, dh);
    }
    c.PopClip();
}

void DocView::PaintContent(Canvas& c, bool exportOnly) {
    Mode m = Mode::Welcome;
    if (!errorMessage_.empty()) m = Mode::Error;
    else if (!doc_) m = Mode::Welcome;
    else if (sourceMode_) m = Mode::Source;
    else if (doc_->kind == DocKind::Json) m = Mode::Json;
    else if (doc_->kind == DocKind::Csv || doc_->kind == DocKind::Xlsx || doc_->kind == DocKind::Xls) m = Mode::Grid;
    else if (doc_->kind == DocKind::Pdf) m = Mode::Pdf;
    else if (doc_->kind == DocKind::Image) m = Mode::Image;
    else m = Mode::Document;

    if (m == Mode::Document && (layoutDirty_ || !layout_)) RelayoutIfNeeded();
    if (m == Mode::Source && !source_.built) BuildSource();
    if (m == Mode::Json && !json_.built) BuildJson();

    c.SetTranslateUi(m == Mode::Welcome || m == Mode::Error);
    switch (m) {
    case Mode::Welcome: PaintWelcome(c); break;
    case Mode::Error: PaintError(c); break;
    case Mode::Document: PaintDocument(c); break;
    case Mode::Source: PaintSource(c); break;
    case Mode::Json: PaintJson(c); break;
    case Mode::Grid: PaintGrid(c); break;
    case Mode::Pdf: PaintPdf(c); break;
    case Mode::Image: PaintImage(c); break;
    }
    c.ResetTransform();
    if (middlePan_ && !exportOnly) {
        auto& pal = c.palette();
        float ax = (float)panAnchor_.x, ay = (float)panAnchor_.y;
        Color shadow = HexColor(L"#000000", 0.14f);
        c.FillCircle(ax + 1, ay + 2, 20, shadow);
        Color bubble = pal.surface;
        bubble.a = 0.94f;
        c.FillCircle(ax, ay, 19, bubble);
        c.DrawEllipse(ax, ay, 18, 18, pal.border, 1.0f);
        Color idle = pal.muted;
        idle.a = 0.82f;
        Color upColor = panVelocityY_ < -18 ? pal.accent : idle;
        Color downColor = panVelocityY_ > 18 ? pal.accent : idle;
        Color leftColor = panVelocityX_ < -18 ? pal.accent : idle;
        Color rightColor = panVelocityX_ > 18 ? pal.accent : idle;
        auto arrow = [&](float x1, float y1, float x2, float y2, const Color& col) {
            c.DrawLine(x1, y1, x2, y2, col, 1.7f);
        };
        // Up
        arrow(ax, ay - 12, ax - 4, ay - 8, upColor);
        arrow(ax, ay - 12, ax + 4, ay - 8, upColor);
        arrow(ax, ay - 12, ax, ay - 5, upColor);
        // Down
        arrow(ax, ay + 12, ax - 4, ay + 8, downColor);
        arrow(ax, ay + 12, ax + 4, ay + 8, downColor);
        arrow(ax, ay + 12, ax, ay + 5, downColor);
        // Left
        arrow(ax - 12, ay, ax - 8, ay - 4, leftColor);
        arrow(ax - 12, ay, ax - 8, ay + 4, leftColor);
        arrow(ax - 12, ay, ax - 5, ay, leftColor);
        // Right
        arrow(ax + 12, ay, ax + 8, ay - 4, rightColor);
        arrow(ax + 12, ay, ax + 8, ay + 4, rightColor);
        arrow(ax + 12, ay, ax + 5, ay, rightColor);
        c.FillCircle(ax, ay, 2.4f, pal.accent);
    }
    // Ink follows the document content, then the copy affordance / pen toolbar
    // are drawn on top so text can never cover the interactive controls.
    c.SetTranslateUi(false);
    PaintInk(c, exportOnly);
    if (!exportOnly) {
        if (!inkMode_) DrawCopyButton(c);
        if (inkMode_) PaintInkToolbar(c);
    }
}

bool DocView::ExportPng(float scale, std::vector<uint8_t>& png, std::wstring* error,
                        int* outW, int* outH) {
    if (error) error->clear();
    png.clear();
    if (!doc_) {
        if (error) *error = L"当前没有可导出的内容";
        return false;
    }
    if (inkDrawing_ || inkFreehandActive_ || inkShapeDrawing_) InkEnd();
    float viewW = PageWidth();
    float contentH = ContentHeightDip();
    if (viewW < 1 || contentH < 1) {
        if (error) *error = L"窗口尺寸无效";
        return false;
    }
    if (!(scale > 0.05f && scale < 64.0f)) {
        if (error) *error = L"缩放比例无效";
        return false;
    }
    double pxW = std::round((double)viewW * scale);
    double pxH = std::round((double)contentH * scale);
    if (pxW < 16 || pxH < 16 || pxW > 20000 || pxH > 20000 ||
        pxW * pxH > 120.0 * 1000.0 * 1000.0) {
        if (error) *error = L"导出尺寸超出范围";
        return false;
    }
    Canvas exportCanvas;
    if (!exportCanvas.BeginBitmap((UINT)pxW, (UINT)pxH, 96.0f * scale,
                                  Palette::Get(theme_), true)) {
        if (error) *error = L"无法创建导出画布";
        return false;
    }
    float oldScrollX = scrollX_, oldScrollY = scrollY_;
    scrollX_ = 0;
    scrollY_ = 0;
    exporting_ = true;
    PaintContent(exportCanvas, true);
    exporting_ = false;
    scrollX_ = oldScrollX;
    scrollY_ = oldScrollY;
    if (!exportCanvas.EndBitmap(png) || png.empty()) {
        if (error) *error = L"无法生成导出图片";
        return false;
    }
    if (outW) *outW = (int)pxW;
    if (outH) *outH = (int)pxH;
    return true;
}

bool DocView::ExportPngPage(float scale, float offsetY, float pageHeight,
                            std::vector<uint8_t>& png, std::wstring* error,
                            int* outW, int* outH) {
    if (error) error->clear();
    png.clear();
    if (!doc_) {
        if (error) *error = L"当前没有可导出的内容";
        return false;
    }
    if (inkDrawing_ || inkFreehandActive_ || inkShapeDrawing_) InkEnd();
    float viewW = PageWidth();
    if (viewW < 1 || pageHeight < 1) {
        if (error) *error = L"窗口尺寸无效";
        return false;
    }
    if (!(scale > 0.05f && scale < 64.0f)) {
        if (error) *error = L"缩放比例无效";
        return false;
    }
    double pxW = std::round((double)viewW * scale);
    double pxH = std::round((double)pageHeight * scale);
    if (pxW < 16 || pxH < 16 || pxW > 20000 || pxH > 20000 ||
        pxW * pxH > 120.0 * 1000.0 * 1000.0) {
        if (error) *error = L"导出尺寸超出范围";
        return false;
    }
    Canvas exportCanvas;
    if (!exportCanvas.BeginBitmap((UINT)pxW, (UINT)pxH, 96.0f * scale, Palette::Get(theme_), true)) {
        if (error) *error = L"无法创建导出画布";
        return false;
    }
    float oldScrollX = scrollX_, oldScrollY = scrollY_;
    scrollX_ = 0;
    scrollY_ = std::max(0.0f, offsetY);
    exporting_ = true;
    PaintContent(exportCanvas, true);
    exporting_ = false;
    scrollX_ = oldScrollX;
    scrollY_ = oldScrollY;
    if (!exportCanvas.EndBitmap(png) || png.empty()) {
        if (error) *error = L"无法生成导出图片";
        return false;
    }
    if (outW) *outW = (int)pxW;
    if (outH) *outH = (int)pxH;
    return true;
}

float DocView::ContentHeightDip() {
    Mode m = Mode::Welcome;
    if (!errorMessage_.empty()) m = Mode::Error;
    else if (!doc_) m = Mode::Welcome;
    else if (sourceMode_) m = Mode::Source;
    else if (doc_->kind == DocKind::Json) m = Mode::Json;
    else if (doc_->kind == DocKind::Csv || doc_->kind == DocKind::Xlsx || doc_->kind == DocKind::Xls) m = Mode::Grid;
    else if (doc_->kind == DocKind::Pdf) m = Mode::Pdf;
    else if (doc_->kind == DocKind::Image) m = Mode::Image;
    else m = Mode::Document;
    float viewH = heightDip();
    if (m == Mode::Welcome)
        return std::max(viewH, ComputeWelcomeMetrics(PageWidth(), viewH).contentH);
    if (m == Mode::Document) {
        if (layoutDirty_ || !layout_) RelayoutIfNeeded();
        return layout_ ? std::max(viewH, layout_->height + 20.0f) : viewH;
    }
    if (m == Mode::Source) {
        if (!source_.built) BuildSource();
        float lineH = 19.0f * (float)zoom_;
        float gutter = 60.0f * (float)zoom_;
        BuildSourceRows(std::max(32.0f, PageWidth() - gutter - 24.0f));
        return std::max(viewH, 16.0f + source_.rows.size() * lineH + 30.0f);
    }
    if (m == Mode::Json) {
        if (!json_.built) BuildJson();
        float rowH = 24.0f * (float)zoom_;
        return std::max(viewH, 24.0f + json_.rows.size() * rowH + 20.0f);
    }
    if (m == Mode::Grid) {
        if (!doc_->grid.sheets.empty()) {
            int s = std::min(grid_.sheet, (int)doc_->grid.sheets.size() - 1);
            if (s < 0 || s >= (int)grid_.rowHeights.size()) return viewH;
            float z = (float)zoom_;
            float h = 30.0f * z;
            auto& heights = grid_.rowHeights[(size_t)s];
            auto& sheet = doc_->grid.sheets[(size_t)s];
            for (size_t r = 1; r < sheet.rows.size(); r++)
                h += r < heights.size() ? heights[r] * z : 27.0f * z;
            return std::max(viewH, h + 10.0f);
        }
        return viewH;
    }
    if (m == Mode::Pdf && pdf_) {
        float z = (float)zoom_;
        float pageBase = std::min(PageWidth() - 96.0f, 900.0f);
        if (pageBase < 260) pageBase = std::max(180.0f, PageWidth() - 40.0f);
        float pageW = pageBase * z;
        float h = 26.0f;
        for (int i = 0; i < pdf_->PageCount(); i++) {
            float pw = 1, ph = 1;
            pdf_->PageSize(i, pw, ph);
            h += pageW * (ph / std::max(1.0f, pw)) + 20.0f;
        }
        return std::max(viewH, h + 10.0f);
    }
    // Image/canvas and welcome/error keep the visible canvas size. The image
    // itself is already the underlying bitmap and is rendered in full.
    return viewH;
}

DocView::WelcomeMetrics DocView::ComputeWelcomeMetrics(float viewW, float viewH) const {
    WelcomeMetrics m;
    m.contentW = std::min(kHomeMaxW, std::max(320.0f, viewW - 48.0f));
    m.viewX = (viewW - m.contentW) * 0.5f;
    m.compactHero = viewH < 600.0f;
    m.heroTop = m.compactHero ? 20.0f : kHomeHeroTop;
    m.heroBottom = m.compactHero ? 204.0f : kHomeHeroBottom;
    m.cardsTop = m.compactHero ? 218.0f : kHomeCardsTop;
    m.twoCol = m.contentW >= 760.0f;

    float recentBody = recent_.empty() ? 46.0f : kHomeListViewH;
    m.recentH = kHomeCardHeaderH + recentBody + kHomeCardPad;
    float workspaceBody = workspaces_.empty() ? 46.0f : kHomeListViewH;
    m.workspaceH = kHomeCardHeaderH + workspaceBody + kHomeCardPad;

    if (m.twoCol) {
        m.recentW = (m.contentW - kHomeGap) * 0.5f;
        m.workspaceW = m.recentW;
        m.recentX = m.viewX;
        m.workspaceX = m.recentX + m.recentW + kHomeGap;
        m.contentH = m.cardsTop + std::max(m.recentH, m.workspaceH) + 24.0f;
    } else {
        m.recentX = m.workspaceX = m.viewX;
        m.recentW = m.workspaceW = m.contentW;
        m.contentH = m.cardsTop + m.recentH + kHomeGap + m.workspaceH + 24.0f;
    }
    return m;
}

void DocView::WelcomeResetRecentPaging() {
    welcomeRecentShown_ = std::min((int)recent_.size(), kWelcomeRecentPage);
    welcomeRecentScroll_ = 0;
    scrollY_ = 0;
    hoverWelcome_ = -1;
}

void DocView::WelcomeScrollRecent(int delta) {
    if (recent_.empty() || delta == 0) return;
    if (welcomeRecentShown_ <= 0) WelcomeResetRecentPaging();
    float viewH = std::max(1.0f, welcomeRecentView_.h);
    float contentH = welcomeRecentContentH_;
    float maxScroll = std::max(0.0f, contentH - viewH);
    float step = delta / 120.0f * 60.0f;
    welcomeRecentScroll_ = Clampf(welcomeRecentScroll_ - step, 0, maxScroll);
    if (delta < 0 && welcomeRecentScroll_ >= maxScroll - 0.5f &&
        welcomeRecentShown_ < (int)recent_.size()) {
        welcomeRecentShown_ = std::min((int)recent_.size(),
                                       welcomeRecentShown_ + kWelcomeRecentPage);
        // The exact height depends on wrapped directory lines; keep the scroll
        // near the bottom and let the next paint clamp it precisely.
        welcomeRecentContentH_ += kWelcomeRecentPage * 38.0f;
        welcomeRecentScroll_ = std::min(welcomeRecentScroll_ + 18.0f,
                                        std::max(0.0f, welcomeRecentContentH_ - viewH));
    }
    hoverWelcome_ = -1;
    hoverWorkspace_ = -1;
    InvalidateAll();
}

void DocView::WelcomeScrollWorkspace(int delta) {
    if (workspaces_.empty() || delta == 0) return;
    float viewH = std::max(1.0f, welcomeWorkspaceView_.h);
    float maxScroll = std::max(0.0f, welcomeWorkspaceContentH_ - viewH);
    float step = delta / 120.0f * 60.0f;
    welcomeWorkspaceScroll_ = Clampf(welcomeWorkspaceScroll_ - step, 0, maxScroll);
    hoverWorkspace_ = -1;
    InvalidateAll();
}

void DocView::PaintWelcome(Canvas& c) {
    c.SetTranslateUi(true);
    float w = c.widthDip(), h = c.heightDip();
    if (welcomeRecentShown_ <= 0 && !recent_.empty())
        welcomeRecentShown_ = std::min((int)recent_.size(), kWelcomeRecentPage);
    auto m = ComputeWelcomeMetrics(w, h);
    float maxScroll = std::max(0.0f, m.contentH - h);
    scrollY_ = Clampf(scrollY_, 0.0f, maxScroll);

    welcomeRecentRects_.clear();
    welcomeRecentDeleteRects_.clear();
    welcomeRecentPaths_.clear();
    welcomeRecentRowIndices_.clear();
    welcomeRecentView_ = DRect{};
    welcomeWorkspaceView_ = DRect{};
    welcomeRecentCard_ = DRect{};
    welcomeWorkspaceCard_ = DRect{};
    welcomeClearRect_ = DRect{};
    welcomeMoreRect_ = DRect{};
    welcomeRecentContentH_ = 0;
    welcomeWorkspaceContentH_ = 0;
    welcomeWorkspaceRects_.clear();
    welcomeWorkspacePaths_.clear();
    welcomeWorkspaceClearRect_ = DRect{};

    c.PushClip(0, 0, w, h);
    c.SetTransform(0, -scrollY_);

    auto& p = c.palette();
    float cx = w * 0.5f;
    bool tight = m.compactHero;
    float logo = tight ? 52.0f : 64.0f;
    float titleY = m.heroTop + (tight ? 58.0f : 72.0f);
    float titleH = tight ? 30.0f : 38.0f;
    float titleSize = tight ? 24.0f : 30.0f;
    float subY = m.heroTop + (tight ? 90.0f : 110.0f);
    float subSize = tight ? 10.5f : 11.5f;
    c.FillRound(cx - logo / 2, m.heroTop, logo, logo, tight ? 12.0f : 14.0f, p.accentSoft);
    c.DrawText(L"md", cx - logo / 2, m.heroTop + (tight ? 5.0f : 7.0f), logo,
               tight ? 40.0f : 48.0f, tight ? 19.0f : 22.0f, p.accent, true, false,
               DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    c.DrawText(L"LitheMD", cx - 240, titleY, 480, titleH, titleSize, p.fg, true, false,
               DWRITE_TEXT_ALIGNMENT_CENTER);
    c.DrawText(L"轻量本地文档阅读器", cx - 240, subY, 480, tight ? 16.0f : 18.0f, subSize,
               p.muted, false, false, DWRITE_TEXT_ALIGNMENT_CENTER);

    float btnW = tight ? 160.0f : 180.0f;
    float btnH = tight ? 38.0f : 42.0f;
    float btnX = cx - btnW / 2, btnY = m.heroTop + (tight ? 112.0f : 144.0f);
    welcomeOpenRect_ = { btnX, btnY, btnW, btnH };
    welcomeFolderRect_ = DRect{};
    c.FillRound(btnX, btnY, btnW, btnH, 8, p.accent);
    c.DrawText(L"打开", btnX, btnY + 1, btnW - 18, btnH - 2, tight ? 13.0f : 14.0f,
               HexColor(L"#FFFFFF"), true, false, DWRITE_TEXT_ALIGNMENT_CENTER,
               DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    c.DrawIcon(L"\uE70D", btnX + btnW - 30, btnY + (btnH - 14) / 2, 14, 14, 9,
               HexColor(L"#FFFFFF"));
    c.DrawText(L"Ctrl+O 打开文件   ·   Ctrl+Shift+O 打开文件夹   ·   Ctrl+L 输入路径",
               cx - std::min(360.0f, m.contentW / 2), m.heroTop + (tight ? 158.0f : 196.0f),
               std::min(720.0f, m.contentW), tight ? 16.0f : 18.0f, tight ? 10.5f : 11.5f,
               p.muted, false, false, DWRITE_TEXT_ALIGNMENT_CENTER);

    auto drawCard = [&](float x, float y, float cw, float ch, const std::wstring& title,
                        DRect& clearRect, bool showClear) {
        c.FillRound(x, y, cw, ch, 12, p.surface);
        c.StrokeRound(x, y, cw, ch, 12, p.border);
        c.DrawText(title, x + 16, y + 9, std::max(40.0f, cw - 80), 22, 12.5f, p.fg, true);
        c.FillRect(x + 16, y + kHomeCardHeaderH - 1, std::max(0.0f, cw - 32), 1, p.border);
        clearRect = DRect{};
        if (showClear) {
            clearRect = { x + cw - 58, y + 8, 46, 24 };
            c.DrawText(L"清空", clearRect.x, clearRect.y, clearRect.w, clearRect.h, 11,
                       p.muted, false, false, DWRITE_TEXT_ALIGNMENT_CENTER,
                       DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    };

    float cardY = m.cardsTop;
    welcomeRecentCard_ = { m.recentX, cardY, m.recentW, m.recentH };
    drawCard(m.recentX, cardY, m.recentW, m.recentH, L"最近打开", welcomeClearRect_,
             !recent_.empty());
    welcomeRecentView_ = { m.recentX + 8, cardY + kHomeCardHeaderH,
                           std::max(40.0f, m.recentW - 16), kHomeListViewH };
    if (recent_.empty()) {
        welcomeRecentView_ = DRect{};
        c.DrawText(L"打开一个文件后，最近记录会显示在这里。", m.recentX + 16,
                   cardY + kHomeCardHeaderH + 12, std::max(40.0f, m.recentW - 32), 34,
                   11.5f, p.muted, false, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        int page = welcomeRecentShown_ > 0 ? welcomeRecentShown_ : kWelcomeRecentPage;
        int shown = std::min((int)recent_.size(), page);
        welcomeRecentShown_ = shown;
        std::vector<float> heights((size_t)shown, 38.0f);
        float textW = std::max(20.0f, welcomeRecentView_.w - 58.0f);
        welcomeRecentContentH_ = 8.0f;
        for (int i = 0; i < shown; i++) {
            bool wrap = c.MeasureText(PathDir(recent_[(size_t)i]), 10.5f, false, false,
                                      textW, false) > textW;
            heights[(size_t)i] = wrap ? 54.0f : 38.0f;
            welcomeRecentContentH_ += heights[(size_t)i];
        }
        welcomeRecentContentH_ += 28.0f; // list footer
        float maxScroll = std::max(0.0f, welcomeRecentContentH_ - welcomeRecentView_.h);
        welcomeRecentScroll_ = Clampf(welcomeRecentScroll_, 0.0f, maxScroll);
        c.PushClip(welcomeRecentView_.x, welcomeRecentView_.y, welcomeRecentView_.w,
                   welcomeRecentView_.h);
        float y = welcomeRecentView_.y + 4 - welcomeRecentScroll_;
        for (int i = 0; i < shown; i++) {
            float rowH = heights[(size_t)i];
            DRect r{ welcomeRecentView_.x + 4, y, welcomeRecentView_.w - 8, rowH };
            if (r.y + r.h < welcomeRecentView_.y || r.y > welcomeRecentView_.y + welcomeRecentView_.h) {
                y += rowH;
                continue;
            }
            if (hoverWelcome_ == i) c.FillRound(r.x, r.y, r.w, r.h, 6, p.hover);
            const std::wstring& path = recent_[(size_t)i];
            bool exists = FileExists(path);
            float nameW = std::max(20.0f, r.w - 50.0f);
            c.DrawText(PathName(path), r.x + 8, r.y + 2, nameW, 19, 12.5f,
                       exists ? p.fg : p.muted, true, false, DWRITE_TEXT_ALIGNMENT_LEADING,
                       DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
            c.DrawText(PathDir(path), r.x + 8, r.y + 20, nameW, rowH > 40 ? 30.0f : 16.0f,
                       10.5f, p.muted, false, false, DWRITE_TEXT_ALIGNMENT_LEADING,
                       DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
            DRect del{ r.x + r.w - 28, r.y + (rowH - 22) * 0.5f, 22, 22 };
            if (hoverWelcome_ == i) {
                c.FillRound(del.x, del.y, del.w, del.h, 4, p.hover);
                c.DrawText(L"×", del.x, del.y - 1, del.w, del.h, 14.0f, p.muted, false, false,
                           DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
            }
            welcomeRecentRects_.push_back(r);
            welcomeRecentDeleteRects_.push_back(del);
            welcomeRecentPaths_.push_back(path);
            welcomeRecentRowIndices_.push_back(i);
            y += rowH;
        }
        bool allLoaded = shown >= (int)recent_.size();
        c.DrawText(allLoaded ? L"已加载全部最近记录" : L"继续向下滑动加载更多",
                   welcomeRecentView_.x, y + 4, welcomeRecentView_.w, 22, 10.5f,
                   p.muted, false, false, DWRITE_TEXT_ALIGNMENT_CENTER,
                   DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        c.PopClip();
    }

    float workspaceY = m.twoCol ? cardY : cardY + m.recentH + kHomeGap;
    welcomeWorkspaceCard_ = { m.workspaceX, workspaceY, m.workspaceW, m.workspaceH };
    drawCard(m.workspaceX, workspaceY, m.workspaceW, m.workspaceH, L"最近工作区",
             welcomeWorkspaceClearRect_, !workspaces_.empty());
    welcomeWorkspaceView_ = { m.workspaceX + 8, workspaceY + kHomeCardHeaderH,
                              std::max(40.0f, m.workspaceW - 16), kHomeListViewH };
    if (workspaces_.empty()) {
        welcomeWorkspaceView_ = DRect{};
        c.DrawText(L"打开文件夹后，最近工作区会显示在这里。", m.workspaceX + 16,
                   workspaceY + kHomeCardHeaderH + 12, std::max(40.0f, m.workspaceW - 32),
                   34, 11.5f, p.muted, false, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        int count = (int)workspaces_.size();
        std::vector<float> heights((size_t)count, 38.0f);
        float textW = std::max(20.0f, welcomeWorkspaceView_.w - 24.0f);
        welcomeWorkspaceContentH_ = 8.0f;
        for (int i = 0; i < count; i++) {
            bool wrap = c.MeasureText(PathDir(workspaces_[(size_t)i]), 10.5f, false, false,
                                      textW, false) > textW;
            heights[(size_t)i] = wrap ? 54.0f : 38.0f;
            welcomeWorkspaceContentH_ += heights[(size_t)i];
        }
        float maxScroll = std::max(0.0f, welcomeWorkspaceContentH_ - welcomeWorkspaceView_.h);
        welcomeWorkspaceScroll_ = Clampf(welcomeWorkspaceScroll_, 0.0f, maxScroll);
        c.PushClip(welcomeWorkspaceView_.x, welcomeWorkspaceView_.y, welcomeWorkspaceView_.w,
                   welcomeWorkspaceView_.h);
        float y = welcomeWorkspaceView_.y + 4 - welcomeWorkspaceScroll_;
        for (int i = 0; i < count; i++) {
            float rowH = heights[(size_t)i];
            DRect r{ welcomeWorkspaceView_.x + 4, y, welcomeWorkspaceView_.w - 8, rowH };
            if (r.y + r.h < welcomeWorkspaceView_.y || r.y > welcomeWorkspaceView_.y + welcomeWorkspaceView_.h) {
                y += rowH;
                continue;
            }
            if (hoverWorkspace_ == i) c.FillRound(r.x, r.y, r.w, r.h, 6, p.hover);
            const std::wstring& path = workspaces_[(size_t)i];
            bool exists = DirExists(path);
            float textW2 = std::max(20.0f, r.w - 16.0f);
            c.DrawText(PathName(path), r.x + 8, r.y + 2, textW2, 19, 12.5f,
                       exists ? p.fg : p.muted, true, false, DWRITE_TEXT_ALIGNMENT_LEADING,
                       DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
            c.DrawText(PathDir(path), r.x + 8, r.y + 20, textW2, rowH > 40 ? 30.0f : 16.0f,
                       10.5f, p.muted, false, false, DWRITE_TEXT_ALIGNMENT_LEADING,
                       DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
            welcomeWorkspaceRects_.push_back(r);
            welcomeWorkspacePaths_.push_back(path);
            y += rowH;
        }
        c.PopClip();
    }

    auto drawListScroll = [&](const DRect& view, float contentH, float scroll) {
        if (view.w < 8 || view.h < 8 || contentH <= view.h + 0.5f) return;
        float trackX = view.x + view.w + 2;
        Color track = p.border;
        track.a = 0.45f;
        c.FillRound(trackX, view.y, 3, view.h, 1.5f, track);
        float thumbH = std::max(24.0f, view.h * view.h / contentH);
        float maxScroll = std::max(1.0f, contentH - view.h);
        float thumbY = view.y + scroll / maxScroll * (view.h - thumbH);
        Color thumb = p.muted;
        thumb.a = 0.55f;
        c.FillRound(trackX, thumbY, 3, thumbH, 1.5f, thumb);
    };
    drawListScroll(welcomeRecentView_, welcomeRecentContentH_, welcomeRecentScroll_);
    drawListScroll(welcomeWorkspaceView_, welcomeWorkspaceContentH_, welcomeWorkspaceScroll_);

    c.ResetTransform();
    c.PopClip();
}

void DocView::PaintError(Canvas& c) {
    c.SetTranslateUi(true);
    float w = c.widthDip(), h = c.heightDip();
    float cardW = std::min(660.0f, w - 40);
    float cardX = (w - cardW) / 2;
    float cardY = std::max(40.0f, (h - 380.0f) / 2);
    c.FillRound(cardX, cardY, cardW, 340, 12, c.palette().surface);
    c.StrokeRound(cardX, cardY, cardW, 340, 12, c.palette().border);
    c.FillCircle(cardX + 46, cardY + 46, 18, c.palette().dangerSoft);
    c.DrawText(L"!", cardX + 28, cardY + 30, 36, 32, 22, c.palette().danger, true, false,
               DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    c.DrawText(L"无法打开文件", cardX + 76, cardY + 30, cardW - 100, 34, 20, c.palette().fg, true);
    c.DrawText(errorMessage_, cardX + 30, cardY + 84, cardW - 60, 130, 13, c.palette().muted, false, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
    c.DrawText(errorPath_, cardX + 30, cardY + 218, cardW - 60, 20, 11.5f, c.palette().muted, false, true,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);

    float by = cardY + 264, bh = 38;
    errorOpenRect_ = { cardX + 30, by, 148, bh };
    c.FillRound(errorOpenRect_.x, errorOpenRect_.y, errorOpenRect_.w, errorOpenRect_.h, 7, c.palette().accent);
    c.DrawText(L"打开其他文件", errorOpenRect_.x, by + 1, errorOpenRect_.w, bh - 2, 13, HexColor(L"#FFFFFF"), true, false,
               DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    errorExternalRect_ = { cardX + 190, by, 180, bh };
    c.StrokeRound(errorExternalRect_.x, errorExternalRect_.y, errorExternalRect_.w, errorExternalRect_.h, 7, c.palette().border);
    c.DrawText(L"用系统默认程序打开", errorExternalRect_.x, by + 1, errorExternalRect_.w, bh - 2, 12.5f,
               c.palette().fg, false, false, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    errorSourceRect_ = { 0, 0, 0, 0 };
}

void DocView::PaintDocument(Canvas& c) {
    if (!layout_) return;
    c.PushClip(0, 0, c.widthDip(), c.heightDip());
    c.SetTransform(0, -scrollY_);
    float z = (float)zoom_;
    auto& p = c.palette();
    for (auto& g : layout_->groups) {
        if (g.y + g.h < scrollY_ - 80 || g.y > scrollY_ + c.heightDip() + 80) continue;
        Color bg = g.role == 1 ? p.accentSoft : p.surface;
        c.FillRound(g.x, g.y, g.w, g.h, 12 * z, bg);
        if (g.role == 2) c.StrokeRound(g.x, g.y, g.w, g.h, 12 * z, p.border);
    }
    PaintTextHighlights(c);
    for (auto& it : layout_->items) {
        if (it.y + it.h < scrollY_ - 100 || it.y > scrollY_ + c.heightDip() + 100) continue;
        const Block& b = *it.block;
        switch (b.kind) {
        case BlockKind::Heading:
            if (b.level <= 1) c.FillRect(it.x, it.contentY + it.h - 1, it.w, 1, p.border);
            it.main.Draw(c, it.contentX, it.contentY);
            break;
        case BlockKind::Paragraph:
        case BlockKind::Quote:
            if (b.quote) c.FillRect(it.x, it.y + 4, 3 * z, it.h - 8, p.quoteBar);
            it.main.Draw(c, it.contentX, it.contentY);
            break;
        case BlockKind::ListItem: {
            float ix = it.contentX - 24 * z;
            float cy = it.y + 3 * z;
            if (b.task) {
                float sz = 13 * z;
                c.StrokeRound(ix, cy + 2, sz, sz, 3 * z, b.checked ? p.accent : p.muted);
                if (b.checked)
                    c.DrawText(L"✓", ix, cy, sz, sz, 10.5f * z, p.accent, true, false,
                               DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            } else if (b.ordered) {
                c.DrawText(std::to_wstring(b.number) + L".", ix, cy, 20 * z, 22 * z, 12.5f * z, p.muted, false, false,
                           DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
            } else {
                c.FillCircle(ix + 7 * z, cy + 10 * z, 2.2f * z, p.muted);
            }
            it.main.Draw(c, it.contentX, it.contentY);
            break;
        }
        case BlockKind::Code:
        case BlockKind::OutputText: {
            Color bg = b.kind == BlockKind::OutputText ? (b.error ? p.dangerSoft : p.codeBg) : p.codeBg;
            c.FillRound(it.x, it.y, it.w, it.h, 8 * z, bg);
            c.StrokeRound(it.x, it.y, it.w, it.h, 8 * z, b.error ? p.danger : p.border);
            if (!it.label.empty())
                c.DrawText(it.label, it.x + 14 * z, it.y + 6 * z, it.w - 28 * z, 16 * z, 10.5f * z,
                           b.error ? p.danger : p.muted, true, true,
                           DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
            it.main.Draw(c, it.contentX, it.contentY);
            break;
        }
        case BlockKind::Math:
            if (b.math) {
                DrawMath(*b.math, c.target(), Engine::Instance().DWrite(), it.mathX, it.mathBaseline,
                         it.mathSize, b.output ? p.muted : p.fg);
                if (!it.label.empty())
                    c.DrawText(it.label, it.x, it.y, it.w, 16 * z, 10.5f * z, p.muted, true, false,
                               DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            break;
        case BlockKind::Table:
            for (auto& cell : it.cells) {
                if (cell.header) c.FillRect(cell.x, cell.y, cell.w, cell.h, p.tableHead);
                c.StrokeRect(cell.x, cell.y, cell.w, cell.h, p.border, 1.0f);
                cell.rt.Draw(c, cell.x + 8 * z, cell.y + 7 * z);
            }
            break;
        case BlockKind::Image:
        case BlockKind::OutputImage: {
            if (b.kind == BlockKind::OutputImage) {
                c.FillRound(it.x, it.y, it.w, it.h, 8 * z, p.surface);
                c.StrokeRound(it.x, it.y, it.w, it.h, 8 * z, p.border);
                if (!it.label.empty())
                    c.DrawText(it.label, it.x + 14 * z, it.y + 7 * z, it.w - 28 * z, 16 * z, 10.5f * z, p.muted, true, false);
            }
            if (it.imageOk && it.image) {
                if (auto* bmp = c.Image(*it.image))
                    c.target()->DrawBitmap(bmp, D2D1::RectF(it.imageX, it.imageY, it.imageX + it.imageW,
                                                            it.imageY + it.imageH), 1.0f,
                                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            } else {
                float ph = b.kind == BlockKind::OutputImage ? 44 * z : 32 * z;
                float py = it.contentY;
                c.FillRound(it.x, py, it.w, ph, 6 * z, p.surface);
                c.StrokeRound(it.x, py, it.w, ph, 6 * z, p.border);
                std::wstring label = b.image ? L"图片无法显示" : (b.text.empty() ? L"图片" : b.text);
                if (it.label.empty()) it.label = L"";
                c.DrawText(label.empty() ? L"图片无法显示" : label, it.x + 12 * z, py, it.w - 24 * z, ph, 11.5f * z,
                           p.muted, false, false, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
            }
            break;
        }
        case BlockKind::Hr:
            c.FillRect(it.x, it.y + it.h / 2, it.w, 1, p.border);
            break;
        case BlockKind::Session:
            c.FillRound(it.x, it.y, it.w, it.h, 10 * z, p.surface);
            c.StrokeRound(it.x, it.y, it.w, it.h, 10 * z, p.border);
            c.DrawText(it.label, it.contentX, it.y + 12 * z, it.w - 36 * z, 16 * z, 11 * z, p.accent, true);
            it.main.Draw(c, it.contentX, it.contentY);
            if (!it.badge.empty())
                c.DrawText(it.badge, it.contentX, it.contentY + it.main.height + 4 * z, it.w - 36 * z, 18 * z,
                           11.5f * z, p.muted, false, false);
            break;
        case BlockKind::ChatHeader: {
            Color lc = b.chatRole == 1 ? p.accent : p.muted;
            DWRITE_TEXT_ALIGNMENT align = it.labelRight ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING;
            c.DrawText(it.label, it.contentX, it.y + 5 * z, it.w, 17 * z, 11.5f * z, lc, true, false, align);
            if (!it.badge.empty())
                c.DrawText(it.badge, it.contentX, it.y + 5 * z, it.w, 17 * z, 10.5f * z, p.muted, false, false,
                           DWRITE_TEXT_ALIGNMENT_TRAILING);
            break;
        }
        case BlockKind::Reasoning:
        case BlockKind::Tool: {
            c.FillRound(it.x, it.y, it.w, it.h, 8 * z, p.surface);
            c.StrokeRound(it.x, it.y, it.w, it.h, 8 * z, p.border);
            if (it.chevron) {
                float cy = it.y + 15 * z;
                float cx = it.x + 12 * z;
                if (b.expanded) {
                    c.DrawLine(cx, cy, cx + 4 * z, cy + 4 * z, p.muted, 1.5f);
                    c.DrawLine(cx + 4 * z, cy + 4 * z, cx + 8 * z, cy, p.muted, 1.5f);
                } else {
                    c.DrawLine(cx, cy, cx + 4 * z, cy + 4 * z, p.muted, 1.5f);
                    c.DrawLine(cx + 4 * z, cy + 4 * z, cx, cy + 8 * z, p.muted, 1.5f);
                }
            }
            c.DrawText(it.label, it.contentX, it.y + 9 * z, it.w - (it.contentX - it.x) - 14 * z, 18 * z,
                       11.5f * z, b.kind == BlockKind::Tool ? p.codeType : p.muted, true, false,
                       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
            if (b.expanded) it.main.Draw(c, it.contentX, it.contentY);
            break;
        }
        case BlockKind::Banner:
            c.FillRound(it.x, it.y, it.w, it.h, 8 * z, p.accentSoft);
            if (!it.label.empty())
                c.DrawText(it.label, it.contentX, it.y + 10 * z, it.w - 28 * z, 16 * z, 11 * z, p.accent, true);
            it.main.Draw(c, it.contentX, it.contentY);
            break;
        default:
            if (it.main.layout) it.main.Draw(c, it.contentX, it.contentY);
            break;
        }
    }
    c.PopClip();
}

void DocView::PaintTextHighlights(Canvas& c) {
    if (!doc_ || !layout_ || (textHighlights_.empty() && textHighlightPending_.empty())) return;
    auto drawRanges = [&](const std::vector<TextHighlight>& ranges, bool pending) {
        for (auto& range : ranges) {
            if (range.block < 0 || range.block >= (int)doc_->blocks.size()) continue;
            const Block* block = &doc_->blocks[(size_t)range.block];
            for (auto& item : layout_->items) {
                if (item.block != block || !item.main.layout) continue;
                int textLen = (int)item.main.text.size();
                if (textLen <= 0) break;
                int start = std::clamp(range.start, 0, textLen);
                int length = std::clamp(range.length, 0, textLen - start);
                if (length <= 0) break;
                UINT32 actual = 0;
                item.main.layout->HitTestTextRange((UINT32)start, (UINT32)length, 0, 0,
                                                   nullptr, 0, &actual);
                if (actual == 0) break;
                std::vector<DWRITE_HIT_TEST_METRICS> metrics(actual);
                if (FAILED(item.main.layout->HitTestTextRange((UINT32)start, (UINT32)length, 0, 0,
                                                              metrics.data(), actual, &actual)))
                    break;
                Color color = pending ? CurrentInkColor() : range.color;
                color.a = std::clamp(color.a * 0.55f, 0.12f, 0.45f);
                for (UINT32 i = 0; i < actual; i++) {
                    auto& m = metrics[i];
                    if (m.width <= 0 || m.height <= 0) continue;
                    c.FillRect(item.contentX + m.left, item.contentY + m.top, m.width, m.height, color);
                }
                break;
            }
        }
    };
    drawRanges(textHighlights_, false);
    drawRanges(textHighlightPending_, true);
}

void DocView::CollectTextHighlightHits(float sx, float sy, float radius,
                                       std::vector<int>& hits) const {
    if (!doc_ || !layout_) return;
    for (int i = 0; i < (int)textHighlights_.size(); i++) {
        auto& h = textHighlights_[(size_t)i];
        if (h.block < 0 || h.block >= (int)doc_->blocks.size()) continue;
        for (auto& item : layout_->items) {
            if (item.block != &doc_->blocks[(size_t)h.block] || !item.main.layout) continue;
            int textLen = (int)item.main.text.size();
            int start = std::clamp(h.start, 0, textLen);
            int length = std::clamp(h.length, 0, textLen - start);
            if (length <= 0) break;
            UINT32 count = 0;
            item.main.layout->HitTestTextRange((UINT32)start, (UINT32)length, 0, 0,
                                               nullptr, 0, &count);
            if (!count) break;
            std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
            if (FAILED(item.main.layout->HitTestTextRange((UINT32)start, (UINT32)length, 0, 0,
                                                          metrics.data(), count, &count)))
                break;
            bool touched = false;
            for (auto& m : metrics) {
                float rx = item.contentX + m.left;
                float ry = item.contentY + m.top - scrollY_;
                if (sx >= rx - radius && sx <= rx + m.width + radius &&
                    sy >= ry - radius && sy <= ry + m.height + radius) {
                    touched = true;
                    break;
                }
            }
            if (touched) hits.push_back(i);
            break;
        }
    }
}

bool DocView::BeginTextHighlight(float sx, float sy) {
    if (!doc_ || sourceMode_ || !layout_) return false;
    int itemIndex = HitBlock(sx, sy + scrollY_);
    if (itemIndex < 0 || itemIndex >= (int)layout_->items.size()) return false;
    auto& item = layout_->items[(size_t)itemIndex];
    if (!item.main.layout || item.main.text.empty()) return false;
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(item.main.layout->HitTestPoint(sx - item.contentX, sy + scrollY_ - item.contentY,
                                              &trailing, &inside, &metrics)))
        return false;
    int block = (int)(item.block - doc_->blocks.data());
    if (block < 0 || block >= (int)doc_->blocks.size()) return false;
    int pos = std::clamp((int)metrics.textPosition, 0, (int)item.main.text.size());
    textHighlighting_ = true;
    textHighlightStartBlock_ = textHighlightEndBlock_ = block;
    textHighlightStartPos_ = textHighlightEndPos_ = pos;
    textHighlightPending_.clear();
    if (GetCapture() != hwnd_) SetCapture(hwnd_);
    return true;
}

void DocView::UpdateTextHighlight(float sx, float sy) {
    if (!textHighlighting_ || !doc_ || !layout_) return;
    int itemIndex = HitBlock(sx, sy + scrollY_);
    if (itemIndex < 0 || itemIndex >= (int)layout_->items.size()) return;
    auto& item = layout_->items[(size_t)itemIndex];
    if (!item.main.layout || item.main.text.empty()) return;
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(item.main.layout->HitTestPoint(sx - item.contentX, sy + scrollY_ - item.contentY,
                                              &trailing, &inside, &metrics)))
        return;
    int block = (int)(item.block - doc_->blocks.data());
    if (block < 0 || block >= (int)doc_->blocks.size()) return;
    textHighlightEndBlock_ = block;
    textHighlightEndPos_ = std::clamp((int)metrics.textPosition, 0, (int)item.main.text.size());

    int b0 = textHighlightStartBlock_, b1 = textHighlightEndBlock_;
    int p0 = textHighlightStartPos_, p1 = textHighlightEndPos_;
    if (b0 > b1 || (b0 == b1 && p0 > p1)) {
        std::swap(b0, b1);
        std::swap(p0, p1);
    }
    textHighlightPending_.clear();
    auto addRange = [&](int block, int start, int length) {
        if (length <= 0) return;
        TextHighlight h;
        h.block = block;
        h.start = start;
        h.length = length;
        h.color = CurrentInkColor();
        textHighlightPending_.push_back(h);
    };
    auto textLength = [&](int block) {
        for (auto& it : layout_->items)
            if (it.block == &doc_->blocks[(size_t)block]) return (int)it.main.text.size();
        return 0;
    };
    if (b0 == b1) {
        addRange(b0, p0, p1 - p0);
    } else {
        addRange(b0, p0, textLength(b0) - p0);
        for (int block = b0 + 1; block < b1; block++) addRange(block, 0, textLength(block));
        addRange(b1, 0, p1);
    }
    InvalidateAll();
}

void DocView::FinishTextHighlight() {
    if (!textHighlighting_) return;
    textHighlighting_ = false;
    if (GetCapture() == hwnd_) ReleaseCapture();
    if (!textHighlightPending_.empty()) {
        InkPushUndo();
        textHighlights_.insert(textHighlights_.end(), textHighlightPending_.begin(), textHighlightPending_.end());
        textHighlightPending_.clear();
        MarkNotesDirty();
    }
    InvalidateAll();
}

void DocView::PaintSource(Canvas& c) {
    if (!source_.built) BuildSource();
    float z = (float)zoom_;
    float lineH = 19.0f * z;
    float gutter = 60.0f * z;
    float font = 13.0f * z;
    float avail = std::max(32.0f, c.widthDip() - gutter - 24.0f);
    BuildSourceRows(avail);
    auto& p = c.palette();
    c.FillRect(0, 0, gutter, c.heightDip(), p.surface);
    c.FillRect(gutter - 1, 0, 1, c.heightDip(), p.border);
    c.PushClip(gutter, 0, c.widthDip() - gutter, c.heightDip());
    int first = std::max(0, (int)((scrollY_ - 16) / lineH) - 1);
    int last = std::min((int)source_.rows.size(), first + (int)(c.heightDip() / lineH) + 4);
    for (int i = first; i < last; i++) {
        float y = 16 + i * lineH - scrollY_;
        c.DrawText(source_.rows[(size_t)i], gutter + 12 * z, y, avail, lineH, font,
                   p.fg, false, true, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    }
    c.PopClip();
    c.PushClip(0, 0, gutter, c.heightDip());
    for (int i = first; i < last; i++) {
        int logical = i < (int)source_.rowLines.size() ? source_.rowLines[(size_t)i] : -1;
        if (logical < 0) continue;
        if (i > 0 && i - 1 < (int)source_.rowLines.size() &&
            source_.rowLines[(size_t)(i - 1)] == logical) continue;
        float y = 16 + i * lineH - scrollY_;
        c.DrawText(std::to_wstring(logical + 1), 4, y, gutter - 12, lineH, font * 0.92f, p.muted, false, true,
                   DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    }
    c.PopClip();
}

void DocView::PaintJson(Canvas& c) {
    if (!json_.built) BuildJson();
    float z = (float)zoom_;
    float rowH = 24.0f * z;
    float font = 13.0f * z;
    float indent = 18.0f * z;
    auto& p = c.palette();
    c.PushClip(0, 0, c.widthDip(), c.heightDip());
    int first = std::max(0, (int)((scrollY_ - 16) / rowH) - 1);
    int last = std::min((int)json_.rows.size(), first + (int)(c.heightDip() / rowH) + 4);
    for (int i = first; i < last; i++) {
        auto& r = json_.rows[(size_t)i];
        float y = 16 + i * rowH - scrollY_;
        if (i == hoverJsonRow_) c.FillRect(0, y, c.widthDip(), rowH, p.hover);
        float x = 16 + r.depth * indent - scrollX_;
        if (r.container && !r.closing) {
            float ax = x + 2, ay = y + rowH / 2 - 4 * z;
            if (r.expanded) {
                c.DrawLine(ax, ay + 1 * z, ax + 8 * z, ay + 1 * z, p.muted, 1.2f);
                c.DrawLine(ax + 8 * z, ay + 1 * z, ax + 4 * z, ay + 6 * z, p.muted, 1.2f);
                c.DrawLine(ax + 4 * z, ay + 6 * z, ax, ay + 1 * z, p.muted, 1.2f);
            } else {
                c.DrawLine(ax, ay, ax + 5 * z, ay + 4 * z, p.muted, 1.2f);
                c.DrawLine(ax + 5 * z, ay + 4 * z, ax, ay + 8 * z, p.muted, 1.2f);
            }
        }
        float tx = x + (r.container ? 14 * z : 2 * z);
        if (r.closing) {
            std::wstring close = r.valueType == 4 ? L"}" : L"]";
            if (r.hasComma) close += L",";
            c.DrawText(close, tx, y, 200, rowH, font, p.muted, false, true,
                       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
            continue;
        }
        float cx = tx;
        if (!r.key.empty()) {
            std::wstring key = r.key + L": ";
            c.DrawText(key, cx, y, 2000, rowH, font, p.jsonKey, false, true,
                       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
            cx += c.MeasureText(key, font, false, true, 100000, false);
        }
        std::wstring value = r.value;
        if (r.container) {
            value = (r.valueType == 4 ? L"{" : L"[");
            if (!r.expanded) {
                value += (r.valueType == 4 ? L"…}" : L"…]");
                if (r.hasComma) value += L",";
            }
        } else if (r.hasComma) value += L",";
        Color vc = p.fg;
        switch (r.valueType) {
        case 0: vc = p.jsonNull; break;
        case 1: vc = p.jsonBool; break;
        case 2: vc = p.jsonNumber; break;
        case 3: vc = p.jsonString; break;
        default: vc = p.muted; break;
        }
        c.DrawText(value, cx, y, 4000, rowH, font, vc, false, true,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    }
    c.PopClip();
}

void DocView::PaintGrid(Canvas& c) {
    if (!doc_ || doc_->grid.sheets.empty()) {
        c.DrawText(L"表格为空", c.widthDip() / 2 - 100, c.heightDip() / 2 - 10, 200, 20, 13,
                   c.palette().muted, false, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    int s = std::min(grid_.sheet, (int)doc_->grid.sheets.size() - 1);
    auto& sheet = doc_->grid.sheets[(size_t)s];
    auto& p = c.palette();
    float z = (float)zoom_;
    float numW = 52 * z;
    float headerH = 30 * z;
    int cols = MaxCols(sheet);
    float tabH = (!exporting_ && doc_->grid.sheets.size() > 1) ? 30 * z : 0;
    float bodyH = c.heightDip() - tabH;
    c.FillRect(0, 0, c.widthDip(), c.heightDip(), p.bg);
    c.PushClip(numW, 0, c.widthDip() - numW, headerH);
    float x = numW - scrollX_;
    for (int col = 0; col < cols; col++) {
        float cw = col < (int)grid_.colWidths[s].size() ? grid_.colWidths[s][col] * z : 84 * z;
        if (x + cw < numW || x > c.widthDip()) { x += cw; continue; }
        c.FillRect(x, 0, cw, headerH, p.tableHead);
        c.StrokeRect(x, 0, cw, headerH, p.border, 1);
        c.DrawText(CellString(sheet, 0, col), x + 8 * z, 0, cw - 16 * z, headerH, 12.5f * z, p.fg, true, false,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        x += cw;
    }
    c.PopClip();

    c.PushClip(numW, headerH, c.widthDip() - numW, bodyH - headerH);
    float y = headerH - scrollY_;
    for (size_t r = 1; r < sheet.rows.size(); r++) {
        float rh = r < grid_.rowHeights[s].size() ? grid_.rowHeights[s][r] * z : 27 * z;
        if (y + rh < headerH) { y += rh; continue; }
        if (y > bodyH) break;
        float cx = numW - scrollX_;
        for (int col = 0; col < cols; col++) {
            float cw = col < (int)grid_.colWidths[s].size() ? grid_.colWidths[s][col] * z : 84 * z;
            if (cx + cw < numW) { cx += cw; continue; }
            if (cx > c.widthDip()) break;
            bool sel = selectedRow_ == (int)r && selectedCol_ == col;
            if (sel) c.FillRect(cx, y, cw, rh, p.selection);
            c.StrokeRect(cx, y, cw, rh, p.border, 1);
            c.DrawText(CellString(sheet, (int)r, col), cx + 8 * z, y, cw - 16 * z, rh, 12.5f * z, p.fg, false, false,
                       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
            cx += cw;
        }
        y += rh;
    }
    c.PopClip();

    c.FillRect(0, 0, numW, bodyH, p.surface);
    c.FillRect(numW - 1, 0, 1, bodyH, p.border);
    c.PushClip(0, headerH, numW, bodyH - headerH);
    float ry = headerH - scrollY_;
    for (size_t r = 1; r < sheet.rows.size(); r++) {
        float rh = r < grid_.rowHeights[s].size() ? grid_.rowHeights[s][r] * z : 27 * z;
        if (ry + rh >= headerH && ry <= bodyH) {
            bool sel = selectedRow_ == (int)r;
            c.DrawText(std::to_wstring(r), 4, ry, numW - 10, rh, 11.5f * z,
                       sel ? p.accent : p.muted, sel, false,
                       DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
            c.FillRect(0, ry + rh - 1, numW - 1, 1, p.border);
        }
        ry += rh;
    }
    c.PopClip();
    c.FillRect(0, headerH - 1, c.widthDip(), 1, p.border);

    if (tabH > 0) {
        float ty = bodyH;
        c.FillRect(0, ty, c.widthDip(), tabH, p.surface);
        c.FillRect(0, ty, c.widthDip(), 1, p.border);
        float tx = 8;
        for (size_t i = 0; i < doc_->grid.sheets.size(); i++) {
            std::wstring name = doc_->grid.sheets[i].name.empty() ? L"Sheet" : doc_->grid.sheets[i].name;
            float tw = c.MeasureText(name, 11.5f, false, false, 400, false) + 28;
            bool active = (int)i == grid_.sheet;
            if (active) c.FillRound(tx, ty + 4, tw, tabH - 8, 5, p.accentSoft);
            c.DrawText(name, tx, ty + 4, tw, tabH - 8, 11.5f, active ? p.accent : p.muted, active, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
            tx += tw + 4;
        }
    }
    if (doc_->grid.truncated && !doc_->grid.note.empty())
        c.DrawText(doc_->grid.note, numW + 12, bodyH - 22 * z, c.widthDip() - numW - 24, 18 * z, 10.5f * z,
                   p.muted, false, false, DWRITE_TEXT_ALIGNMENT_LEADING);

    // Show the active resize edge so the draggable area is discoverable even
    // without moving the pointer onto exactly the right pixel.
    int sepCol = dragging_ == 1 ? dragIndex_ : hoverGridColSep_;
    if (sepCol >= 0) {
        float xx = numW - scrollX_;
        for (int col = 0; col <= sepCol && col < MaxCols(sheet); col++) {
            float cw = col < (int)grid_.colWidths[s].size() ? grid_.colWidths[s][col] * z : 84 * z;
            xx += cw;
        }
        if (xx >= numW && xx <= c.widthDip()) {
            Color hl = p.accent;
            hl.a = 0.85f;
            c.FillRect(xx - 1, 0, 2, headerH, hl);
        }
    }
    int sepRow = dragging_ == 2 ? dragIndex_ : hoverGridRowSep_;
    if (sepRow >= 0) {
        float yy = headerH - scrollY_;
        for (int r = 1; r <= sepRow && r < (int)sheet.rows.size(); r++) {
            float rh = r < (int)grid_.rowHeights[s].size() ? grid_.rowHeights[s][r] * z : 27 * z;
            yy += rh;
        }
        if (yy >= headerH && yy <= bodyH) {
            Color hl = p.accent;
            hl.a = 0.55f;
            c.FillRect(0, yy - 1, c.widthDip(), 2, hl);
        }
    }
}

void DocView::PaintPdf(Canvas& c) {
    c.SetTranslateUi(true);
    if (!pdf_) {
        c.DrawText(pdfState_.error.empty() ? L"PDF 渲染不可用" : pdfState_.error, 40, 40,
                   c.widthDip() - 80, 80, 14, c.palette().muted, false, false,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    if (!pdfState_.error.empty()) {
        c.DrawText(pdfState_.error, 40, 40, c.widthDip() - 80, 80, 14, c.palette().danger, false, false,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    float z = (float)zoom_;
    float pageBase = std::min(c.widthDip() - 96.0f, 900.0f);
    if (pageBase < 260) pageBase = std::max(180.0f, c.widthDip() - 40.0f);
    float pageW = pageBase * z;
    float gap = 20.0f;
    float y = 26.0f - scrollY_;
    auto& p = c.palette();
    for (int i = 0; i < pdf_->PageCount(); i++) {
        float pw = 1, ph = 1;
        pdf_->PageSize(i, pw, ph);
        float pageH = pageW * (ph / std::max(1.0f, pw));
        if (y + pageH < -40 || y > c.heightDip() + 40) { y += pageH + gap; continue; }
        float x = (c.widthDip() - pageW) / 2 - (scrollX_ > 0 ? scrollX_ : 0);
        c.FillRound(x + 2, y + 3, pageW, pageH, 2, HexColor(L"#000000", 0.12f));
        c.FillRect(x, y, pageW, pageH, HexColor(L"#FFFFFF"));
        c.StrokeRect(x, y, pageW, pageH, p.border, 1);
        int maxPx = exporting_ ? 12000 : 2400;
        int targetPx = (int)Clampf(pageW * c.scale() * 1.35f, 300.0f, (float)maxPx);
        if (i >= (int)pdfState_.pages.size()) {
            pdfState_.pages.resize(pdf_->PageCount());
            pdfState_.renderedWidth.resize(pdf_->PageCount(), 0);
        }
        if ((!pdfState_.pages[(size_t)i] || pdfState_.renderedWidth[(size_t)i] != targetPx) && !pdfRendering_) {
            pdfRendering_ = true;
            std::vector<uint8_t> png;
            std::wstring err;
            bool ok = pdf_->RenderPage(i, targetPx, png, err) && !png.empty();
            pdfRendering_ = false;
            if (ok) {
                auto img = std::make_shared<ImageRef>();
                img->bytes = std::move(png);
                img->mime = L"image/png";
                img->cacheKey = L"pdf:" + doc_->path + L":" + std::to_wstring(i) + L":" + std::to_wstring(targetPx);
                pdfState_.pages[(size_t)i] = img;
                pdfState_.renderedWidth[(size_t)i] = targetPx;
                int rendered = 0;
                for (auto& p : pdfState_.pages) if (p) rendered++;
                if (rendered > 8) {
                    for (int k = 0; k < (int)pdfState_.pages.size(); k++) {
                        if (std::abs(k - i) > 2) {
                            pdfState_.pages[(size_t)k].reset();
                            pdfState_.renderedWidth[(size_t)k] = 0;
                        }
                    }
                    c.ClearImageCache();
                }
            } else if (!err.empty()) {
                pdfState_.error = err;
                break;
            }
        }
        if (pdfState_.pages[(size_t)i]) {
            if (auto* bmp = c.Image(*pdfState_.pages[(size_t)i])) {
                c.PushClip(x, y, pageW, pageH);
                c.target()->DrawBitmap(bmp, D2D1::RectF(x, y, x + pageW, y + pageH), 1.0f,
                                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                c.PopClip();
            }
        }
        PaintPdfHighlights(c, i, x, y, pageW, pageH);
        if (!exporting_)
            c.DrawText(L"第 " + std::to_wstring(i + 1) + L" / " + std::to_wstring(pdf_->PageCount()) + L" 页",
                       x, y + pageH + 2, pageW, 16, 10, p.muted, false, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
        y += pageH + gap;
    }
}

void DocView::PaintImage(Canvas& c) {
    c.SetTranslateUi(true);
    if (!doc_ || !doc_->image) {
        c.DrawText(L"无法显示图片", 40, 40, c.widthDip() - 80, 40, 14, c.palette().muted, false, false,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    float x = 0, y = 0, dw = 0, dh = 0;
    if (!ImageDisplayRect(x, y, dw, dh)) {
        c.DrawText(L"无法显示图片", 40, 40, c.widthDip() - 80, 40, 14, c.palette().muted, false, false,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    auto* bmp = c.Image(*doc_->image);
    if (!bmp) {
        c.DrawText(L"无法解码图片", 40, 40, c.widthDip() - 80, 40, 14, c.palette().muted, false, false,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }
    if (!exporting_)
        c.FillRound(x - 4, y - 4, dw + 8, dh + 8, 4, c.palette().surface);
    c.target()->DrawBitmap(bmp, D2D1::RectF(x, y, x + dw, y + dh), 1.0f,
                           D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void DocView::UpdateCopyHover(float x, float y) {
    CopyTarget old = copyHover_;
    copyHover_.active = false;
    copyHover_.block = nullptr;
    copyHover_.cellIndex = -1;
    copyHover_.row = copyHover_.col = -1;

    Mode m = Mode::Welcome;
    if (!errorMessage_.empty()) m = Mode::Error;
    else if (!doc_) m = Mode::Welcome;
    else if (sourceMode_) m = Mode::Source;
    else if (doc_->kind == DocKind::Json) m = Mode::Json;
    else if (doc_->kind == DocKind::Csv || doc_->kind == DocKind::Xlsx || doc_->kind == DocKind::Xls) m = Mode::Grid;
    else if (doc_->kind == DocKind::Pdf) m = Mode::Pdf;
    else if (doc_->kind == DocKind::Image) m = Mode::Image;
    else m = Mode::Document;

    if (m == Mode::Document) {
        if (!layout_) RelayoutIfNeeded();
        if (layout_) {
            for (auto& it : layout_->items) {
                const Block& b = *it.block;
                float top = it.y - scrollY_;
                if (b.kind == BlockKind::Code && !b.text.empty() &&
                    x >= it.x && x <= it.x + it.w && y >= top && y <= top + it.h) {
                    copyHover_.active = true;
                    copyHover_.kind = 1;
                    copyHover_.block = &b;
                    copyHover_.rect = { it.x + it.w - 38, top + 8, 28, 22 };
                    break;
                }
                if (b.kind == BlockKind::Table) {
                    bool found = false;
                    for (size_t c = 0; c < it.cells.size(); c++) {
                        auto& cell = it.cells[c];
                        if (cell.rt.text.empty()) continue;
                        float cy = cell.y - scrollY_;
                        if (x >= cell.x && x <= cell.x + cell.w && y >= cy && y <= cy + cell.h) {
                            copyHover_.active = true;
                            copyHover_.kind = 2;
                            copyHover_.block = &b;
                            copyHover_.cellIndex = (int)c;
                            copyHover_.rect = { cell.x + cell.w - 32, cy + 4, 26, 20 };
                            found = true;
                            break;
                        }
                    }
                    if (found) break;
                }
            }
        }
    } else if (m == Mode::Grid && doc_ && !doc_->grid.sheets.empty()) {
        int s = std::min(grid_.sheet, (int)doc_->grid.sheets.size() - 1);
        auto& sheet = doc_->grid.sheets[(size_t)s];
        float z = (float)zoom_;
        float numW = 52 * z, headerH = 30 * z;
        float tabH = doc_->grid.sheets.size() > 1 ? 30 * z : 0;
        float bodyH = heightDip() - tabH;
        if (x > numW && y < headerH) {
            float cx = numW - scrollX_;
            for (int c = 0; c < MaxCols(sheet); c++) {
                float cw = c < (int)grid_.colWidths[s].size() ? grid_.colWidths[s][c] * z : 84 * z;
                if (x >= cx && x < cx + cw) {
                    copyHover_.active = true;
                    copyHover_.kind = 3;
                    copyHover_.row = 0;
                    copyHover_.col = c;
                    copyHover_.rect = { cx + cw - 44, (headerH - 20) / 2, 24, 20 };
                    break;
                }
                cx += cw;
            }
        } else if (x > numW && y > headerH && y < bodyH) {
            int col = -1;
            float cx = numW - scrollX_;
            for (int c = 0; c < MaxCols(sheet); c++) {
                float cw = c < (int)grid_.colWidths[s].size() ? grid_.colWidths[s][c] * z : 84 * z;
                if (x >= cx && x < cx + cw) { col = c; break; }
                cx += cw;
            }
            int row = -1;
            float ry = headerH - scrollY_;
            for (size_t r = 1; r < sheet.rows.size(); r++) {
                float rh = r < grid_.rowHeights[s].size() ? grid_.rowHeights[s][r] * z : 27 * z;
                if (y >= ry && y < ry + rh) { row = (int)r; break; }
                ry += rh;
            }
            if (col >= 0 && row >= 1) {
                float cw = col < (int)grid_.colWidths[s].size() ? grid_.colWidths[s][col] * z : 84 * z;
                float rh = row < (int)grid_.rowHeights[s].size() ? grid_.rowHeights[s][row] * z : 27 * z;
                copyHover_.active = true;
                copyHover_.kind = 3;
                copyHover_.row = row;
                copyHover_.col = col;
                copyHover_.rect = { cx + cw - 30, ry + (rh - 20) / 2, 24, 20 };
                if (copyHover_.rect.y < headerH) copyHover_.rect.y = headerH + 2;
            }
        }
    }

    bool changed = old.active != copyHover_.active || old.block != copyHover_.block ||
                   old.cellIndex != copyHover_.cellIndex || old.row != copyHover_.row ||
                   old.col != copyHover_.col || old.rect.x != copyHover_.rect.x ||
                   old.rect.y != copyHover_.rect.y;
    if (changed) {
        // The "copied" tick belongs to the cell that was clicked. Once the
        // pointer moves to another cell, show the normal copy icon again.
        ClearCopyFeedback();
        InvalidateAll();
    }
}

void DocView::ClearCopyFeedback() {
    copyPressed_ = false;
    copyDone_ = false;
    if (copyTimer_) {
        if (hwnd_) KillTimer(hwnd_, 2);
        copyTimer_ = 0;
    }
}

void DocView::DrawCopyButton(Canvas& c) {
    if (!copyHover_.active) return;
    c.SetTranslateUi(true);
    auto& p = c.palette();
    DRect r = copyHover_.rect;
    if (r.y < 0 || r.y > c.heightDip() - r.h || r.x < 0 || r.x > c.widthDip() - r.w) return;
    auto withAlpha = [](Color color, float alpha) {
        color.a = alpha;
        return color;
    };
    // Keep the chip translucent so cell/code text stays readable underneath.
    // Pressed is the strongest state and is capped at 60% opacity.
    Color bg = copyPressed_ ? withAlpha(p.accent, 0.60f)
                            : (copyDone_ ? withAlpha(p.accentSoft, 0.55f)
                                         : withAlpha(p.surface, 0.55f));
    Color border = copyDone_ ? withAlpha(p.accent, 0.60f) : withAlpha(p.border, 0.60f);
    c.FillRound(r.x, r.y, r.w, r.h, 5, bg);
    c.StrokeRound(r.x, r.y, r.w, r.h, 5, border);
    Color fg = copyPressed_ ? HexColor(L"#FFFFFF") : (copyDone_ ? p.accent : p.muted);
    if (copyDone_)
        c.DrawIcon(L"\uE73E", r.x, r.y, r.w, r.h, 13, fg);
    else
        c.DrawIcon(L"\uE8C8", r.x, r.y, r.w, r.h, 13, fg);
}

// ---------------------------------------------------------------- input
int DocView::HitBlock(float x, float y) const {
    if (!layout_) return -1;
    for (size_t i = 0; i < layout_->items.size(); i++) {
        auto& it = layout_->items[i];
        if (y >= it.y && y <= it.y + it.h && x >= it.x - 20 && x <= it.x + it.w + 20)
            return (int)i;
    }
    return -1;
}

void DocView::ToggleBlock(int index) {
    if (!doc_ || index < 0 || index >= (int)doc_->blocks.size()) return;
    Block& b = doc_->blocks[(size_t)index];
    if (b.kind != BlockKind::Reasoning && b.kind != BlockKind::Tool) return;
    if (!b.collapsible) return;
    b.expanded = !b.expanded;
    layoutDirty_ = true;
    RelayoutIfNeeded();
    InvalidateAll();
}

void DocView::OpenLink(const std::wstring& url) {
    if (url.empty() || !doc_) return;
    if (url[0] == L'#') {
        std::wstring anchor = url.substr(1);
        for (size_t i = 0; i < doc_->blocks.size(); i++) {
            if (_wcsicmp(doc_->blocks[i].anchor.c_str(), anchor.c_str()) == 0) {
                ScrollToBlock((int)i);
                return;
            }
        }
        return;
    }
    if (IsWebUrl(url)) { OpenExternal(url); return; }
    std::wstring target = url;
    if (target.rfind(L"file:///", 0) == 0) target = target.substr(8);
    if (!(target.size() > 1 && target[1] == L':') && !target.empty() && target[0] != L'\\' && target[0] != L'/')
        target = PathJoin(PathDir(doc_->path), target);
    target = ReplaceAll(target, L"/", L"\\");
    if (FileExists(target)) {
        DocKind k = DetectKind(target, L"");
        if (k != DocKind::None) {
            if (events_.openPath) events_.openPath(target);
            else OpenExternal(target);
            return;
        }
    }
    OpenExternal(target);
}

void DocView::BeginMiddlePan(int px, int py) {
    if (inkEdit_) InkCommitText();
    copyHover_ = CopyTarget{};
    ClearCopyFeedback();
    float scale = GetDpiForWindow(hwnd_) / 96.0f;
    panAnchor_.x = panCurrent_.x = (LONG)std::lround(px / scale);
    panAnchor_.y = panCurrent_.y = (LONG)std::lround(py / scale);
    panVelocityX_ = panVelocityY_ = 0;
    panLastTick_ = GetTickCount64();
    middlePan_ = true;
    SetFocus(hwnd_);
    SetCapture(hwnd_);
    if (panTimer_) KillTimer(hwnd_, 1);
    panTimer_ = 1;
    if (!panTimerResolution_) {
        timeBeginPeriod(1);
        panTimerResolution_ = true;
    }
    SetTimer(hwnd_, 1, 10, nullptr);
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    InvalidateAll();
}

void DocView::UpdateMiddlePan(int px, int py) {
    if (!middlePan_) return;
    float scale = GetDpiForWindow(hwnd_) / 96.0f;
    panCurrent_.x = (LONG)std::lround(px / scale);
    panCurrent_.y = (LONG)std::lround(py / scale);
    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
    // High-rate mice can starve the 10 ms timer with mouse-move messages.
    // Advance from the move path as well, but keep an 8 ms floor so a single
    // frame is not invalidated multiple times for one display refresh.
    if (GetTickCount64() - panLastTick_ >= 8) AdvanceMiddlePan();
}

void DocView::AdvanceMiddlePan() {
    if (!middlePan_) return;
    ULONGLONG now = GetTickCount64();
    float dt = (now - panLastTick_) / 1000.0f;
    if (dt < 0.008f) return;
    panLastTick_ = now;
    dt = Clampf(dt, 0.008f, 0.05f);
    float dx = (float)(panCurrent_.x - panAnchor_.x);
    float dy = (float)(panCurrent_.y - panAnchor_.y);
    const float deadZone = 15.0f;
    float fullRadius = Clampf(heightDip() * 0.35f, 140.0f, 280.0f);
    const float maxSpeed = 5000.0f;
    auto axisSpeed = [&](float d) -> float {
        float ad = std::abs(d);
        if (ad <= deadZone) return 0.0f;
        float t = std::min(1.0f, (ad - deadZone) / (fullRadius - deadZone));
        float v = maxSpeed * std::pow(t, 1.6f);
        return d < 0 ? -v : v;
    };
    float targetX = axisSpeed(dx);
    float targetY = axisSpeed(dy);
    float blend = 1.0f - std::exp(-dt * 28.0f);
    panVelocityX_ += (targetX - panVelocityX_) * blend;
    panVelocityY_ += (targetY - panVelocityY_) * blend;
    if (std::abs(panVelocityX_) < 0.4f) panVelocityX_ = 0;
    if (std::abs(panVelocityY_) < 0.4f) panVelocityY_ = 0;
    scrollX_ = std::max(0.0f, scrollX_ + panVelocityX_ * dt);
    scrollY_ = std::max(0.0f, scrollY_ + panVelocityY_ * dt);
    UpdateScrollPosition();
}

void DocView::EndMiddlePan() {
    if (!middlePan_) return;
    middlePan_ = false;
    panVelocityX_ = panVelocityY_ = 0;
    if (panTimer_) { KillTimer(hwnd_, 1); panTimer_ = 0; }
    if (panTimerResolution_) { timeEndPeriod(1); panTimerResolution_ = false; }
    if (GetCapture() == hwnd_) ReleaseCapture();
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    lastScrollStatusTick_ = GetTickCount64();
    if (events_.statusChanged) events_.statusChanged();
    InvalidateAll();
}

void DocView::OnMouseDown(int px, int py, bool ctrl) {
    SetFocus(hwnd_);
    if (inkMode_) CancelInkAutoShape();
    float scale = GetDpiForWindow(hwnd_) / 96.0f;
    float x = px / scale, y = py / scale;
    if (middlePan_) {
        EndMiddlePan();
        return;
    }
    if (inkMode_) {
        if (inkEdit_) InkCommitText();
        bool gridTabStrip = false;
        if (doc_ && (doc_->kind == DocKind::Csv || doc_->kind == DocKind::Xlsx ||
                     doc_->kind == DocKind::Xls) && doc_->grid.sheets.size() > 1) {
            float tabH = 30.0f * (float)zoom_;
            gridTabStrip = y >= heightDip() - tabH;
        }
        if (!gridTabStrip) {
            int action = 0;
            bool overUi = inkBarRect_.Contains(x, y) || inkMorePanel_.Contains(x, y) ||
                          inkSelectionBarRect_.Contains(x, y);
            if (overUi) {
                if (InkHitButton(x, y, action) && action) InkAction(action);
                return;
            }
            if (inkMoreOpen_) {
                inkMoreOpen_ = false;
                InvalidateAll();
                return;
            }
            if (inkTool_ == INK_TOOL_SELECT) {
                if (!inkSelection_.empty()) {
                    float x0, y0, x1, y1;
                    InkSelectionBounds(x0, y0, x1, y1);
                    float sx0, sy0, sx1, sy1;
                    InkContentToScreen(x0, y0, sx0, sy0);
                    InkContentToScreen(x1, y1, sx1, sy1);
                    float cx = (sx0 + sx1) / 2;
                    const float movePad = 10.0f;
                    auto nearHandle = [&](float px, float py) {
                        return std::abs(x - px) <= 8 && std::abs(y - py) <= 8;
                    };
                    if (nearHandle(cx, sy0 - 22)) {
                        InkSelectBeginDrag(x, y, 3);
                        return;
                    }
                    if (nearHandle(sx0, sy0) || nearHandle(sx1, sy0) ||
                        nearHandle(sx0, sy1) || nearHandle(sx1, sy1)) {
                        InkSelectBeginDrag(x, y, 2);
                        return;
                    }
                    if (x >= sx0 - movePad && x <= sx1 + movePad &&
                        y >= sy0 - movePad && y <= sy1 + movePad) {
                        InkSelectBeginDrag(x, y, 1);
                        return;
                    }
                }
                bool add = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                inkLassoing_ = true;
                inkLassoMoved_ = false;
                inkLassoAdd_ = add;
                inkLassoStart_ = { x, y };
                inkLasso_.clear();
                inkLasso_.push_back({ x, y });
                SetCapture(hwnd_);
                InvalidateAll();
                return;
            }
            if (inkTool_ == INK_TOOL_HIGHLIGHT && CanHighlightPdf()) {
                pdfHighlighting_ = true;
                pdfHighlightPage_ = -1;
                pdfHighlightDragRect_ = { x, y, 0, 0 };
                pdfHighlightPath_.clear();
                pdfHighlightPath_.push_back({ x, y });
                for (int i = 0; i < pdf_->PageCount(); i++) {
                    float px = 0, py = 0, pw = 0, ph = 0;
                    if (!PdfPageRect(i, px, py, pw, ph)) continue;
                    if (x >= px && x <= px + pw && y >= py && y <= py + ph) {
                        pdfHighlightPage_ = i;
                        pdfHighlightPageRect_ = { px, py, pw, ph };
                        break;
                    }
                }
                if (pdfHighlightPage_ >= 0) SetCapture(hwnd_);
                else pdfHighlighting_ = false;
                InvalidateAll();
                return;
            }
            InkBegin(x, y);
            return;
        }
    }
    if (copyHover_.active && CopyHitRect().Contains(x, y)) {
        std::wstring text;
        if (copyHover_.kind == 1 && copyHover_.block) text = copyHover_.block->text;
        else if (copyHover_.kind == 2 && copyHover_.block && layout_) {
            for (auto& it : layout_->items) {
                if (it.block == copyHover_.block && copyHover_.cellIndex >= 0 &&
                    copyHover_.cellIndex < (int)it.cells.size()) {
                    text = it.cells[(size_t)copyHover_.cellIndex].rt.text;
                    break;
                }
            }
        } else if (copyHover_.kind == 3 && doc_ && !doc_->grid.sheets.empty()) {
            int s = std::min(grid_.sheet, (int)doc_->grid.sheets.size() - 1);
            text = CellString(doc_->grid.sheets[(size_t)s], copyHover_.row, copyHover_.col);
        }
        if (!text.empty()) {
            SetClipboardText(hwnd_, text);
            copyPressed_ = true;
            copyDone_ = true;
            if (copyTimer_) KillTimer(hwnd_, 2);
            copyTimer_ = 2;
            SetTimer(hwnd_, 2, 1200, nullptr);
            InvalidateAll();
        }
        return;
    }
    mouseDown_ = true;
    Mode m = Mode::Welcome;
    if (!errorMessage_.empty()) m = Mode::Error;
    else if (!doc_) m = Mode::Welcome;
    else if (sourceMode_) m = Mode::Source;
    else if (doc_->kind == DocKind::Json) m = Mode::Json;
    else if (doc_->kind == DocKind::Csv || doc_->kind == DocKind::Xlsx || doc_->kind == DocKind::Xls) m = Mode::Grid;
    else if (doc_->kind == DocKind::Pdf) m = Mode::Pdf;
    else if (doc_->kind == DocKind::Image) m = Mode::Image;
    else m = Mode::Document;
    if (m == Mode::Welcome) {
        float wy = y + scrollY_;
        if (welcomeMoreRect_.Contains(x, wy)) {
            welcomeRecentShown_ = std::min((int)recent_.size(),
                                           std::max(welcomeRecentShown_, kWelcomeRecentPage) + kWelcomeRecentPage);
            UpdateScrollBars();
            InvalidateAll();
            return;
        }
        if (welcomeOpenRect_.Contains(x, wy)) {
            if (events_.openMenu) events_.openMenu();
            else if (events_.openFile) events_.openFile();
            return;
        }
        if (welcomeClearRect_.Contains(x, wy)) {
            if (events_.clearRecent) events_.clearRecent();
            return;
        }
        if (welcomeWorkspaceClearRect_.Contains(x, wy)) {
            if (events_.clearWorkspaces) events_.clearWorkspaces();
            return;
        }
        for (size_t i = 0; i < welcomeWorkspaceRects_.size(); i++) {
            if (welcomeWorkspaceRects_[i].Contains(x, wy) && i < welcomeWorkspacePaths_.size()) {
                if (events_.openWorkspace) events_.openWorkspace(welcomeWorkspacePaths_[i]);
                return;
            }
        }
        for (size_t i = 0; i < welcomeRecentRects_.size(); i++) {
            if (i < welcomeRecentPaths_.size() &&
                i < welcomeRecentDeleteRects_.size() &&
                welcomeRecentDeleteRects_[i].Contains(x, wy)) {
                if (events_.removeRecent) events_.removeRecent(welcomeRecentPaths_[i]);
                return;
            }
            if (welcomeRecentRects_[i].Contains(x, wy) && i < welcomeRecentPaths_.size()) {
                if (events_.openPath) events_.openPath(welcomeRecentPaths_[i]);
                return;
            }
        }
        return;
    }
    if (m == Mode::Error) {
        if (errorOpenRect_.Contains(x, y)) { if (events_.openFile) events_.openFile(); return; }
        if (errorExternalRect_.Contains(x, y)) { OpenExternal(errorPath_); return; }
        return;
    }
    if (m == Mode::Grid && doc_ && !doc_->grid.sheets.empty()) {
        int s = std::min(grid_.sheet, (int)doc_->grid.sheets.size() - 1);
        auto& sheet = doc_->grid.sheets[(size_t)s];
        float z = (float)zoom_;
        float numW = 52 * z, headerH = 30 * z;
        float tabH = doc_->grid.sheets.size() > 1 ? 30 * z : 0;
        float bodyH = heightDip() - tabH;
        if (tabH > 0 && y >= bodyH) {
            float tx = 8;
            for (size_t i = 0; i < doc_->grid.sheets.size(); i++) {
                std::wstring name = doc_->grid.sheets[i].name.empty() ? L"Sheet" : doc_->grid.sheets[i].name;
                float tw = name.size() * 7.0f + 28;
                if (x >= tx && x <= tx + tw) {
                    grid_.sheet = (int)i;
                    scrollX_ = scrollY_ = 0;
                    selectedRow_ = selectedCol_ = -1;
                    hoverGridColSep_ = hoverGridRowSep_ = -1;
                    UpdateScrollBars();
                    InvalidateAll();
                    return;
                }
                tx += tw + 4;
            }
            return;
        }
        if (y < headerH && x >= numW) {
            float cx = numW - scrollX_;
            for (int col = 0; col < MaxCols(sheet); col++) {
                float cw = col < (int)grid_.colWidths[s].size() ? grid_.colWidths[s][col] * z : 84 * z;
                if (x >= cx + cw - 6 && x <= cx + cw + 6) {
                    dragging_ = 1; dragIndex_ = col; dragStart_ = x; dragValue_ = cw / z;
                    hoverGridColSep_ = col;
                    hoverGridRowSep_ = -1;
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    SetCapture(hwnd_);
                    return;
                }
                cx += cw;
            }
            return;
        }
        if (x <= numW && y >= headerH) {
            float ry = headerH - scrollY_;
            for (size_t r = 1; r < sheet.rows.size(); r++) {
                float rh = r < grid_.rowHeights[s].size() ? grid_.rowHeights[s][r] * z : 27 * z;
                if (y >= ry + rh - 4 && y <= ry + rh + 4) {
                    dragging_ = 2; dragIndex_ = (int)r; dragStart_ = y; dragValue_ = rh / z;
                    hoverGridRowSep_ = (int)r;
                    hoverGridColSep_ = -1;
                    SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                    SetCapture(hwnd_);
                    return;
                }
                ry += rh;
            }
        }
        if (x > numW && y > headerH && y < bodyH) {
            float cx = numW - scrollX_;
            int col = -1;
            for (int c = 0; c < MaxCols(sheet); c++) {
                float cw = c < (int)grid_.colWidths[s].size() ? grid_.colWidths[s][c] * z : 84 * z;
                if (x >= cx && x < cx + cw) { col = c; break; }
                cx += cw;
            }
            float ry = headerH - scrollY_;
            int row = -1;
            for (size_t r = 1; r < sheet.rows.size(); r++) {
                float rh = r < grid_.rowHeights[s].size() ? grid_.rowHeights[s][r] * z : 27 * z;
                if (y >= ry && y < ry + rh) { row = (int)r; break; }
                ry += rh;
            }
            if (col >= 0 && row >= 0) {
                selectedRow_ = row;
                selectedCol_ = col;
                InvalidateAll();
            }
        }
        return;
    }
    if (m == Mode::Json && json_.built) {
        float z = (float)zoom_;
        int row = (int)((y - 16 + scrollY_) / (24 * z));
        if (row >= 0 && row < (int)json_.rows.size()) {
            auto& jr = json_.rows[(size_t)row];
            if (jr.container && !jr.closing && jr.node) {
                if (json_.collapsed.count(jr.node)) json_.collapsed.erase(jr.node);
                else json_.collapsed.insert(jr.node);
                json_.built = false;
                json_.rows.clear();
                BuildJson();
                UpdateScrollBars();
                InvalidateAll();
            }
        }
        return;
    }
    if (m == Mode::Document && doc_ && layout_) {
        int idx = HitBlock(x, y + scrollY_);
        if (idx >= 0) {
            auto& it = layout_->items[(size_t)idx];
            if (it.main.layout) {
                DWRITE_HIT_TEST_METRICS hm{};
                BOOL trailing = FALSE, inside = FALSE;
                if (SUCCEEDED(it.main.layout->HitTestPoint(x - it.contentX, y + scrollY_ - it.contentY,
                                                           &trailing, &inside, &hm)) && inside) {
                    std::wstring url = it.main.LinkAt(hm.textPosition);
                    if (!url.empty()) { OpenLink(url); return; }
                }
            }
            if (it.block->kind == BlockKind::Reasoning || it.block->kind == BlockKind::Tool) {
                ToggleBlock(idx);
                return;
            }
        }
    }
    (void)ctrl;
}

void DocView::OnMouseUp(int px, int py) {
    if (inkLassoing_) {
        bool moved = inkLassoMoved_;
        bool add = inkLassoAdd_;
        InkPoint start = inkLassoStart_;
        inkLassoing_ = false;
        if (moved && inkLasso_.size() >= 3) {
            // The closing segment is only implied after release. If the user
            // already returned to the starting point, use that loop directly;
            // otherwise connect the last point to the first before hit-testing.
            bool closed = false;
            if (!inkLasso_.empty()) {
                float dx = inkLasso_.back().x - inkLasso_.front().x;
                float dy = inkLasso_.back().y - inkLasso_.front().y;
                closed = dx * dx + dy * dy <= 100.0f;
            }
            if (!closed) inkLasso_.push_back(inkLasso_.front());
            InkSelectLasso(add);
        } else {
            InkSelectAt(start.x, start.y, add);
        }
        inkLasso_.clear();
        if (GetCapture() == hwnd_) ReleaseCapture();
        InvalidateAll();
        return;
    }
    if (inkDragMode_) {
        InkSelectEndDrag();
        return;
    }
    if (pdfHighlighting_) {
        float scale = GetDpiForWindow(hwnd_) / 96.0f;
        float x = px / scale, y = py / scale;
        float x0 = pdfHighlightDragRect_.x, y0 = pdfHighlightDragRect_.y;
        FinishPdfHighlight(x0, y0, x, y);
        return;
    }
    if (inkDrawing_ || inkFreehandActive_ || inkErasing_ || inkShapeDrawing_ ||
        textHighlighting_) {
        InkEnd();
        return;
    }
    if (dragging_) {
        ReleaseCapture();
        dragging_ = 0;
        dragIndex_ = -1;
    }
    if (copyPressed_) {
        copyPressed_ = false;
        InvalidateAll();
    }
    mouseDown_ = false;
    (void)px; (void)py;
}

void DocView::OnMouseMove(int px, int py) {
    float scale = GetDpiForWindow(hwnd_) / 96.0f;
    float x = px / scale, y = py / scale;
    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
    TrackMouseEvent(&tme);
    if (middlePan_) {
        UpdateMiddlePan(px, py);
        SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
        return;
    }
    if (inkMode_) {
        if (inkLassoing_) {
            if (!inkLasso_.empty()) {
                float dx = x - inkLasso_.back().x, dy = y - inkLasso_.back().y;
                if (dx * dx + dy * dy >= 2.25f) {
                    inkLasso_.push_back({ x, y });
                }
                float sx = x - inkLassoStart_.x, sy = y - inkLassoStart_.y;
                if (sx * sx + sy * sy >= 25.0f) inkLassoMoved_ = true;
                if (inkLassoMoved_) InvalidateAll();
            }
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return;
        }
        if (inkDragMode_) {
            InkSelectMoveDrag(x, y);
            if (inkDragMode_ == 3) SetCursor(LoadCursorW(nullptr, IDC_HAND));
            else if (inkDragMode_ == 2) SetCursor(LoadCursorW(nullptr, IDC_SIZENWSE));
            else SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return;
        }
        if (pdfHighlighting_) {
            pdfHighlightDragRect_.w = x - pdfHighlightDragRect_.x;
            pdfHighlightDragRect_.h = y - pdfHighlightDragRect_.y;
            if (!pdfHighlightPath_.empty()) {
                float dx = x - pdfHighlightPath_.back().x;
                float dy = y - pdfHighlightPath_.back().y;
                if (dx * dx + dy * dy >= 2.25f) pdfHighlightPath_.push_back({ x, y });
            }
            InvalidateAll();
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return;
        }
        if (inkDrawing_ || inkFreehandActive_ || inkErasing_ || inkShapeDrawing_ ||
            textHighlighting_) {
            InkMove(x, y);
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
            return;
        }
        int action = 0;
        bool overBar = inkBarRect_.Contains(x, y) || inkMorePanel_.Contains(x, y) ||
                       inkSelectionBarRect_.Contains(x, y);
        if (overBar) InkHitButton(x, y, action);
        if (action != inkHover_) {
            inkHover_ = action;
            InvalidateAll();
        }
        if (action) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
        } else if (overBar) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        } else if (inkTool_ == INK_TOOL_SELECT) {
            HCURSOR cur = LoadCursorW(nullptr, IDC_ARROW);
            if (!inkSelection_.empty()) {
                float x0, y0, x1, y1;
                InkSelectionBounds(x0, y0, x1, y1);
                float sx0, sy0, sx1, sy1;
                InkContentToScreen(x0, y0, sx0, sy0);
                InkContentToScreen(x1, y1, sx1, sy1);
                float cx = (sx0 + sx1) / 2;
                if (std::abs(x - cx) <= 8 && std::abs(y - (sy0 - 22)) <= 8) {
                    cur = LoadCursorW(nullptr, IDC_HAND);
                } else if ((std::abs(x - sx0) <= 8 && std::abs(y - sy0) <= 8) ||
                           (std::abs(x - sx1) <= 8 && std::abs(y - sy1) <= 8)) {
                    cur = LoadCursorW(nullptr, IDC_SIZENWSE);
                } else if ((std::abs(x - sx1) <= 8 && std::abs(y - sy0) <= 8) ||
                           (std::abs(x - sx0) <= 8 && std::abs(y - sy1) <= 8)) {
                    cur = LoadCursorW(nullptr, IDC_SIZENESW);
                } else if (x >= sx0 - 10 && x <= sx1 + 10 && y >= sy0 - 10 && y <= sy1 + 10) {
                    cur = LoadCursorW(nullptr, IDC_SIZEALL);
                }
            }
            SetCursor(cur);
        } else if (inkTool_ == INK_TOOL_TEXT) {
            SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
        } else {
            SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        }
        return;
    }
    UpdateCopyHover(x, y);
    if (copyHover_.active && CopyHitRect().Contains(x, y)) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return;
    }
    Mode m = Mode::Welcome;
    if (!errorMessage_.empty()) m = Mode::Error;
    else if (!doc_) m = Mode::Welcome;
    else if (sourceMode_) m = Mode::Source;
    else if (doc_->kind == DocKind::Json) m = Mode::Json;
    else if (doc_->kind == DocKind::Csv || doc_->kind == DocKind::Xlsx || doc_->kind == DocKind::Xls) m = Mode::Grid;
    else if (doc_->kind == DocKind::Pdf) m = Mode::Pdf;
    else if (doc_->kind == DocKind::Image) m = Mode::Image;
    else m = Mode::Document;

    if (dragging_ == 1 && doc_ && !doc_->grid.sheets.empty()) {
        int s = std::min(grid_.sheet, (int)doc_->grid.sheets.size() - 1);
        if (dragIndex_ >= 0 && dragIndex_ < (int)grid_.colWidths[s].size()) {
            float z = (float)zoom_;
            grid_.colWidths[s][dragIndex_] = Clampf(dragValue_ + (x - dragStart_) / z, 40, 800);
            hoverGridColSep_ = dragIndex_;
            hoverGridRowSep_ = -1;
            UpdateScrollBars();
            InvalidateAll();
        }
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return;
    }
    if (dragging_ == 2 && doc_ && !doc_->grid.sheets.empty()) {
        int s = std::min(grid_.sheet, (int)doc_->grid.sheets.size() - 1);
        if (dragIndex_ >= 0 && dragIndex_ < (int)grid_.rowHeights[s].size()) {
            float z = (float)zoom_;
            grid_.rowHeights[s][dragIndex_] = Clampf(dragValue_ + (y - dragStart_) / z, 18, 200);
            hoverGridRowSep_ = dragIndex_;
            hoverGridColSep_ = -1;
            UpdateScrollBars();
            InvalidateAll();
        }
        SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
        return;
    }

    if (m == Mode::Welcome) {
        float wy = y + scrollY_;
        int hover = -1;
        for (size_t i = 0; i < welcomeRecentRects_.size(); i++) {
            if (welcomeRecentRects_[i].Contains(x, wy)) {
                if (i < welcomeRecentRowIndices_.size()) hover = welcomeRecentRowIndices_[i];
                break;
            }
        }
        int workspaceHover = -1;
        for (size_t i = 0; i < welcomeWorkspaceRects_.size(); i++) {
            if (welcomeWorkspaceRects_[i].Contains(x, wy)) { workspaceHover = (int)i; break; }
        }
        if (hover != hoverWelcome_ || workspaceHover != hoverWorkspace_) {
            hoverWelcome_ = hover;
            hoverWorkspace_ = workspaceHover;
            InvalidateAll();
        }
        bool action = welcomeOpenRect_.Contains(x, wy) ||
                      welcomeClearRect_.Contains(x, wy) ||
                      welcomeWorkspaceClearRect_.Contains(x, wy) ||
                      welcomeMoreRect_.Contains(x, wy) ||
                      hover >= 0 || workspaceHover >= 0;
        SetCursor(LoadCursorW(nullptr, action ? IDC_HAND : IDC_ARROW));
    } else if (m == Mode::Document && layout_) {
        int idx = HitBlock(x, y + scrollY_);
        std::wstring url;
        if (idx >= 0) {
            auto& it = layout_->items[(size_t)idx];
            if (it.main.layout) {
                DWRITE_HIT_TEST_METRICS hm{};
                BOOL trailing = FALSE, inside = FALSE;
                if (SUCCEEDED(it.main.layout->HitTestPoint(x - it.contentX, y + scrollY_ - it.contentY,
                                                           &trailing, &inside, &hm)) && inside)
                    url = it.main.LinkAt(hm.textPosition);
            }
        }
        if (idx != hoverLinkBlock_ || url != hoverLink_) {
            hoverLinkBlock_ = idx;
            hoverLink_ = url;
            InvalidateAll();
        }
        SetCursor(LoadCursorW(nullptr, !url.empty() ? IDC_HAND : IDC_ARROW));
    } else if (m == Mode::Json && json_.built) {
        float z = (float)zoom_;
        int row = (int)((y - 16 + scrollY_) / (24 * z));
        if (row < 0 || row >= (int)json_.rows.size()) row = -1;
        if (row != hoverJsonRow_) { hoverJsonRow_ = row; InvalidateAll(); }
        SetCursor(LoadCursorW(nullptr, row >= 0 ? IDC_HAND : IDC_ARROW));
    } else if (m == Mode::Grid) {
        int colSep = -1, rowSep = -1;
        bool hand = false;
        if (doc_ && !doc_->grid.sheets.empty()) {
            int s = std::min(grid_.sheet, (int)doc_->grid.sheets.size() - 1);
            auto& sheet = doc_->grid.sheets[(size_t)s];
            float z = (float)zoom_;
            float numW = 52 * z, headerH = 30 * z;
            float tabH = doc_->grid.sheets.size() > 1 ? 30 * z : 0;
            float bodyH = heightDip() - tabH;
            if (tabH > 0 && y >= bodyH) {
                hand = true;
            } else if (y < headerH && x >= numW) {
                float cx = numW - scrollX_;
                for (int col = 0; col < MaxCols(sheet); col++) {
                    float cw = col < (int)grid_.colWidths[s].size() ? grid_.colWidths[s][col] * z : 84 * z;
                    if (x >= cx + cw - 6 && x <= cx + cw + 6) { colSep = col; break; }
                    cx += cw;
                }
            } else if (x <= numW && y >= headerH && y < bodyH) {
                float ry = headerH - scrollY_;
                for (size_t r = 1; r < sheet.rows.size(); r++) {
                    float rh = r < grid_.rowHeights[s].size() ? grid_.rowHeights[s][r] * z : 27 * z;
                    if (y >= ry + rh - 4 && y <= ry + rh + 4) { rowSep = (int)r; break; }
                    ry += rh;
                }
            }
        }
        if (colSep != hoverGridColSep_ || rowSep != hoverGridRowSep_) {
            hoverGridColSep_ = colSep;
            hoverGridRowSep_ = rowSep;
            InvalidateAll();
        }
        if (colSep >= 0) SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        else if (rowSep >= 0) SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
        else SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
    } else if (m == Mode::Grid) {
        if (hoverGridColSep_ >= 0 || hoverGridRowSep_ >= 0) {
            hoverGridColSep_ = hoverGridRowSep_ = -1;
            InvalidateAll();
        }
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    } else {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
}

void DocView::OnMouseWheel(int delta, bool ctrl, bool shift, float x, float y) {
    if (inkEdit_) InkCommitText();
    if (middlePan_) EndMiddlePan();
    copyHover_.active = false;
    hoverGridColSep_ = hoverGridRowSep_ = -1;
    ClearCopyFeedback();
    if (ctrl) {
        if (events_.requestZoom) events_.requestZoom(delta > 0 ? 1 : -1);
        return;
    }
    bool touchpad = (GetMessageExtraInfo() & 0xFFFFFF00) == 0xFF515700;
    if (!touchpad && inkMode_ && inkTool_ == INK_TOOL_SELECT && !inkSelection_.empty()) {
        float notches = (float)delta / (float)WHEEL_DELTA;
        float scale = Clampf(1.0f + notches * 0.02f * (float)wheelStep_, 0.5f, 2.0f);
        InkNormalizeSelection();
        float x0, y0, x1, y1;
        InkSelectionBounds(x0, y0, x1, y1);
        if (x1 > x0 && y1 > y0) {
            float scx = 0, scy = 0;
            InkContentToScreen((x0 + x1) / 2, (y0 + y1) / 2, scx, scy);
            InkPushUndo();
            auto& layer = Ink();
            inkDragBase_.strokes = layer.strokes;
            inkDragBase_.shapes = layer.shapes;
            inkDragBase_.texts = layer.texts;
            inkDragPivot_ = { scx, scy };
            InkApplySelectionTransform(2, 0, 0, scale, 0.0f);
            MarkNotesDirty();
            InvalidateAll();
        }
        return;
    }
    if (!doc_ && errorMessage_.empty()) {
        float wy = y + scrollY_;
        if (welcomeRecentCard_.Contains(x, wy)) {
            WelcomeScrollRecent(delta);
            return;
        }
        if (welcomeWorkspaceCard_.Contains(x, wy)) {
            WelcomeScrollWorkspace(delta);
            return;
        }
        float step = wheelStep_ * kWheelLine;
        if (delta > 0) step = -step;
        scrollY_ = std::max(0.0f, scrollY_ + step);
        UpdateScrollBars();
        InvalidateAll();
        return;
    }
    float step = wheelStep_ * kWheelLine * (float)zoom_;
    if (delta > 0) step = -step;
    if (shift) scrollX_ = std::max(0.0f, scrollX_ + step);
    else scrollY_ = std::max(0.0f, scrollY_ + step);
    UpdateScrollPosition();
}

void DocView::OnKeyDown(WPARAM key) {
    // Customizable application/document shortcuts are handled by MainWindow's
    // message loop. Only context-local keys remain here.
    if (middlePan_ && key == VK_ESCAPE) {
        EndMiddlePan();
        return;
    }
    if (inkMode_ && key == VK_ESCAPE) {
        if (!inkSelection_.empty()) {
            inkSelection_.clear();
            InvalidateAll();
        } else {
            SetInkMode(false);
        }
        return;
    }
    if (inkMode_ && (key == VK_DELETE || key == VK_BACK) && !inkSelection_.empty()) {
        DeleteInkSelection();
        return;
    }
    bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (inkMode_ && ctrl && (key == 'A' || key == 'D')) {
        if (key == 'A') {
            inkSelection_.clear();
            auto& layer = Ink();
            for (int i = 0; i < (int)layer.strokes.size(); i++) inkSelection_.push_back({ 0, i });
            for (int i = 0; i < (int)layer.shapes.size(); i++) inkSelection_.push_back({ 1, i });
            for (int i = 0; i < (int)layer.texts.size(); i++) inkSelection_.push_back({ 2, i });
            InkNormalizeSelection();
            InvalidateAll();
        } else {
            if (!inkSelection_.empty()) {
                CopyInkSelection();
                PasteInkSelection();
            }
        }
        return;
    }
    if (inkMode_ && inkTool_ == INK_TOOL_SELECT && !inkSelection_.empty()) {
        float step = (GetKeyState(VK_SHIFT) & 0x8000) ? 10.0f : 1.0f;
        float dx = 0, dy = 0;
        if (key == VK_LEFT) dx = -step;
        else if (key == VK_RIGHT) dx = step;
        else if (key == VK_UP) dy = -step;
        else if (key == VK_DOWN) dy = step;
        if (dx != 0 || dy != 0) {
            InkNormalizeSelection();
            auto& layer = Ink();
            InkPushUndo();
            inkDragBase_.strokes = layer.strokes;
            inkDragBase_.shapes = layer.shapes;
            inkDragBase_.texts = layer.texts;
            InkApplySelectionTransform(1, dx, dy, 1.0f, 0.0f);
            InvalidateAll();
        }
    }
}

void DocView::OnCopy() {
    if (!doc_) return;
    if (doc_->kind == DocKind::Csv || doc_->kind == DocKind::Xlsx || doc_->kind == DocKind::Xls) {
        if (!doc_->grid.sheets.empty() && selectedRow_ >= 0 && selectedCol_ >= 0) {
            int s = std::min(grid_.sheet, (int)doc_->grid.sheets.size() - 1);
            auto& sheet = doc_->grid.sheets[(size_t)s];
            SetClipboardText(hwnd_, CellString(sheet, 0, selectedCol_) + L"\n" + CellString(sheet, selectedRow_, selectedCol_));
            return;
        }
    }
    std::wstring text;
    if (sourceMode_) text = doc_->text;
    else if (doc_->kind == DocKind::Json) text = PrettyJson(doc_->json);
    else text = BlocksPlainText(doc_->blocks);
    SetClipboardText(hwnd_, text);
}

void DocView::ScrollToBlock(int index) {
    if (!doc_ || index < 0 || index >= (int)doc_->blocks.size()) return;
    RelayoutIfNeeded();
    if (!layout_ || index >= (int)layout_->items.size()) return;
    scrollY_ = std::max(0.0f, layout_->items[(size_t)index].y - 20 * (float)zoom_);
    UpdateScrollBars();
    InvalidateAll();
}

void DocView::ScrollToTop() {
    scrollY_ = 0;
    scrollX_ = 0;
    UpdateScrollBars();
    InvalidateAll();
}

void DocView::CopySelection() {
    if (inkMode_ && !inkSelection_.empty()) CopyInkSelection();
    else OnCopy();
}

void DocView::ScrollBy(float dx, float dy) {
    scrollX_ = std::max(0.0f, scrollX_ + dx);
    scrollY_ = std::max(0.0f, scrollY_ + dy);
    UpdateScrollBars();
    InvalidateAll();
}

void DocView::ScrollPage(int dir) {
    ScrollBy(0, dir * heightDip() * 0.9f);
}

void DocView::ScrollHome() {
    scrollY_ = 0;
    UpdateScrollBars();
    InvalidateAll();
}

void DocView::ScrollEnd() {
    scrollY_ = 1e9f;
    UpdateScrollBars();
    InvalidateAll();
}

void DocView::ExpandJson(bool expand) {
    if (!doc_ || doc_->kind != DocKind::Json) return;
    json_.collapsed.clear();
    if (!expand) {
        std::function<void(const JNode&)> add = [&](const JNode& n) {
            if (n.kind == JKind::Object || n.kind == JKind::Array) {
                json_.collapsed.insert(&n);
                for (auto& m : n.members) add(m.second);
                for (auto& i : n.items) add(i);
            }
        };
        add(doc_->json);
    }
    json_.built = false;
    json_.rows.clear();
    BuildJson();
    UpdateScrollBars();
    InvalidateAll();
}

void DocView::FocusView() { if (hwnd_) SetFocus(hwnd_); }

LRESULT CALLBACK DocView::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    DocView* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        self = (DocView*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = hwnd;
    } else {
        self = (DocView*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->Handle(msg, wp, lp);
}

LRESULT DocView::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        layoutDirty_ = true;
        json_.built = false;
        source_.wrapWidth = -1.0f;
        UpdateScrollBars();
        InvalidateAll();
        return 0;
    case WM_KILLFOCUS:
        if (middlePan_) EndMiddlePan();
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        Paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_VSCROLL: {
        if (inkEdit_) InkCommitText();
        copyHover_.active = false;
        hoverGridColSep_ = hoverGridRowSep_ = -1;
        ClearCopyFeedback();
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd_, SB_VERT, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= (int)(wheelStep_ * 30); break;
        case SB_LINEDOWN: pos += (int)(wheelStep_ * 30); break;
        case SB_PAGEUP: pos -= (int)si.nPage; break;
        case SB_PAGEDOWN: pos += (int)si.nPage; break;
        case SB_THUMBTRACK: pos = si.nTrackPos; break;
        case SB_TOP: pos = si.nMin; break;
        case SB_BOTTOM: pos = si.nMax; break;
        }
        scrollY_ = (float)std::max(si.nMin, pos);
        UpdateScrollPosition();
        return 0;
    }
    case WM_HSCROLL: {
        if (inkEdit_) InkCommitText();
        copyHover_.active = false;
        hoverGridColSep_ = hoverGridRowSep_ = -1;
        ClearCopyFeedback();
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd_, SB_HORZ, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINELEFT: pos -= 60; break;
        case SB_LINERIGHT: pos += 60; break;
        case SB_PAGELEFT: pos -= (int)si.nPage; break;
        case SB_PAGERIGHT: pos += (int)si.nPage; break;
        case SB_THUMBTRACK: pos = si.nTrackPos; break;
        }
        scrollX_ = (float)std::max(0, pos);
        UpdateScrollPosition();
        return 0;
    }
    case WM_MBUTTONDOWN:
        if (middlePan_) EndMiddlePan();
        else BeginMiddlePan(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MBUTTONUP:
        // Autoscroll remains active after release; left/middle click or Esc
        // exits, matching common browser/PDF reader behavior.
        return 0;
    case WM_RBUTTONDOWN:
        if (middlePan_) {
            EndMiddlePan();
            return 0;
        }
        break;
    case WM_TIMER:
        if (wp == 1 && middlePan_) {
            copyHover_.active = false;
            AdvanceMiddlePan();
        } else if (wp == 2) {
            copyDone_ = false;
            copyPressed_ = false;
            copyTimer_ = 0;
            InvalidateAll();
        } else if (wp == 3 && inkAutoShapeTimer_) {
            KillTimer(hwnd_, 3);
            inkAutoShapeTimer_ = 0;
            RecognizeInkShape();
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (middlePan_ && (HWND)lp != hwnd_) EndMiddlePan();
        if ((inkDrawing_ || inkFreehandActive_ || inkErasing_ || inkShapeDrawing_ ||
             textHighlighting_) && (HWND)lp != hwnd_) InkEnd();
        if (inkLassoing_ && (HWND)lp != hwnd_) {
            inkLassoing_ = false;
            inkLasso_.clear();
            InvalidateAll();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        bool ctrl = (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) != 0;
        bool shift = (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0;
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd_, &pt);
        float scale = GetDpiForWindow(hwnd_) / 96.0f;
        OnMouseWheel(delta, ctrl, shift, pt.x / scale, pt.y / scale);
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        // Precision touchpads and horizontal mouse wheels send this instead
        // of WM_MOUSEWHEEL + Shift. Without it, wide PDF pages and images
        // appear to ignore horizontal swipes.
        if (inkEdit_) InkCommitText();
        if (middlePan_) EndMiddlePan();
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        float step = ((float)delta / (float)WHEEL_DELTA) *
                     (float)wheelStep_ * kWheelLine * (float)zoom_;
        scrollX_ = std::max(0.0f, scrollX_ + step);
        UpdateScrollPosition();
        return 0;
    }
    case WM_LBUTTONDOWN:
        OnMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), (wp & MK_CONTROL) != 0);
        return 0;
    case WM_LBUTTONUP:
        OnMouseUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEMOVE:
        OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSELEAVE:
        hoverJsonRow_ = -1;
        hoverWelcome_ = -1;
        hoverWorkspace_ = -1;
        hoverGridColSep_ = hoverGridRowSep_ = -1;
        hoverLink_.clear();
        hoverLinkBlock_ = -1;
        copyHover_.active = false;
        ClearCopyFeedback();
        InvalidateAll();
        return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        auto& p = Palette::Get(theme_);
        SetTextColor(dc, ToColorRef(p.fg));
        SetBkColor(dc, ToColorRef(p.surface));
        if (!inkEditBrush_) inkEditBrush_ = CreateSolidBrush(ToColorRef(p.surface));
        return (LRESULT)inkEditBrush_;
    }
    case WM_INK_COMMIT:
        InkCommitText();
        return 0;
    case WM_INK_CANCEL:
        InkCancelText();
        return 0;
    case WM_RBUTTONUP: {
        if (inkMode_) {
            float sc = GetDpiForWindow(hwnd_) / 96.0f;
            float x = GET_X_LPARAM(lp) / sc, y = GET_Y_LPARAM(lp) / sc;
            if (inkBarRect_.Contains(x, y)) {
                for (auto& b : inkButtons_) {
                    int offset = b.action - INK_COLOR;
                    if (offset < 0 || offset >= (int)inkColors_.size()) continue;
                    if (!b.rect.Contains(x, y)) continue;
                    int preset = offset;
                    if (settings_ && offset < (int)settings_->inkToolbarPresets.size())
                        preset = settings_->inkToolbarPresets[(size_t)offset];
                    if (events_.openColorPicker) events_.openColorPicker(preset);
                    return 0;
                }
                if (events_.requestFullscreenInk) {
                    events_.requestFullscreenInk();
                    return 0;
                }
            }
        }
        break;
    }
    case WM_KEYDOWN:
        OnKeyDown(wp);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) return TRUE;
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
