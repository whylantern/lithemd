#pragma once
#include "common.h"
#include "document.h"
#include "pdf.h"
#include <functional>
#include <map>
#include <memory>
#include <vector>
#include <set>

class Canvas;
class PdfDocument;
struct JsonRow;

struct DRect {
    float x = 0, y = 0, w = 0, h = 0;
    bool Contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

struct JsonRow {
    const JNode* node = nullptr;
    std::wstring key;
    std::wstring value;
    int depth = 0;
    int valueType = 0;       // 0 null, 1 bool, 2 number, 3 string, 4 object, 5 array
    bool container = false;
    bool expanded = false;
    bool closing = false;
    bool hasComma = false;
};

struct DocViewEvents {
    std::function<void()> openFile;
    std::function<void()> openFolder;
    std::function<void(const std::wstring&)> openPath;
    std::function<void(const std::wstring&)> removeRecent;
    std::function<void()> clearRecent;
    std::function<void()> reload;
    std::function<void()> statusChanged;
    std::function<void()> outlineChanged;
    std::function<void()> sourceModeChanged;
    std::function<void(int)> requestZoom;   // +1 / -1 steps, 0 reset
    std::function<void(int)> openColorPicker; // -1 current color, otherwise preset index
};

struct InkPoint {
    float x = 0, y = 0;
};

class DocView {
public:
    ~DocView();
    static void Register(HINSTANCE inst);
    bool Create(HWND parent, int id);
    HWND hwnd() const { return hwnd_; }

    void SetEvents(DocViewEvents events) { events_ = std::move(events); }
    void SetSettings(Settings* settings);
    void ApplyTheme(const std::wstring& theme);
    void SetZoom(double zoom);
    void SetFontScale(double scale);
    void SetContentWidth(int width);
    void SetWheelStep(int step);
    void SetRecent(const std::vector<std::wstring>& recent);

    void SetDocument(std::unique_ptr<Document> doc, bool preserveScroll = false,
                     bool preserveInk = false);
    void SetError(const std::wstring& message, const std::wstring& path, const std::wstring& sourceText);
    void ClearDocument();
    void SetSource(bool source);
    bool IsSource() const { return sourceMode_; }
    bool CanSource() const;
    bool CanOutline() const;
    Document* Doc() const { return doc_.get(); }
    bool HasDocument() const { return doc_ != nullptr; }
    bool CanAnnotate() const;
    bool InkMode() const { return inkMode_; }
    void SetInkMode(bool on);
    void UndoInk();

    void ScrollToBlock(int index);
    void ScrollToTop();
    int VisibleHeadingBlock() const;
    void CopySelection();
    void ScrollBy(float dx, float dy);
    void ScrollPage(int dir);
    void ScrollHome();
    void ScrollEnd();
    void RedoInk();
    bool HasUnsavedNotes() const { return notesDirty_; }
    Color CurrentInkColor() const;
    int CurrentInkColorIndex() const { return inkColor_; }
    void SelectInkColor(int index);
    float ViewWidthDip() const { return PageWidth(); }
    float ViewHeightDip() const { return heightDip(); }
    bool ExportPng(float scale, std::vector<uint8_t>& png, std::wstring* error,
                   int* outW = nullptr, int* outH = nullptr);
    bool ExportPngPage(float scale, float offsetY, float pageHeight,
                       std::vector<uint8_t>& png, std::wstring* error,
                       int* outW = nullptr, int* outH = nullptr);
    float ContentHeightDip();
    const std::wstring& NotesError() const { return notesError_; }
    bool CanSaveNotes() const { return doc_ && !doc_->path.empty(); }
    bool SaveNotes(std::wstring* error = nullptr);
    bool LoadNotes(std::wstring* error = nullptr);
    void CopyInkSelection();
    void PasteInkSelection();
    void DeleteInkSelection();
    void GroupInkSelection(bool group);
    void SetPdfHighlighter();
    bool CanHighlightPdf() const { return pdf_ && doc_ && doc_->kind == DocKind::Pdf && !sourceMode_; }
    void ExpandJson(bool expand);
    void FocusView();
    void RefreshLayout();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);

    HWND hwnd_ = nullptr;
    HWND parent_ = nullptr;
    std::unique_ptr<Canvas> canvas_;
    Settings* settings_ = nullptr;
    DocViewEvents events_;
    std::unique_ptr<Document> doc_;
    std::shared_ptr<PdfDocument> pdf_;
    std::wstring errorMessage_;
    std::wstring errorPath_;
    std::wstring errorSource_;

    bool dark_ = false;
    std::wstring theme_ = L"blue";
    bool sourceMode_ = false;
    double zoom_ = 1.0;
    double fontScale_ = 1.0;
    int contentWidth_ = 1040;
    int wheelStep_ = 3;
    float scrollX_ = 0;
    float scrollY_ = 0;
    std::vector<std::wstring> recent_;
    bool middlePan_ = false;
    POINT panAnchor_{};
    POINT panCurrent_{};
    UINT_PTR panTimer_ = 0;

    struct Layout;
    std::shared_ptr<Layout> layout_;
    bool layoutDirty_ = true;
    bool rebuilding_ = false;
    bool updatingScrollbars_ = false;
    bool vertScrollShown_ = true;
    bool horzScrollShown_ = true;
    float lastWidth_ = 0;
    float pendingScrollFrac_ = -1;
    float heightDip() const;

    int hoverCellRow_ = -1;
    int hoverCellCol_ = -1;
    int hoverGridColSep_ = -1;
    int hoverGridRowSep_ = -1;
    int hoverJsonRow_ = -1;
    int hoverWelcome_ = -1;
    int hoverLinkBlock_ = -1;
    std::wstring hoverLink_;
    DRect welcomeOpenRect_;
    DRect welcomeFolderRect_;
    DRect welcomeClearRect_;
    std::vector<DRect> welcomeRecentRects_;
    std::vector<DRect> welcomeRecentDeleteRects_;
    std::vector<std::wstring> welcomeRecentPaths_;
    std::vector<int> welcomeRecentRowIndices_;
    DRect welcomeRecentView_;
    float welcomeRecentScroll_ = 0;
    float welcomeRecentContentH_ = 0;
    int welcomeRecentShown_ = 0;
    DRect errorOpenRect_, errorSourceRect_, errorExternalRect_;
    struct CopyTarget {
        bool active = false;
        DRect rect;
        int kind = 0; // 1 code block, 2 table cell, 3 grid cell
        const Block* block = nullptr;
        int cellIndex = -1;
        int row = -1;
        int col = -1;
    } copyHover_;
    bool copyPressed_ = false;
    bool copyDone_ = false;
    UINT_PTR copyTimer_ = 0;

    struct InkStroke {
        std::vector<InkPoint> points;
        Color color;
        float width = 3.0f;
        int group = 0;
        bool highlighter = false;
    };
    struct InkShape {
        int kind = 0; // 0 line, 1 arrow, 2 rectangle, 3 ellipse
        InkPoint a;
        InkPoint b;
        Color color;
        float width = 3.0f;
        float rotation = 0.0f;
        int group = 0;
    };
    struct InkText {
        InkPoint at;
        std::wstring text;
        Color color;
        float size = 20.0f;
        float rotation = 0.0f;
        int group = 0;
    };
    struct TextHighlight {
        int block = 0;
        int start = 0;
        int length = 0;
        Color color = Color(1.0f, 0.82f, 0.18f, 0.34f);
    };
    struct InkSnapshot {
        std::vector<InkStroke> strokes;
        std::vector<InkShape> shapes;
        std::vector<InkText> texts;
        std::vector<TextHighlight> textHighlights;
    };
    struct InkLayer {
        std::vector<InkStroke> strokes;
        std::vector<InkShape> shapes;
        std::vector<InkText> texts;
        std::vector<InkSnapshot> undo;
        std::vector<InkSnapshot> redo;
        int groupSeq = 1;
    };
    struct InkItemRef {
        int kind = 0; // 0 stroke, 1 shape, 2 text
        int index = -1;
    };
    struct PdfHighlight {
        std::vector<DRect> rects; // normalized page coordinates, y down
        Color color = Color(1.0f, 0.82f, 0.18f, 0.34f);
    };
    struct InkButton {
        DRect rect;
        int action = 0;
    };
    bool inkMode_ = false;
    int inkTool_ = 0; // 0 pen, 1 eraser, 2 line, 3 arrow, 4 rectangle, 5 ellipse, 6 text, 7 select, 8 PDF highlighter
    bool inkDrawing_ = false;
    bool inkErasing_ = false;
    bool inkErasePushed_ = false;
    bool inkShapeDrawing_ = false;
    int inkColor_ = 0;
    float inkWidth_ = 3.0f;
    InkStroke inkCurrent_;
    InkShape inkCurrentShape_;
    std::map<int, InkLayer> inkLayers_;
    std::vector<InkButton> inkButtons_;
    std::vector<Color> inkColors_;
    DRect inkBarRect_;
    DRect inkMorePanel_;
    bool inkMoreOpen_ = false;
    int inkHover_ = 0;
    HWND inkEdit_ = nullptr;
    HFONT inkEditFont_ = nullptr;
    HBRUSH inkEditBrush_ = nullptr;
    InkPoint inkTextAnchor_;
    float inkTextSize_ = 20.0f;
    std::vector<InkItemRef> inkSelection_;
    bool inkLassoing_ = false;
    bool inkLassoMoved_ = false;
    bool inkLassoAdd_ = false;
    InkPoint inkLassoStart_;
    std::vector<InkPoint> inkLasso_;
    InkSnapshot inkClipboard_;
    bool inkClipboardValid_ = false;
    int inkDragMode_ = 0; // 1 move, 2 scale, 3 rotate
    bool inkDragMoved_ = false;
    InkPoint inkDragStart_;
    InkPoint inkDragPivot_;
    float inkDragStartDist_ = 1.0f;
    float inkDragStartAngle_ = 0.0f;
    InkSnapshot inkDragBase_;
    DRect inkSelectionBarRect_;
    std::map<int, std::vector<PdfHighlight>> pdfHighlights_;
    std::vector<TextHighlight> textHighlights_;
    std::vector<TextHighlight> textHighlightPending_;
    bool textHighlighting_ = false;
    int textHighlightStartBlock_ = 0;
    int textHighlightStartPos_ = 0;
    int textHighlightEndBlock_ = 0;
    int textHighlightEndPos_ = 0;
    std::map<int, std::vector<PdfChar>> pdfTextCache_;
    bool pdfHighlighting_ = false;
    int pdfHighlightPage_ = -1;
    DRect pdfHighlightPageRect_;
    DRect pdfHighlightDragRect_;
    std::vector<InkPoint> pdfHighlightPath_;
    bool notesDirty_ = false;
    std::wstring notesError_;
    int dragging_ = 0;        // 1 column, 2 row
    int dragIndex_ = -1;
    float dragStart_ = 0;
    float dragValue_ = 0;
    bool mouseDown_ = false;
    bool pdfRendering_ = false;
    bool exporting_ = false;
    int selectedRow_ = -1;
    int selectedCol_ = -1;

    struct GridState {
        int sheet = 0;
        std::vector<std::vector<float>> colWidths;
        std::vector<std::vector<float>> rowHeights;
        float scrollY = 0;
        float scrollX = 0;
    } grid_;

    struct JsonState {
        std::vector<JsonRow> rows;
        std::set<const JNode*> collapsed;
        float scrollX = 0;
        float scrollY = 0;
        float contentWidth = 0;
        bool built = false;
    } json_;

    struct SourceState {
        std::vector<std::wstring> lines;
        std::vector<std::wstring> rows;   // wrapped display rows
        std::vector<int> rowLines;        // logical line for each display row
        float scrollX = 0;
        float scrollY = 0;
        float contentWidth = 0;
        float wrapWidth = -1.0f;
        float wrapZoom = -1.0f;
        bool built = false;
    } source_;

    struct PdfState {
        std::vector<std::shared_ptr<ImageRef>> pages;
        std::vector<int> renderedWidth;
        float scrollY = 0;
        float scrollX = 0;
        float contentWidth = 0;
        bool loaded = false;
        std::wstring error;
    } pdfState_;

    void BuildSource();
    void BuildJson();
    void RelayoutIfNeeded();
    void UpdateScrollBars();
    void Paint();
    void PaintWelcome(class Canvas& c);
    void PaintError(class Canvas& c);
    void PaintDocument(class Canvas& c);
    void PaintSource(class Canvas& c);
    void PaintJson(class Canvas& c);
    void PaintGrid(class Canvas& c);
    void PaintPdf(class Canvas& c);
    void PaintImage(class Canvas& c);
    void RebuildGridWidths();
    void ImageDisplaySize(float viewW, float viewH, float& outW, float& outH) const;
    void BeginMiddlePan(int px, int py);
    void UpdateMiddlePan(int px, int py);
    void EndMiddlePan();
    void UpdateCopyHover(float x, float y);
    void DrawCopyButton(class Canvas& c);
    void ClearCopyFeedback();
    void WelcomeScrollRecent(int delta);
    void WelcomeResetRecentPaging();
    DRect CopyHitRect() const {
        if (!copyHover_.active) return {};
        const float pad = 4.0f;
        return { copyHover_.rect.x - pad, copyHover_.rect.y - pad,
                 copyHover_.rect.w + pad * 2, copyHover_.rect.h + pad * 2 };
    }

    bool ImageDisplayRect(float& x, float& y, float& w, float& h) const;
    int InkKey() const;
    InkLayer& Ink();
    bool InkImageSpace() const;
    bool InkScreenToContent(float sx, float sy, float& cx, float& cy) const;
    void InkContentToScreen(float cx, float cy, float& sx, float& sy) const;
    void InkPushUndo();
    void InkUndo();
    void InkRedo();
    void MarkNotesDirty();
    std::wstring NotesPath() const;
    void InkSelectionBounds(float& x0, float& y0, float& x1, float& y1) const;
    int InkGroupOf(const InkItemRef& ref) const;
    std::vector<InkItemRef> InkGroupMembers(int group) const;
    void InkNormalizeSelection();
    bool InkHitItem(float sx, float sy, InkItemRef& ref) const;
    bool InkSelectAt(float sx, float sy, bool add);
    void InkSelectLasso(bool add);
    void InkPaintLasso(class Canvas& c);
    void InkSelectBeginDrag(float sx, float sy, int mode);
    void InkSelectMoveDrag(float sx, float sy);
    void InkSelectEndDrag();
    void InkApplySelectionTransform(int mode, float dx, float dy, float scale, float angle);
    void InkPaintSelection(class Canvas& c);
    bool PdfPageRect(int index, float& x, float& y, float& w, float& h) const;
    void LoadPdfText(int page);
    void FinishPdfHighlight(float sx0, float sy0, float sx1, float sy1);
    bool BeginTextHighlight(float sx, float sy);
    void UpdateTextHighlight(float sx, float sy);
    void FinishTextHighlight();
    void PaintTextHighlights(class Canvas& c);
    void PaintPdfHighlights(class Canvas& c, int page, float x, float y, float w, float h);
    void InkClear();
    void InkScaleForZoom(float factor);
    void InkDrawStroke(class Canvas& c, const InkStroke& stroke);
    void InkDrawShape(class Canvas& c, const InkShape& shape);
    void InkDrawTextItem(class Canvas& c, const InkText& text);
    float InkTextWidth(const std::wstring& text, float size) const;
    const std::vector<Color>& InkPalette() const { return inkColors_; }
    void PaintInk(class Canvas& c, bool exportOnly = false);
    void PaintContent(class Canvas& c, bool exportOnly = false);
    void PaintInkToolbar(class Canvas& c);
    bool InkHitButton(float x, float y, int& action) const;
    void InkAction(int action);
    void InkBegin(float x, float y);
    void InkMove(float x, float y);
    void InkEnd();
    void BuildSourceRows(float width);
    void InkEraseAt(float x, float y);
    void InkOpenTextEditor(float x, float y);
    void InkCommitText();
    void InkCancelText();
    void InkCloseTextEditor();

    void OnMouseDown(int x, int y, bool ctrl);
    void OnMouseUp(int x, int y);
    void OnMouseMove(int x, int y);
    void OnMouseWheel(int delta, bool ctrl, bool shift, float x, float y);
    void OnKeyDown(WPARAM key);
    void OnCopy();
    void OpenLink(const std::wstring& url);

    int HitBlock(float x, float y) const;
    void ToggleBlock(int index);
    float PageWidth() const;
    void InvalidateAll();
};

class OutlineView {
public:
    static void Register(HINSTANCE inst);
    bool Create(HWND parent, int id);
    HWND hwnd() const { return hwnd_; }
    void SetItems(const std::vector<Heading>& items);
    void SetActive(int block);
    void ApplyTheme(const std::wstring& theme);
    void SetCallback(std::function<void(int)> cb) { callback_ = std::move(cb); }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);
    void Paint();
    void UpdateScroll();
    HWND hwnd_ = nullptr;
    std::vector<Heading> items_;
    int active_ = -1;
    int hover_ = -1;
    int scroll_ = 0;
    bool dark_ = false;
    std::wstring theme_ = L"blue";
    std::function<void(int)> callback_;
};
