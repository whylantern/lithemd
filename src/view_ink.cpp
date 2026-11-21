#include "view.h"
#include "view_internal.h"
#include "render.h"
#include "latex.h"
#include "pdf.h"
#include "i18n.h"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <map>
#include <functional>
#include <shellapi.h>
#include <commctrl.h>
#include <windowsx.h>

namespace {
float DistanceToSegment(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay;
    float len2 = dx * dx + dy * dy;
    if (len2 < 0.0001f) return std::sqrt((px - ax) * (px - ax) + (py - ay) * (py - ay));
    float t = Clampf(((px - ax) * dx + (py - ay) * dy) / len2, 0.0f, 1.0f);
    float qx = ax + dx * t, qy = ay + dy * t;
    return std::sqrt((px - qx) * (px - qx) + (py - qy) * (py - qy));
}

LRESULT CALLBACK InkEditSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                     UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            PostMessageW(GetParent(hwnd), WM_INK_COMMIT, 0, 0);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            PostMessageW(GetParent(hwnd), WM_INK_CANCEL, 0, 0);
            return 0;
        }
    }
    if (msg == WM_CHAR && (wp == L'\r' || wp == L'\n' || wp == 27)) return 0;
    if (msg == WM_KILLFOCUS) PostMessageW(GetParent(hwnd), WM_INK_COMMIT, 0, 0);
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, InkEditSubclassProc, 1);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

bool PointInPolygon(const std::vector<InkPoint>& poly, float x, float y) {
    if (poly.size() < 3) return false;
    bool inside = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        float xi = poly[i].x, yi = poly[i].y;
        float xj = poly[j].x, yj = poly[j].y;
        if (((yi > y) != (yj > y)) &&
            (x < (xj - xi) * (y - yi) / (yj - yi + 1e-20f) + xi))
            inside = !inside;
    }
    return inside;
}
} // namespace

// ---------------------------------------------------------------- ink
namespace {

JNode NoteNumber(double value) {
    JNode n;
    n.kind = JKind::Number;
    n.number = value;
    wchar_t buf[40];
    swprintf(buf, 40, L"%.3f", value);
    n.raw = buf;
    return n;
}

JNode NoteInt(int value) {
    JNode n;
    n.kind = JKind::Number;
    n.number = value;
    n.raw = std::to_wstring(value);
    return n;
}

JNode NoteBool(bool value) {
    JNode n;
    n.kind = JKind::Bool;
    n.boolean = value;
    return n;
}

JNode NoteString(const std::wstring& value) {
    JNode n;
    n.kind = JKind::String;
    n.str = value;
    return n;
}

JNode NoteArray() {
    JNode n;
    n.kind = JKind::Array;
    return n;
}

JNode NoteObject() {
    JNode n;
    n.kind = JKind::Object;
    return n;
}

JNode NotePoint(const InkPoint& p) {
    JNode n = NoteArray();
    n.items.push_back(NoteNumber(p.x));
    n.items.push_back(NoteNumber(p.y));
    return n;
}

JNode NoteColor(const Color& c) {
    JNode n = NoteArray();
    n.items.push_back(NoteNumber(c.r));
    n.items.push_back(NoteNumber(c.g));
    n.items.push_back(NoteNumber(c.b));
    n.items.push_back(NoteNumber(c.a));
    return n;
}

JNode NoteRect(const DRect& r) {
    JNode n = NoteArray();
    n.items.push_back(NoteNumber(r.x));
    n.items.push_back(NoteNumber(r.y));
    n.items.push_back(NoteNumber(r.w));
    n.items.push_back(NoteNumber(r.h));
    return n;
}

bool ReadNumber(const JNode* n, float& out) {
    if (!n) return false;
    if (n->kind == JKind::Number) { out = (float)n->number; return true; }
    if (n->kind == JKind::String) {
        wchar_t* end = nullptr;
        double v = wcstod(n->str.c_str(), &end);
        if (end && end != n->str.c_str()) { out = (float)v; return true; }
    }
    return false;
}

bool ReadInt(const JNode* n, int& out) {
    float v = 0;
    if (!ReadNumber(n, v)) return false;
    out = (int)std::lround(v);
    return true;
}

bool ReadPoint(const JNode* n, InkPoint& p) {
    if (!n || n->kind != JKind::Array || n->items.size() < 2) return false;
    return ReadNumber(&n->items[0], p.x) && ReadNumber(&n->items[1], p.y);
}

bool ReadColor(const JNode* n, Color& c) {
    if (!n || n->kind != JKind::Array || n->items.size() < 3) return false;
    ReadNumber(&n->items[0], c.r);
    ReadNumber(&n->items[1], c.g);
    ReadNumber(&n->items[2], c.b);
    c.a = 1.0f;
    if (n->items.size() > 3) ReadNumber(&n->items[3], c.a);
    return true;
}

bool ReadRect(const JNode* n, DRect& r) {
    if (!n || n->kind != JKind::Array || n->items.size() < 4) return false;
    return ReadNumber(&n->items[0], r.x) && ReadNumber(&n->items[1], r.y) &&
           ReadNumber(&n->items[2], r.w) && ReadNumber(&n->items[3], r.h);
}

} // namespace

bool DocView::CanAnnotate() const {
    return doc_ && errorMessage_.empty();
}

std::wstring DocView::NotesPath() const {
    if (!doc_ || doc_->path.empty()) return L"";
    std::wstring cfg = (settings_ && !settings_->configPath.empty())
        ? settings_->configPath : Settings::ResolveConfigPath();
    std::wstring normalized = ToLower(AbsolutePath(doc_->path));
    uint64_t hash = 1469598103934665603ull;
    for (wchar_t c : normalized) {
        hash ^= (uint64_t)c;
        hash *= 1099511628211ull;
    }
    wchar_t name[40];
    swprintf(name, 40, L"%016llX.json", (unsigned long long)hash);
    std::wstring dir = PathDir(cfg);
    std::wstring fresh = PathJoin(dir, L"lithemd-notes");
    std::wstring legacy = PathJoin(dir, L"openmd-notes");
    if (DirExists(legacy) && !DirExists(fresh)) return PathJoin(legacy, name);
    return PathJoin(fresh, name);
}

void DocView::ClearAnnotations() {
    if (inkEdit_) InkCommitText();
    inkLayers_.clear();
    pdfHighlights_.clear();
    textHighlights_.clear();
    inkSelection_.clear();
    inkLassoing_ = false;
    inkLasso_.clear();
    pdfHighlighting_ = false;
    notesDirty_ = false;
    notesError_.clear();
    InvalidateAll();
    if (events_.statusChanged) events_.statusChanged();
}

void DocView::MarkNotesDirty() {
    if (!notesDirty_) {
        notesDirty_ = true;
        if (events_.statusChanged) events_.statusChanged();
    }
}

bool DocView::SaveNotes(std::wstring* error) {
    if (error) error->clear();
    if (!doc_ || doc_->path.empty()) {
        if (error) *error = L"当前没有可保存的文档";
        return false;
    }
    if (inkEdit_) InkCommitText();
    if (inkDrawing_ || inkShapeDrawing_) InkEnd();

    JNode root = NoteObject();
    root.members.emplace_back(L"version", NoteInt(1));
    root.members.emplace_back(L"document", NoteString(AbsolutePath(doc_->path)));

    JNode layers = NoteObject();
    for (auto& entry : inkLayers_) {
        auto& layer = entry.second;
        if (layer.strokes.empty() && layer.shapes.empty() && layer.texts.empty()) continue;
        JNode layerNode = NoteObject();
        JNode strokes = NoteArray();
        for (auto& s : layer.strokes) {
            JNode n = NoteObject();
            JNode points = NoteArray();
            for (auto& p : s.points) points.items.push_back(NotePoint(p));
            n.members.emplace_back(L"points", points);
            n.members.emplace_back(L"color", NoteColor(s.color));
            n.members.emplace_back(L"width", NoteNumber(s.width));
            n.members.emplace_back(L"group", NoteInt(s.group));
            if (s.highlighter) n.members.emplace_back(L"highlighter", NoteBool(true));
            strokes.items.push_back(std::move(n));
        }
        JNode shapes = NoteArray();
        for (auto& s : layer.shapes) {
            JNode n = NoteObject();
            n.members.emplace_back(L"kind", NoteInt(s.kind));
            n.members.emplace_back(L"a", NotePoint(s.a));
            n.members.emplace_back(L"b", NotePoint(s.b));
            n.members.emplace_back(L"color", NoteColor(s.color));
            n.members.emplace_back(L"width", NoteNumber(s.width));
            n.members.emplace_back(L"rotation", NoteNumber(s.rotation));
            n.members.emplace_back(L"group", NoteInt(s.group));
            shapes.items.push_back(std::move(n));
        }
        JNode texts = NoteArray();
        for (auto& t : layer.texts) {
            JNode n = NoteObject();
            n.members.emplace_back(L"at", NotePoint(t.at));
            n.members.emplace_back(L"text", NoteString(t.text));
            n.members.emplace_back(L"color", NoteColor(t.color));
            n.members.emplace_back(L"size", NoteNumber(t.size));
            n.members.emplace_back(L"rotation", NoteNumber(t.rotation));
            n.members.emplace_back(L"group", NoteInt(t.group));
            texts.items.push_back(std::move(n));
        }
        layerNode.members.emplace_back(L"strokes", strokes);
        layerNode.members.emplace_back(L"shapes", shapes);
        layerNode.members.emplace_back(L"texts", texts);
        layers.members.emplace_back(std::to_wstring(entry.first), std::move(layerNode));
    }
    root.members.emplace_back(L"layers", std::move(layers));

    JNode highlights = NoteObject();
    for (auto& entry : pdfHighlights_) {
        if (entry.second.empty()) continue;
        JNode page = NoteArray();
        for (auto& h : entry.second) {
            JNode item = NoteObject();
            item.members.emplace_back(L"color", NoteColor(h.color));
            JNode rects = NoteArray();
            for (auto& r : h.rects) rects.items.push_back(NoteRect(r));
            item.members.emplace_back(L"rects", rects);
            page.items.push_back(std::move(item));
        }
        highlights.members.emplace_back(std::to_wstring(entry.first), std::move(page));
    }
    root.members.emplace_back(L"pdfHighlights", std::move(highlights));

    JNode textHl = NoteArray();
    for (auto& h : textHighlights_) {
        JNode item = NoteObject();
        item.members.emplace_back(L"block", NoteInt(h.block));
        item.members.emplace_back(L"start", NoteInt(h.start));
        item.members.emplace_back(L"length", NoteInt(h.length));
        item.members.emplace_back(L"color", NoteColor(h.color));
        textHl.items.push_back(std::move(item));
    }
    root.members.emplace_back(L"textHighlights", std::move(textHl));

    std::wstring path = NotesPath();
    if (path.empty()) {
        if (error) *error = L"无法确定笔记文件位置";
        return false;
    }
    std::wstring text = PrettyJson(root) + L"\n";
    EnsureDir(PathDir(path));
    std::wstring tmp = path + L".tmp";
    std::wstring writeError;
    if (!WriteTextFile(tmp, text)) {
        if (error) *error = L"无法写入笔记文件";
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        if (error) *error = L"无法替换笔记文件（可能被占用）";
        return false;
    }
    notesDirty_ = false;
    notesError_.clear();
    if (events_.statusChanged) events_.statusChanged();
    return true;
}

bool DocView::LoadNotes(std::wstring* error) {
    if (error) error->clear();
    notesError_.clear();
    if (!doc_ || doc_->path.empty()) return true;
    std::wstring path = NotesPath();
    if (path.empty() || !FileExists(path)) return true;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        uint64_t noteSize = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        if (noteSize > 64ull * 1024 * 1024) {
            notesError_ = L"笔记文件超过 64 MB，已跳过加载";
            if (error) *error = notesError_;
            return false;
        }
    }
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(path, bytes)) {
        notesError_ = L"无法读取笔记文件";
        if (error) *error = notesError_;
        return false;
    }
    std::wstring enc, text = DecodeBytes(bytes, enc);
    JNode root;
    if (!ParseJson(text, root) || root.kind != JKind::Object) {
        notesError_ = L"笔记文件格式无效";
        if (error) *error = notesError_;
        return false;
    }
    std::wstring stored;
    if (auto* docNode = root.Get(L"document"); docNode && docNode->kind == JKind::String)
        stored = docNode->str;
    if (!stored.empty() && _wcsicmp(stored.c_str(), AbsolutePath(doc_->path).c_str()) != 0) {
        notesError_ = L"笔记文件属于其他文档";
        if (error) *error = notesError_;
        return false;
    }

    inkLayers_.clear();
    pdfHighlights_.clear();
    inkSelection_.clear();
    if (auto* layers = root.Get(L"layers"); layers && layers->kind == JKind::Object) {
        for (auto& entry : layers->members) {
            wchar_t* end = nullptr;
            long key = wcstol(entry.first.c_str(), &end, 10);
            if (!end || end == entry.first.c_str()) continue;
            InkLayer layer;
            if (auto* strokes = entry.second.Get(L"strokes"); strokes && strokes->kind == JKind::Array) {
                for (auto& sn : strokes->items) {
                    if (sn.kind != JKind::Object) continue;
                    InkStroke s;
                    if (auto* points = sn.Get(L"points"); points && points->kind == JKind::Array) {
                        for (auto& pn : points->items) {
                            InkPoint p;
                            if (ReadPoint(&pn, p)) s.points.push_back(p);
                        }
                    }
                    if (s.points.empty()) continue;
                    if (auto* c = sn.Get(L"color")) ReadColor(c, s.color);
                    if (auto* w = sn.Get(L"width")) ReadNumber(w, s.width);
                    if (auto* g = sn.Get(L"group")) ReadInt(g, s.group);
                    if (auto* h = sn.Get(L"highlighter"); h && h->kind == JKind::Bool)
                        s.highlighter = h->boolean;
                    layer.groupSeq = std::max(layer.groupSeq, s.group + 1);
                    layer.strokes.push_back(std::move(s));
                }
            }
            if (auto* shapes = entry.second.Get(L"shapes"); shapes && shapes->kind == JKind::Array) {
                for (auto& sn : shapes->items) {
                    if (sn.kind != JKind::Object) continue;
                    InkShape s;
                    if (auto* k = sn.Get(L"kind")) ReadInt(k, s.kind);
                    if (auto* a = sn.Get(L"a")) ReadPoint(a, s.a);
                    if (auto* b = sn.Get(L"b")) ReadPoint(b, s.b);
                    if (auto* c = sn.Get(L"color")) ReadColor(c, s.color);
                    if (auto* w = sn.Get(L"width")) ReadNumber(w, s.width);
                    if (auto* r = sn.Get(L"rotation")) ReadNumber(r, s.rotation);
                    if (auto* g = sn.Get(L"group")) ReadInt(g, s.group);
                    layer.groupSeq = std::max(layer.groupSeq, s.group + 1);
                    layer.shapes.push_back(std::move(s));
                }
            }
            if (auto* texts = entry.second.Get(L"texts"); texts && texts->kind == JKind::Array) {
                for (auto& tn : texts->items) {
                    if (tn.kind != JKind::Object) continue;
                    InkText t;
                    if (auto* a = tn.Get(L"at")) ReadPoint(a, t.at);
                    if (auto* v = tn.Get(L"text"); v && v->kind == JKind::String) t.text = v->str;
                    if (auto* c = tn.Get(L"color")) ReadColor(c, t.color);
                    if (auto* sz = tn.Get(L"size")) ReadNumber(sz, t.size);
                    if (auto* r = tn.Get(L"rotation")) ReadNumber(r, t.rotation);
                    if (auto* g = tn.Get(L"group")) ReadInt(g, t.group);
                    if (t.text.empty()) continue;
                    layer.groupSeq = std::max(layer.groupSeq, t.group + 1);
                    layer.texts.push_back(std::move(t));
                }
            }
            inkLayers_[(int)key] = std::move(layer);
        }
    }
    if (auto* highlights = root.Get(L"pdfHighlights"); highlights && highlights->kind == JKind::Object) {
        for (auto& entry : highlights->members) {
            wchar_t* end = nullptr;
            long page = wcstol(entry.first.c_str(), &end, 10);
            if (!end || end == entry.first.c_str() || entry.second.kind != JKind::Array) continue;
            std::vector<PdfHighlight> list;
            for (auto& hn : entry.second.items) {
                if (hn.kind != JKind::Object) continue;
                PdfHighlight h;
                if (auto* color = hn.Get(L"color")) ReadColor(color, h.color);
                if (auto* rects = hn.Get(L"rects"); rects && rects->kind == JKind::Array) {
                    for (auto& rn : rects->items) {
                        DRect r;
                        if (ReadRect(&rn, r)) h.rects.push_back(r);
                    }
                }
                if (!h.rects.empty()) list.push_back(std::move(h));
            }
            if (!list.empty()) pdfHighlights_[(int)page] = std::move(list);
        }
    }
    if (auto* textHl = root.Get(L"textHighlights"); textHl && textHl->kind == JKind::Array) {
        textHighlights_.clear();
        for (auto& hn : textHl->items) {
            if (hn.kind != JKind::Object) continue;
            TextHighlight h;
            if (auto* b = hn.Get(L"block")) ReadInt(b, h.block);
            if (auto* s = hn.Get(L"start")) ReadInt(s, h.start);
            if (auto* l = hn.Get(L"length")) ReadInt(l, h.length);
            if (auto* c = hn.Get(L"color")) ReadColor(c, h.color);
            if (h.block >= 0 && h.start >= 0 && h.length > 0) textHighlights_.push_back(h);
        }
    }
    notesDirty_ = false;
    if (events_.statusChanged) events_.statusChanged();
    InvalidateAll();
    return true;
}

bool DocView::PdfPageRect(int index, float& x, float& y, float& w, float& h) const {
    if (!pdf_ || index < 0 || index >= pdf_->PageCount()) return false;
    float z = (float)zoom_;
    float pageBase = std::min(PageWidth() - 96.0f, 900.0f);
    if (pageBase < 260) pageBase = std::max(180.0f, PageWidth() - 40.0f);
    float pageW = pageBase * z;
    float gap = 20.0f;
    float yy = 26.0f - scrollY_;
    for (int i = 0; i < pdf_->PageCount(); i++) {
        float pw = 1, ph = 1;
        pdf_->PageSize(i, pw, ph);
        float pageH = pageW * (ph / std::max(1.0f, pw));
        if (i == index) {
            x = (PageWidth() - pageW) / 2 - (scrollX_ > 0 ? scrollX_ : 0);
            y = yy;
            w = pageW;
            h = pageH;
            return true;
        }
        yy += pageH + gap;
    }
    return false;
}

void DocView::LoadPdfText(int page) {
    if (!pdf_ || page < 0 || page >= pdf_->PageCount()) return;
    if (pdfTextCache_.find(page) != pdfTextCache_.end()) return;
    std::vector<PdfChar> raw;
    if (!pdf_->PageText(page, raw)) {
        pdfTextCache_[page] = {};
        return;
    }
    float pw = 1, ph = 1;
    pdf_->PageSize(page, pw, ph);
    if (pw <= 1 || ph <= 1) { pdfTextCache_[page] = {}; return; }
    std::vector<PdfChar> normalized;
    normalized.reserve(raw.size());
    for (auto& c : raw) {
        PdfChar n;
        n.left = c.left / pw;
        n.right = c.right / pw;
        float a = 1.0f - c.top / ph;
        float b = 1.0f - c.bottom / ph;
        n.top = std::min(a, b);
        n.bottom = std::max(a, b);
        n.ch = c.ch;
        normalized.push_back(n);
    }
    pdfTextCache_[page] = std::move(normalized);
}

void DocView::FinishPdfHighlight(float sx0, float sy0, float sx1, float sy1) {
    bool active = pdfHighlighting_;
    pdfHighlighting_ = false;
    pdfHighlightDragRect_ = DRect{};
    if (GetCapture() == hwnd_) ReleaseCapture();
    if (!active || pdfHighlightPage_ < 0 || !pdf_) { InvalidateAll(); return; }
    float px = 0, py = 0, pw = 0, ph = 0;
    if (!PdfPageRect(pdfHighlightPage_, px, py, pw, ph) || pw <= 1 || ph <= 1) {
        InvalidateAll();
        return;
    }
    float x0 = std::min(sx0, sx1), x1 = std::max(sx0, sx1);
    float y0 = std::min(sy0, sy1), y1 = std::max(sy0, sy1);
    x0 = Clampf(x0, px, px + pw);
    x1 = Clampf(x1, px, px + pw);
    y0 = Clampf(y0, py, py + ph);
    y1 = Clampf(y1, py, py + ph);
    if (x1 - x0 < 2 || y1 - y0 < 2) { InvalidateAll(); return; }
    DRect sel{ (x0 - px) / pw, (y0 - py) / ph, (x1 - x0) / pw, (y1 - y0) / ph };
    LoadPdfText(pdfHighlightPage_);
    auto& chars = pdfTextCache_[pdfHighlightPage_];
    struct LineBox { float top, bottom, left, right; };
    std::vector<LineBox> lines;
    for (auto& c : chars) {
        if (iswspace((wint_t)c.ch)) continue;
        if (c.right <= sel.x || c.left >= sel.x + sel.w ||
            c.bottom <= sel.y || c.top >= sel.y + sel.h) continue;
        bool merged = false;
        for (auto& line : lines) {
            float overlap = std::min(line.bottom, c.bottom) - std::max(line.top, c.top);
            if (overlap > std::min(line.bottom - line.top, c.bottom - c.top) * 0.45f) {
                line.top = std::min(line.top, c.top);
                line.bottom = std::max(line.bottom, c.bottom);
                line.left = std::min(line.left, c.left);
                line.right = std::max(line.right, c.right);
                merged = true;
                break;
            }
        }
        if (!merged) lines.push_back({ c.top, c.bottom, c.left, c.right });
    }
    PdfHighlight h;
    h.color = CurrentInkColor();
    h.color.a = 0.34f;
    for (auto& line : lines) {
        float padY = std::max(0.001f, (line.bottom - line.top) * 0.12f);
        h.rects.push_back({ line.left, line.top - padY,
                            std::max(0.001f, line.right - line.left),
                            (line.bottom - line.top) + padY * 2 });
    }
    if (h.rects.empty()) {
        // No text layer (e.g. scanned PDF): use a freehand highlighter stroke,
        // exactly like the pen fallback used by other non-text formats.
        InkPushUndo();
        InkStroke stroke;
        stroke.color = InkPalette().empty() ? HexColor(L"#FFD84D") : InkPalette()[(size_t)inkColor_];
        stroke.color.a = 1.0f;
        stroke.width = std::max(12.0f, inkWidth_ * 4.0f);
        stroke.highlighter = true;
        if (pdfHighlightPath_.size() < 2)
            pdfHighlightPath_ = { { sx0, sy0 }, { sx1, sy1 } };
        for (auto& p : pdfHighlightPath_) {
            float cx = 0, cy = 0;
            if (InkScreenToContent(p.x, p.y, cx, cy)) stroke.points.push_back({ cx, cy });
        }
        if (stroke.points.size() >= 2) Ink().strokes.push_back(std::move(stroke));
        pdfHighlightPath_.clear();
        MarkNotesDirty();
        InvalidateAll();
        return;
    }
    pdfHighlights_[pdfHighlightPage_].push_back(std::move(h));
    pdfHighlightPath_.clear();
    MarkNotesDirty();
    InvalidateAll();
}

void DocView::PaintPdfHighlights(Canvas& c, int page, float x, float y, float w, float h) {
    auto it = pdfHighlights_.find(page);
    if (it != pdfHighlights_.end()) {
        for (auto& hl : it->second) {
            for (auto& r : hl.rects)
                c.FillRect(x + r.x * w, y + r.y * h, r.w * w, r.h * h, hl.color);
        }
    }
    if (pdfHighlighting_ && pdfHighlightPage_ == page) {
        float x0 = std::min(pdfHighlightDragRect_.x, pdfHighlightDragRect_.x + pdfHighlightDragRect_.w);
        float x1 = std::max(pdfHighlightDragRect_.x, pdfHighlightDragRect_.x + pdfHighlightDragRect_.w);
        float y0 = std::min(pdfHighlightDragRect_.y, pdfHighlightDragRect_.y + pdfHighlightDragRect_.h);
        float y1 = std::max(pdfHighlightDragRect_.y, pdfHighlightDragRect_.y + pdfHighlightDragRect_.h);
        x0 = Clampf(x0, x, x + w); x1 = Clampf(x1, x, x + w);
        y0 = Clampf(y0, y, y + h); y1 = Clampf(y1, y, y + h);
        if (x1 > x0 && y1 > y0)
            c.FillRect(x0, y0, x1 - x0, y1 - y0, Color(CurrentInkColor().r, CurrentInkColor().g,
                                                        CurrentInkColor().b, 0.22f));
    }
}

int DocView::InkKey() const {
    if (!doc_) return 0;
    if (sourceMode_) return 2;
    if (doc_->kind == DocKind::Json) return 3;
    if (doc_->kind == DocKind::Csv || doc_->kind == DocKind::Xlsx || doc_->kind == DocKind::Xls)
        return 100 + grid_.sheet;
    if (doc_->kind == DocKind::Pdf) return 4;
    if (doc_->kind == DocKind::Image) return 5;
    return 1;
}

DocView::InkLayer& DocView::Ink() {
    return inkLayers_[InkKey()];
}

bool DocView::InkImageSpace() const {
    return doc_ && doc_->kind == DocKind::Image && !sourceMode_;
}

bool DocView::InkScreenToContent(float sx, float sy, float& cx, float& cy) const {
    if (!doc_) return false;
    if (InkImageSpace()) {
        float x = 0, y = 0, w = 0, h = 0;
        if (!ImageDisplayRect(x, y, w, h)) return false;
        if (sx < x || sy < y || sx > x + w || sy > y + h) return false;
        cx = (sx - x) / w;
        cy = (sy - y) / h;
        return true;
    }
    cx = sx + scrollX_;
    cy = sy + scrollY_;
    return true;
}

void DocView::InkContentToScreen(float cx, float cy, float& sx, float& sy) const {
    if (InkImageSpace()) {
        float x = 0, y = 0, w = 0, h = 0;
        if (ImageDisplayRect(x, y, w, h)) {
            sx = x + cx * w;
            sy = y + cy * h;
            return;
        }
    }
    sx = cx - scrollX_;
    sy = cy - scrollY_;
}

int DocView::InkGroupOf(const InkItemRef& ref) const {
    auto it = inkLayers_.find(InkKey());
    if (it == inkLayers_.end()) return 0;
    auto& layer = it->second;
    if (ref.kind == 0 && ref.index >= 0 && ref.index < (int)layer.strokes.size())
        return layer.strokes[(size_t)ref.index].group;
    if (ref.kind == 1 && ref.index >= 0 && ref.index < (int)layer.shapes.size())
        return layer.shapes[(size_t)ref.index].group;
    if (ref.kind == 2 && ref.index >= 0 && ref.index < (int)layer.texts.size())
        return layer.texts[(size_t)ref.index].group;
    return 0;
}

std::vector<DocView::InkItemRef> DocView::InkGroupMembers(int group) const {
    std::vector<InkItemRef> refs;
    if (!group) return refs;
    auto it = inkLayers_.find(InkKey());
    if (it == inkLayers_.end()) return refs;
    auto& layer = it->second;
    for (int i = 0; i < (int)layer.strokes.size(); i++)
        if (layer.strokes[(size_t)i].group == group) refs.push_back({ 0, i });
    for (int i = 0; i < (int)layer.shapes.size(); i++)
        if (layer.shapes[(size_t)i].group == group) refs.push_back({ 1, i });
    for (int i = 0; i < (int)layer.texts.size(); i++)
        if (layer.texts[(size_t)i].group == group) refs.push_back({ 2, i });
    return refs;
}

void DocView::InkNormalizeSelection() {
    auto it = inkLayers_.find(InkKey());
    if (it == inkLayers_.end()) {
        inkSelection_.clear();
        return;
    }
    auto& layer = it->second;
    std::set<int> groups;
    for (auto& ref : inkSelection_) {
        int group = InkGroupOf(ref);
        if (group) groups.insert(group);
    }
    std::vector<InkItemRef> normalized;
    auto add = [&](int kind, int index) {
        for (auto& r : normalized)
            if (r.kind == kind && r.index == index) return;
        normalized.push_back({ kind, index });
    };
    for (auto& ref : inkSelection_)
        if (ref.index >= 0 &&
            ((ref.kind == 0 && ref.index < (int)layer.strokes.size()) ||
             (ref.kind == 1 && ref.index < (int)layer.shapes.size()) ||
             (ref.kind == 2 && ref.index < (int)layer.texts.size())))
            add(ref.kind, ref.index);
    for (int group : groups) {
        for (int i = 0; i < (int)layer.strokes.size(); i++)
            if (layer.strokes[(size_t)i].group == group) add(0, i);
        for (int i = 0; i < (int)layer.shapes.size(); i++)
            if (layer.shapes[(size_t)i].group == group) add(1, i);
        for (int i = 0; i < (int)layer.texts.size(); i++)
            if (layer.texts[(size_t)i].group == group) add(2, i);
    }
    inkSelection_ = std::move(normalized);
}

void DocView::InkSelectionBounds(float& x0, float& y0, float& x1, float& y1) const {
    x0 = y0 = 1e9f;
    x1 = y1 = -1e9f;
    auto it = inkLayers_.find(InkKey());
    if (it == inkLayers_.end()) return;
    auto& layer = it->second;
    auto include = [&](float x, float y) {
        x0 = std::min(x0, x); y0 = std::min(y0, y);
        x1 = std::max(x1, x); y1 = std::max(y1, y);
    };
    for (auto& ref : inkSelection_) {
        if (ref.kind == 0 && ref.index >= 0 && ref.index < (int)layer.strokes.size()) {
            auto& s = layer.strokes[(size_t)ref.index];
            for (auto& p : s.points) include(p.x, p.y);
        } else if (ref.kind == 1 && ref.index >= 0 && ref.index < (int)layer.shapes.size()) {
            auto& s = layer.shapes[(size_t)ref.index];
            float cx = (s.a.x + s.b.x) / 2, cy = (s.a.y + s.b.y) / 2;
            float hw = std::abs(s.b.x - s.a.x) / 2, hh = std::abs(s.b.y - s.a.y) / 2;
            if (std::abs(s.rotation) < 0.0001f) {
                include(std::min(s.a.x, s.b.x), std::min(s.a.y, s.b.y));
                include(std::max(s.a.x, s.b.x), std::max(s.a.y, s.b.y));
            } else {
                for (int k = 0; k < 4; k++) {
                    float px = (k == 0 || k == 3) ? cx - hw : cx + hw;
                    float py = (k < 2) ? cy - hh : cy + hh;
                    float rx = cx + (px - cx) * std::cos(s.rotation) - (py - cy) * std::sin(s.rotation);
                    float ry = cy + (px - cx) * std::sin(s.rotation) + (py - cy) * std::cos(s.rotation);
                    include(rx, ry);
                }
            }
        } else if (ref.kind == 2 && ref.index >= 0 && ref.index < (int)layer.texts.size()) {
            auto& t = layer.texts[(size_t)ref.index];
            float w = InkTextWidth(t.text, t.size) + 8, h = t.size * 1.5f;
            for (int k = 0; k < 4; k++) {
                float px = t.at.x + ((k == 0 || k == 3) ? 0 : w);
                float py = t.at.y + (k < 2 ? 0 : h);
                float rx = t.at.x + (px - t.at.x) * std::cos(t.rotation) - (py - t.at.y) * std::sin(t.rotation);
                float ry = t.at.y + (px - t.at.x) * std::sin(t.rotation) + (py - t.at.y) * std::cos(t.rotation);
                include(rx, ry);
            }
        }
    }
    if (x1 < x0) return;
    float pad = InkImageSpace() ? 0.006f : 8.0f;
    x0 -= pad; y0 -= pad; x1 += pad; y1 += pad;
}

bool DocView::InkHitItem(float sx, float sy, InkItemRef& ref) const {
    auto it = inkLayers_.find(InkKey());
    if (it == inkLayers_.end()) return false;
    auto& layer = it->second;
    auto rotateBack = [](float& x, float& y, float cx, float cy, float a) {
        float dx = x - cx, dy = y - cy;
        x = cx + dx * std::cos(-a) - dy * std::sin(-a);
        y = cy + dx * std::sin(-a) + dy * std::cos(-a);
    };
    for (int i = (int)layer.texts.size() - 1; i >= 0; i--) {
        auto& t = layer.texts[(size_t)i];
        float tx = 0, ty = 0;
        InkContentToScreen(t.at.x, t.at.y, tx, ty);
        float px = sx, py = sy;
        if (std::abs(t.rotation) > 0.0001f) rotateBack(px, py, tx, ty, t.rotation);
        float w = InkTextWidth(t.text, t.size) + 8, h = t.size * 1.5f;
        if (px >= tx && px <= tx + w && py >= ty && py <= ty + h) {
            ref = { 2, i };
            return true;
        }
    }
    for (int i = (int)layer.shapes.size() - 1; i >= 0; i--) {
        auto& s = layer.shapes[(size_t)i];
        float ax = 0, ay = 0, bx = 0, by = 0;
        InkContentToScreen(s.a.x, s.a.y, ax, ay);
        InkContentToScreen(s.b.x, s.b.y, bx, by);
        float px = sx, py = sy;
        float cx = (ax + bx) / 2, cy = (ay + by) / 2;
        if (std::abs(s.rotation) > 0.0001f) rotateBack(px, py, cx, cy, s.rotation);
        bool hit = false;
        if (s.kind == INK_SHAPE_LINE || s.kind == INK_SHAPE_ARROW) {
            hit = DistanceToSegment(px, py, ax, ay, bx, by) <= 7.0f;
        } else if (s.kind == INK_SHAPE_RECT) {
            float x0 = std::min(ax, bx), y0 = std::min(ay, by);
            float x1 = std::max(ax, bx), y1 = std::max(ay, by);
            hit = (px >= x0 - 7 && px <= x1 + 7 && py >= y0 - 7 && py <= y1 + 7) &&
                  !(px > x0 + 7 && px < x1 - 7 && py > y0 + 7 && py < y1 - 7);
        } else {
            float rx = std::abs(bx - ax) / 2, ry = std::abs(by - ay) / 2;
            float nx = (px - cx) / std::max(1.0f, rx), ny = (py - cy) / std::max(1.0f, ry);
            hit = std::abs(nx * nx + ny * ny - 1.0f) < 0.18f;
        }
        if (hit) { ref = { 1, i }; return true; }
    }
    for (int i = (int)layer.strokes.size() - 1; i >= 0; i--) {
        auto& s = layer.strokes[(size_t)i];
        if (s.points.empty()) continue;
        float ax = 0, ay = 0;
        InkContentToScreen(s.points[0].x, s.points[0].y, ax, ay);
        if (s.points.size() == 1) {
            float dx = sx - ax, dy = sy - ay;
            if (dx * dx + dy * dy <= 64.0f) { ref = { 0, i }; return true; }
            continue;
        }
        for (size_t k = 1; k < s.points.size(); k++) {
            float bx = 0, by = 0;
            InkContentToScreen(s.points[k].x, s.points[k].y, bx, by);
            if (DistanceToSegment(sx, sy, ax, ay, bx, by) <= std::max(6.0f, s.width * 1.5f)) {
                ref = { 0, i };
                return true;
            }
            ax = bx; ay = by;
        }
    }
    return false;
}

bool DocView::InkSelectAt(float sx, float sy, bool add) {
    auto it = inkLayers_.find(InkKey());
    if (it == inkLayers_.end()) return false;
    auto& layer = it->second;
    InkItemRef hit;
    if (!InkHitItem(sx, sy, hit)) {
        if (!add) inkSelection_.clear();
        InvalidateAll();
        return false;
    }
    std::vector<InkItemRef> picked;
    int group = InkGroupOf(hit);
    if (group != 0) {
        picked = InkGroupMembers(group);
    } else {
        picked.push_back(hit);
    }
    if (!add) {
        inkSelection_ = std::move(picked);
    } else {
        bool wasSelected = false;
        for (auto& p : picked) {
            for (auto& s : inkSelection_) if (s.kind == p.kind && s.index == p.index) { wasSelected = true; break; }
        }
        if (wasSelected) {
            for (auto& p : picked) {
                inkSelection_.erase(std::remove_if(inkSelection_.begin(), inkSelection_.end(),
                    [&](const InkItemRef& s) { return s.kind == p.kind && s.index == p.index; }),
                    inkSelection_.end());
            }
        } else {
            inkSelection_.insert(inkSelection_.end(), picked.begin(), picked.end());
        }
    }
    InkNormalizeSelection();
    InvalidateAll();
    return true;
}

void DocView::InkSelectLasso(bool add) {
    if (inkLasso_.size() < 3) return;
    auto& layer = Ink();
    std::vector<InkItemRef> picked;
    auto already = [&](const InkItemRef& r) {
        for (auto& p : picked) if (p.kind == r.kind && p.index == r.index) return true;
        return false;
    };
    auto include = [&](int kind, int index) {
        InkItemRef r{ kind, index };
        if (!already(r)) picked.push_back(r);
    };
    auto inside = [&](float x, float y) { return PointInPolygon(inkLasso_, x, y); };

    for (int i = 0; i < (int)layer.strokes.size(); i++) {
        auto& s = layer.strokes[(size_t)i];
        if (s.points.empty()) continue;
        bool all = true;
        for (auto& p : s.points) {
            float sx = 0, sy = 0;
            InkContentToScreen(p.x, p.y, sx, sy);
            if (!inside(sx, sy)) { all = false; break; }
        }
        if (all) include(0, i);
    }
    for (int i = 0; i < (int)layer.shapes.size(); i++) {
        auto& s = layer.shapes[(size_t)i];
        float ax = 0, ay = 0, bx = 0, by = 0;
        InkContentToScreen(s.a.x, s.a.y, ax, ay);
        InkContentToScreen(s.b.x, s.b.y, bx, by);
        float cx = (ax + bx) / 2, cy = (ay + by) / 2;
        bool all = true;
        if (s.kind == INK_SHAPE_LINE || s.kind == INK_SHAPE_ARROW) {
            all = inside(ax, ay) && inside(bx, by);
        } else {
            for (int k = 0; k < 4 && all; k++) {
                float px = (k == 0 || k == 3) ? ax : bx;
                float py = (k < 2) ? ay : by;
                if (std::abs(s.rotation) > 0.0001f) {
                    float dx = px - cx, dy = py - cy;
                    px = cx + dx * std::cos(s.rotation) - dy * std::sin(s.rotation);
                    py = cy + dx * std::sin(s.rotation) + dy * std::cos(s.rotation);
                }
                all = inside(px, py);
            }
        }
        if (all) include(1, i);
    }
    for (int i = 0; i < (int)layer.texts.size(); i++) {
        auto& t = layer.texts[(size_t)i];
        float sx = 0, sy = 0;
        InkContentToScreen(t.at.x, t.at.y, sx, sy);
        float w = InkTextWidth(t.text, t.size) + 8, h = t.size * 1.5f;
        bool all = true;
        for (int k = 0; k < 4 && all; k++) {
            float px = sx + ((k == 0 || k == 3) ? 0 : w);
            float py = sy + (k < 2 ? 0 : h);
            if (std::abs(t.rotation) > 0.0001f) {
                float dx = px - sx, dy = py - sy;
                px = sx + dx * std::cos(t.rotation) - dy * std::sin(t.rotation);
                py = sy + dx * std::sin(t.rotation) + dy * std::cos(t.rotation);
            }
            all = inside(px, py);
        }
        if (all) include(2, i);
    }

    // A group is one logical object: if any member is fully enclosed, the
    // whole group joins the selection even if another member sits outside.
    std::set<int> groups;
    for (auto& r : picked) {
        int g = 0;
        if (r.kind == 0) g = layer.strokes[(size_t)r.index].group;
        else if (r.kind == 1) g = layer.shapes[(size_t)r.index].group;
        else g = layer.texts[(size_t)r.index].group;
        if (g) groups.insert(g);
    }
    if (!groups.empty()) {
        for (int i = 0; i < (int)layer.strokes.size(); i++)
            if (groups.count(layer.strokes[(size_t)i].group)) include(0, i);
        for (int i = 0; i < (int)layer.shapes.size(); i++)
            if (groups.count(layer.shapes[(size_t)i].group)) include(1, i);
        for (int i = 0; i < (int)layer.texts.size(); i++)
            if (groups.count(layer.texts[(size_t)i].group)) include(2, i);
    }

    if (!add) {
        inkSelection_ = std::move(picked);
    } else {
        for (auto& r : picked) {
            bool exists = false;
            for (auto& s : inkSelection_)
                if (s.kind == r.kind && s.index == r.index) { exists = true; break; }
            if (!exists) inkSelection_.push_back(r);
        }
    }
    InkNormalizeSelection();
    InvalidateAll();
}

void DocView::InkPaintLasso(Canvas& c) {
    if (!inkLassoing_ || inkLasso_.size() < 2) return;
    Color color = c.palette().accent;
    color.a = 0.9f;
    auto dash = [&](float x1, float y1, float x2, float y2) {
        float dx = x2 - x1, dy = y2 - y1;
        float len = std::hypot(dx, dy);
        if (len < 0.5f) return;
        const float dashLen = 6.0f, gapLen = 4.0f;
        float pos = 0;
        while (pos < len) {
            float end = std::min(len, pos + dashLen);
            float ax = x1 + dx * (pos / len), ay = y1 + dy * (pos / len);
            float bx = x1 + dx * (end / len), by = y1 + dy * (end / len);
            c.DrawLine(ax, ay, bx, by, color, 1.3f);
            pos = end + gapLen;
        }
    };
    for (size_t i = 1; i < inkLasso_.size(); i++)
        dash(inkLasso_[i - 1].x, inkLasso_[i - 1].y, inkLasso_[i].x, inkLasso_[i].y);
}

void DocView::InkSelectBeginDrag(float sx, float sy, int mode) {
    InkNormalizeSelection();
    float x0, y0, x1, y1;
    InkSelectionBounds(x0, y0, x1, y1);
    if (x1 < x0) return;
    inkDragMode_ = mode;
    inkDragMoved_ = false;
    inkDragStart_ = { sx, sy };
    float pcx = (x0 + x1) / 2, pcy = (y0 + y1) / 2;
    InkContentToScreen(pcx, pcy, inkDragPivot_.x, inkDragPivot_.y);
    inkDragStartDist_ = std::max(1.0f, std::hypot(sx - inkDragPivot_.x, sy - inkDragPivot_.y));
    inkDragStartAngle_ = std::atan2(sy - inkDragPivot_.y, sx - inkDragPivot_.x);
    auto& layer = Ink();
    inkDragBase_.strokes = layer.strokes;
    inkDragBase_.shapes = layer.shapes;
    inkDragBase_.texts = layer.texts;
    SetCapture(hwnd_);
}

void DocView::InkSelectMoveDrag(float sx, float sy) {
    if (!inkDragMode_) return;
    float dx = sx - inkDragStart_.x, dy = sy - inkDragStart_.y;
    if (!inkDragMoved_) {
        if (dx * dx + dy * dy < 4.0f) return;
        InkPushUndo();
        inkDragMoved_ = true;
    }
    float scale = 1.0f, angle = 0.0f;
    if (inkDragMode_ == 2) {
        float d = std::hypot(sx - inkDragPivot_.x, sy - inkDragPivot_.y);
        scale = Clampf(d / inkDragStartDist_, 0.1f, 10.0f);
    } else if (inkDragMode_ == 3) {
        angle = std::atan2(sy - inkDragPivot_.y, sx - inkDragPivot_.x) - inkDragStartAngle_;
    }
    InkApplySelectionTransform(inkDragMode_, dx, dy, scale, angle);
    InvalidateAll();
}

void DocView::InkSelectEndDrag() {
    if (!inkDragMode_) return;
    inkDragMode_ = 0;
    if (GetCapture() == hwnd_) ReleaseCapture();
    InvalidateAll();
}

void DocView::InkApplySelectionTransform(int mode, float dx, float dy, float scale, float angle) {
    InkNormalizeSelection();
    auto& layer = Ink();
    layer.strokes = inkDragBase_.strokes;
    layer.shapes = inkDragBase_.shapes;
    layer.texts = inkDragBase_.texts;

    // Work in screen space: image annotations use normalized content
    // coordinates, so adding a screen-space drag delta directly to content
    // coordinates would throw them off the image (or make them disappear).
    auto toContent = [&](float sx, float sy, InkPoint& p) {
        if (InkImageSpace()) {
            float x = 0, y = 0, w = 0, h = 0;
            if (ImageDisplayRect(x, y, w, h) && w > 0 && h > 0) {
                p.x = (sx - x) / w;
                p.y = (sy - y) / h;
                return;
            }
        }
        p.x = sx + scrollX_;
        p.y = sy + scrollY_;
    };
    auto movePoint = [&](InkPoint& p) {
        float sx = 0, sy = 0;
        InkContentToScreen(p.x, p.y, sx, sy);
        if (mode == 1) {
            sx += dx;
            sy += dy;
        } else if (mode == 2) {
            sx = inkDragPivot_.x + (sx - inkDragPivot_.x) * scale;
            sy = inkDragPivot_.y + (sy - inkDragPivot_.y) * scale;
        } else if (mode == 3) {
            float qx = sx - inkDragPivot_.x, qy = sy - inkDragPivot_.y;
            sx = inkDragPivot_.x + qx * std::cos(angle) - qy * std::sin(angle);
            sy = inkDragPivot_.y + qx * std::sin(angle) + qy * std::cos(angle);
        }
        toContent(sx, sy, p);
    };
    for (auto& ref : inkSelection_) {
        if (ref.kind == 0 && ref.index >= 0 && ref.index < (int)layer.strokes.size()) {
            auto& s = layer.strokes[(size_t)ref.index];
            for (auto& p : s.points) movePoint(p);
            if (mode == 2) s.width *= scale;
        } else if (ref.kind == 1 && ref.index >= 0 && ref.index < (int)layer.shapes.size()) {
            auto& s = layer.shapes[(size_t)ref.index];
            if (mode == 3 && (s.kind == INK_SHAPE_RECT || s.kind == INK_SHAPE_ELLIPSE)) {
                InkPoint c{ (s.a.x + s.b.x) / 2, (s.a.y + s.b.y) / 2 };
                InkPoint half{ (s.b.x - s.a.x) / 2, (s.b.y - s.a.y) / 2 };
                movePoint(c);
                s.a = { c.x - half.x, c.y - half.y };
                s.b = { c.x + half.x, c.y + half.y };
                s.rotation += angle;
            } else {
                movePoint(s.a);
                movePoint(s.b);
                if (mode == 2) s.width *= scale;
            }
        } else if (ref.kind == 2 && ref.index >= 0 && ref.index < (int)layer.texts.size()) {
            auto& t = layer.texts[(size_t)ref.index];
            movePoint(t.at);
            if (mode == 2) t.size *= scale;
            if (mode == 3) t.rotation += angle;
        }
    }
}

void DocView::InkPaintSelection(Canvas& c) {
    if (inkTool_ != INK_TOOL_SELECT || inkSelection_.empty()) return;
    float x0, y0, x1, y1;
    InkSelectionBounds(x0, y0, x1, y1);
    if (x1 <= x0 || y1 <= y0) return;
    float sx0, sy0, sx1, sy1;
    InkContentToScreen(x0, y0, sx0, sy0);
    InkContentToScreen(x1, y1, sx1, sy1);
    auto& p = c.palette();
    c.StrokeRect(sx0, sy0, sx1 - sx0, sy1 - sy0, p.accent, 1.5f);
    float hs = 5;
    float xs[4] = { sx0, sx1, sx0, sx1 };
    float ys[4] = { sy0, sy0, sy1, sy1 };
    for (int i = 0; i < 4; i++) c.FillRect(xs[i] - hs, ys[i] - hs, hs * 2, hs * 2, p.accent);
    float cx = (sx0 + sx1) / 2;
    c.DrawLine(cx, sy0, cx, sy0 - 22, p.accent, 1.2f);
    c.FillCircle(cx, sy0 - 22, 5, p.accent);
}

void DocView::CopyInkSelection() {
    InkNormalizeSelection();
    auto& layer = Ink();
    inkClipboard_ = InkSnapshot{};
    for (auto& ref : inkSelection_) {
        if (ref.kind == 0 && ref.index < (int)layer.strokes.size())
            inkClipboard_.strokes.push_back(layer.strokes[(size_t)ref.index]);
        else if (ref.kind == 1 && ref.index < (int)layer.shapes.size())
            inkClipboard_.shapes.push_back(layer.shapes[(size_t)ref.index]);
        else if (ref.kind == 2 && ref.index < (int)layer.texts.size())
            inkClipboard_.texts.push_back(layer.texts[(size_t)ref.index]);
    }
    inkClipboardValid_ = !inkClipboard_.strokes.empty() || !inkClipboard_.shapes.empty() ||
                         !inkClipboard_.texts.empty();
    InvalidateAll();
}

void DocView::PasteInkSelection() {
    if (!inkMode_ || !inkClipboardValid_) return;
    auto& layer = Ink();
    InkPushUndo();
    inkSelection_.clear();
    std::map<int, int> groupMap;
    auto remapGroup = [&](int oldGroup) {
        if (!oldGroup) return 0;
        auto it = groupMap.find(oldGroup);
        if (it != groupMap.end()) return it->second;
        int gid = layer.groupSeq++;
        groupMap[oldGroup] = gid;
        return gid;
    };
    for (auto& s : inkClipboard_.strokes) {
        InkStroke copy = s;
        for (auto& p : copy.points) { p.x += 20; p.y += 20; }
        copy.group = remapGroup(copy.group);
        layer.strokes.push_back(std::move(copy));
        inkSelection_.push_back({ 0, (int)layer.strokes.size() - 1 });
    }
    for (auto& s : inkClipboard_.shapes) {
        InkShape copy = s;
        copy.a.x += 20; copy.a.y += 20; copy.b.x += 20; copy.b.y += 20;
        copy.group = remapGroup(copy.group);
        layer.shapes.push_back(std::move(copy));
        inkSelection_.push_back({ 1, (int)layer.shapes.size() - 1 });
    }
    for (auto& t : inkClipboard_.texts) {
        InkText copy = t;
        copy.at.x += 20; copy.at.y += 20;
        copy.group = remapGroup(copy.group);
        layer.texts.push_back(std::move(copy));
        inkSelection_.push_back({ 2, (int)layer.texts.size() - 1 });
    }
    InkNormalizeSelection();
    MarkNotesDirty();
    InvalidateAll();
}

void DocView::DeleteInkSelection() {
    if (!inkMode_ || inkSelection_.empty()) return;
    InkNormalizeSelection();
    auto& layer = Ink();
    InkPushUndo();
    std::vector<InkItemRef> refs = inkSelection_;
    std::sort(refs.begin(), refs.end(), [](const InkItemRef& a, const InkItemRef& b) {
        if (a.kind != b.kind) return a.kind > b.kind;
        return a.index > b.index;
    });
    for (auto& ref : refs) {
        if (ref.kind == 0 && ref.index >= 0 && ref.index < (int)layer.strokes.size())
            layer.strokes.erase(layer.strokes.begin() + ref.index);
        else if (ref.kind == 1 && ref.index >= 0 && ref.index < (int)layer.shapes.size())
            layer.shapes.erase(layer.shapes.begin() + ref.index);
        else if (ref.kind == 2 && ref.index >= 0 && ref.index < (int)layer.texts.size())
            layer.texts.erase(layer.texts.begin() + ref.index);
    }
    inkSelection_.clear();
    InvalidateAll();
}

void DocView::GroupInkSelection(bool group) {
    if (!inkMode_) return;
    InkNormalizeSelection();
    if (inkSelection_.empty()) return;
    auto& layer = Ink();
    if (group && inkSelection_.size() < 2) return;
    InkPushUndo();
    if (group) {
        int gid = layer.groupSeq++;
        for (auto& ref : inkSelection_) {
            if (ref.kind == 0 && ref.index < (int)layer.strokes.size()) layer.strokes[(size_t)ref.index].group = gid;
            else if (ref.kind == 1 && ref.index < (int)layer.shapes.size()) layer.shapes[(size_t)ref.index].group = gid;
            else if (ref.kind == 2 && ref.index < (int)layer.texts.size()) layer.texts[(size_t)ref.index].group = gid;
        }
    } else {
        std::set<int> groups;
        for (auto& ref : inkSelection_) {
            int g = InkGroupOf(ref);
            if (g) groups.insert(g);
        }
        for (int i = 0; i < (int)layer.strokes.size(); i++)
            if (groups.count(layer.strokes[(size_t)i].group)) layer.strokes[(size_t)i].group = 0;
        for (int i = 0; i < (int)layer.shapes.size(); i++)
            if (groups.count(layer.shapes[(size_t)i].group)) layer.shapes[(size_t)i].group = 0;
        for (int i = 0; i < (int)layer.texts.size(); i++)
            if (groups.count(layer.texts[(size_t)i].group)) layer.texts[(size_t)i].group = 0;
    }
    InkNormalizeSelection();
    MarkNotesDirty();
    InvalidateAll();
}

void DocView::SetPdfHighlighter() {
    if (!CanAnnotate()) return;
    if (!inkMode_) SetInkMode(true);
    inkTool_ = INK_TOOL_HIGHLIGHT;
    inkSelection_.clear();
    inkLassoing_ = false;
    inkLasso_.clear();
    inkMoreOpen_ = false;
    InvalidateAll();
}

void DocView::InkPushUndo() {
    auto& layer = Ink();
    InkSnapshot snap;
    snap.strokes = layer.strokes;
    snap.shapes = layer.shapes;
    snap.texts = layer.texts;
    snap.textHighlights = textHighlights_;
    layer.undo.push_back(std::move(snap));
    if (layer.undo.size() > 60) layer.undo.erase(layer.undo.begin());
    layer.redo.clear();
    MarkNotesDirty();
}

void DocView::InkUndo() {
    auto& layer = Ink();
    if (layer.undo.empty()) return;
    InkSnapshot current;
    current.strokes = layer.strokes;
    current.shapes = layer.shapes;
    current.texts = layer.texts;
    current.textHighlights = textHighlights_;
    layer.redo.push_back(std::move(current));
    if (layer.redo.size() > 60) layer.redo.erase(layer.redo.begin());
    InkSnapshot snap = std::move(layer.undo.back());
    layer.undo.pop_back();
    layer.strokes = std::move(snap.strokes);
    layer.shapes = std::move(snap.shapes);
    layer.texts = std::move(snap.texts);
    textHighlights_ = std::move(snap.textHighlights);
    inkSelection_.clear();
    MarkNotesDirty();
    InvalidateAll();
}

void DocView::InkRedo() {
    auto& layer = Ink();
    if (layer.redo.empty()) return;
    InkSnapshot current;
    current.strokes = layer.strokes;
    current.shapes = layer.shapes;
    current.texts = layer.texts;
    layer.undo.push_back(std::move(current));
    if (layer.undo.size() > 60) layer.undo.erase(layer.undo.begin());
    InkSnapshot snap = std::move(layer.redo.back());
    layer.redo.pop_back();
    layer.strokes = std::move(snap.strokes);
    layer.shapes = std::move(snap.shapes);
    layer.texts = std::move(snap.texts);
    textHighlights_ = std::move(snap.textHighlights);
    inkSelection_.clear();
    MarkNotesDirty();
    InvalidateAll();
}

void DocView::InkClear() {
    auto& layer = Ink();
    if (layer.strokes.empty() && layer.shapes.empty() && layer.texts.empty() &&
        inkCurrent_.points.empty() && !inkShapeDrawing_) return;
    InkPushUndo();
    layer.strokes.clear();
    layer.shapes.clear();
    layer.texts.clear();
    textHighlights_.clear();
    inkSelection_.clear();
    inkCurrent_.points.clear();
    inkCurrentShape_ = InkShape{};
    inkShapeDrawing_ = false;
    if (inkEdit_) InkCancelText();
    InvalidateAll();
}

void DocView::InkScaleForZoom(float factor) {
    if (std::abs(factor - 1.0f) < 0.001f) return;
    auto scalePoint = [factor](InkPoint& p) { p.x *= factor; p.y *= factor; };
    auto scaleStrokes = [&](std::vector<InkStroke>& strokes) {
        for (auto& s : strokes)
            for (auto& p : s.points) scalePoint(p);
    };
    auto scaleShapes = [&](std::vector<InkShape>& shapes) {
        for (auto& s : shapes) { scalePoint(s.a); scalePoint(s.b); }
    };
    auto scaleTexts = [&](std::vector<InkText>& texts) {
        for (auto& t : texts) scalePoint(t.at);
    };
    for (auto& entry : inkLayers_) {
        if (entry.first == 5) continue; // image annotations are normalized
        scaleStrokes(entry.second.strokes);
        scaleShapes(entry.second.shapes);
        scaleTexts(entry.second.texts);
        for (auto& snapshot : entry.second.undo) {
            scaleStrokes(snapshot.strokes);
            scaleShapes(snapshot.shapes);
            scaleTexts(snapshot.texts);
        }
    }
    if (!InkImageSpace()) {
        for (auto& p : inkCurrent_.points) scalePoint(p);
        scalePoint(inkCurrentShape_.a);
        scalePoint(inkCurrentShape_.b);
        scalePoint(inkTextAnchor_);
    }
}

Color DocView::CurrentInkColor() const {
    if (inkColors_.empty()) return Color(0, 0, 0, 1);
    int i = std::clamp(inkColor_, 0, (int)inkColors_.size() - 1);
    return inkColors_[(size_t)i];
}

void DocView::SelectInkColor(int index) {
    if (inkColors_.empty()) return;
    inkColor_ = std::clamp(index, 0, (int)inkColors_.size() - 1);
    InvalidateAll();
}

void DocView::InkDrawStroke(Canvas& c, const InkStroke& stroke) {
    if (stroke.points.empty()) return;
    Color color = stroke.color;
    float width = std::max(1.0f, stroke.width);
    if (stroke.highlighter) {
        color.a = std::clamp(0.38f * stroke.color.a, 0.0f, 1.0f);
        width = std::max(8.0f, width);
    } else {
        color.a = std::clamp(stroke.color.a, 0.0f, 1.0f);
    }
    std::vector<D2D1_POINT_2F> points;
    points.reserve(stroke.points.size());
    for (auto& point : stroke.points) {
        float x = 0, y = 0;
        InkContentToScreen(point.x, point.y, x, y);
        points.push_back(D2D1::Point2F(x, y));
    }
    c.DrawStrokePath(points.data(), points.size(), color, width);
}

float DocView::InkTextWidth(const std::wstring& text, float size) const {
    float w = 0;
    for (wchar_t ch : text) w += (ch > 0x7F ? 1.0f : 0.56f) * size;
    return std::max(size, w);
}

void DocView::InkDrawTextItem(Canvas& c, const InkText& text) {
    if (text.text.empty()) return;
    float sx = 0, sy = 0;
    InkContentToScreen(text.at.x, text.at.y, sx, sy);
    Color color = text.color;
    color.a = std::clamp(text.color.a, 0.0f, 1.0f);
    float fs = std::max(8.0f, text.size);
    float w = InkTextWidth(text.text, fs) + fs * 0.5f;
    bool rotated = std::abs(text.rotation) > 0.0001f;
    if (rotated)
        c.target()->SetTransform(D2D1::Matrix3x2F::Rotation(
            text.rotation * 57.2957795f, D2D1::Point2F(sx, sy)));
    c.DrawText(text.text, sx, sy, w, fs * 1.7f, fs, color, false, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    if (rotated) c.ResetTransform();
}

void DocView::InkDrawShape(Canvas& c, const InkShape& shape) {
    float ax = 0, ay = 0, bx = 0, by = 0;
    InkContentToScreen(shape.a.x, shape.a.y, ax, ay);
    InkContentToScreen(shape.b.x, shape.b.y, bx, by);
    Color color = shape.color;
    color.a = std::clamp(shape.color.a, 0.0f, 1.0f);
    float width = std::max(1.0f, shape.width);
    bool rotated = std::abs(shape.rotation) > 0.0001f;
    if (rotated)
        c.target()->SetTransform(D2D1::Matrix3x2F::Rotation(
            shape.rotation * 57.2957795f, D2D1::Point2F((ax + bx) / 2, (ay + by) / 2)));
    if (shape.kind == INK_SHAPE_LINE) {
        c.DrawLine(ax, ay, bx, by, color, width);
    } else if (shape.kind == INK_SHAPE_ARROW) {
        c.DrawLine(ax, ay, bx, by, color, width);
        float len = std::max(8.0f, width * 4.0f);
        float ang = std::atan2(by - ay, bx - ax);
        c.DrawLine(bx, by, bx - len * std::cos(ang - 0.45f), by - len * std::sin(ang - 0.45f), color, width);
        c.DrawLine(bx, by, bx - len * std::cos(ang + 0.45f), by - len * std::sin(ang + 0.45f), color, width);
    } else if (shape.kind == INK_SHAPE_RECT) {
        float x = std::min(ax, bx), y = std::min(ay, by);
        c.StrokeRect(x, y, std::abs(bx - ax), std::abs(by - ay), color, width);
    } else if (shape.kind == INK_SHAPE_ELLIPSE) {
        float cx = (ax + bx) / 2, cy = (ay + by) / 2;
        c.DrawEllipse(cx, cy, std::abs(bx - ax) / 2, std::abs(by - ay) / 2, color, width);
    }
    if (rotated) c.ResetTransform();
}

void DocView::PaintInk(Canvas& c, bool exportOnly) {
    if (!inkMode_ || !CanAnnotate()) {
        if (!exportOnly) return;
    }
    auto& layer = Ink();
    if (layer.strokes.empty() && layer.shapes.empty() && layer.texts.empty() &&
        inkCurrent_.points.empty() && !inkShapeDrawing_ && inkSelection_.empty() &&
        !inkLassoing_) return;
    c.PushClip(0, 0, c.widthDip(), c.heightDip());
    bool imageClip = false;
    if (InkImageSpace()) {
        float x = 0, y = 0, w = 0, h = 0;
        if (ImageDisplayRect(x, y, w, h)) {
            c.PushClip(x, y, w, h);
            imageClip = true;
        }
    }
    for (auto& stroke : layer.strokes) InkDrawStroke(c, stroke);
    for (auto& shape : layer.shapes) InkDrawShape(c, shape);
    for (auto& text : layer.texts) InkDrawTextItem(c, text);
    if (inkDrawing_) InkDrawStroke(c, inkCurrent_);
    if (inkShapeDrawing_) InkDrawShape(c, inkCurrentShape_);
    if (imageClip) c.PopClip();
    if (!exportOnly && inkTool_ == INK_TOOL_SELECT) InkPaintSelection(c);
    if (!exportOnly && inkLassoing_) InkPaintLasso(c);
    c.PopClip();
}

void DocView::PaintInkToolbar(Canvas& c) {
    if (!inkMode_ || !CanAnnotate()) return;
    c.SetTranslateUi(true);
    auto& p = c.palette();
    float pad = 8, gap = 6, barH = 40;
    const wchar_t* toolLabels[] = { L"笔", L"擦", L"线", L"箭", L"框", L"圆", L"字" };
    int mask = (settings_ ? settings_->inkToolsVisible : Settings::DefaultInkTools) & 0x7F;
    int visibleCount = 0, hiddenCount = 0;
    for (int i = 0; i < 7; i++) {
        if (i == 1) continue; // eraser is pinned and never hideable
        if (mask & (1 << i)) visibleCount++;
        else hiddenCount++;
    }
    if (hiddenCount == 0) inkMoreOpen_ = false;
    float toolArea = 0.0f;
    toolArea += 28.0f + gap; // select
    toolArea += 28.0f + gap; // eraser
    toolArea += 28.0f + gap; // highlighter
    toolArea += visibleCount * 28.0f;
    if (visibleCount > 1) toolArea += (visibleCount - 1) * 3.0f;
    if (hiddenCount > 0) toolArea += (visibleCount > 0 ? 3.0f : 0.0f) + 28.0f;
    float colorArea = 0.0f;
    int colorCount = (int)InkPalette().size();
    for (int i = 0; i < colorCount; i++) colorArea += 22.0f;
    if (colorCount > 1) colorArea += (colorCount - 1) * 2.0f;
    float paletteArea = 28.0f + gap;
    auto toolbarWidth = [&](float colors, float paletteBtn) {
        return pad * 2 + toolArea + gap + colors + gap + paletteBtn + gap +
               (24 + gap + 34 + gap + 24 + gap) + 40 + gap + 44 + gap + 44;
    };
    float barW = toolbarWidth(colorArea, paletteArea);
    bool compact = barW > c.widthDip() - 16;
    if (compact) {
        colorArea = colorCount * 10.0f;
        paletteArea = 24.0f;
        barW = toolbarWidth(colorArea, paletteArea);
    }
    float barX = std::max(8.0f, (c.widthDip() - barW) / 2);
    float barY = 12;
    inkBarRect_ = { barX, barY, barW, barH };
    inkMorePanel_ = DRect{};
    inkButtons_.clear();

    Color barBg = p.surface;
    barBg.a = 0.97f;
    c.FillRound(barX + 1, barY + 3, barW, barH, 10, HexColor(L"#000000", 0.12f));
    c.FillRound(barX, barY, barW, barH, 10, barBg);
    c.StrokeRound(barX, barY, barW, barH, 10, p.border);

    float x = barX + pad;
    auto button = [&](int action, float w, const std::wstring& label, bool active, bool enabled) {
        DRect r{ x, barY + 5, w, barH - 10 };
        inkButtons_.push_back({ r, action });
        if (active) c.FillRound(r.x, r.y, r.w, r.h, 6, p.accentSoft);
        else if (inkHover_ == action && enabled) c.FillRound(r.x, r.y, r.w, r.h, 6, p.hover);
        Color fg = !enabled ? p.muted : (active ? p.accent : p.fg);
        c.DrawText(label, r.x, r.y, r.w, r.h, 11.5f, fg, active, false,
                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        x += w + gap;
    };

    button(INK_SELECT, 28, L"", inkTool_ == INK_TOOL_SELECT, true);
    {
        DRect r = inkButtons_.back().rect;
        Color fg = inkTool_ == INK_TOOL_SELECT ? p.accent : p.fg;
        D2D1_POINT_2F pts[] = {
            { r.x + 7, r.y + 6 }, { r.x + 7, r.y + 24 }, { r.x + 12, r.y + 19 },
            { r.x + 16, r.y + 27 }, { r.x + 19, r.y + 25 }, { r.x + 15, r.y + 17 },
            { r.x + 22, r.y + 17 }
        };
        c.FillPolygon(pts, 7, fg);
    }
    button(INK_ERASER, 28, L"", inkTool_ == INK_TOOL_ERASER, true);
    {
        DRect r = inkButtons_.back().rect;
        Color body = inkTool_ == INK_TOOL_ERASER ? p.accent : p.fg;
        D2D1_POINT_2F pts[] = {
            { r.x + 8, r.y + 21 }, { r.x + 15, r.y + 7 },
            { r.x + 26, r.y + 14 }, { r.x + 19, r.y + 28 }
        };
        c.FillPolygon(pts, 4, body);
        c.DrawLine(r.x + 14, r.y + 24, r.x + 22, r.y + 28, p.surface, 2.0f);
    }
    button(INK_HIGHLIGHT, 28, L"荧", inkTool_ == INK_TOOL_HIGHLIGHT, true);

    DRect moreButton{};
    bool firstTool = true;
    for (int i = 0; i < 7; i++) {
        if (i == 1 || !(mask & (1 << i))) continue;
        if (!firstTool) x += 3;
        firstTool = false;
        int action = INK_PEN + i;
        DRect r{ x, barY + 5, 28, barH - 10 };
        inkButtons_.push_back({ r, action });
        bool active = inkTool_ == i;
        if (active) c.FillRound(r.x, r.y, r.w, r.h, 6, p.accentSoft);
        else if (inkHover_ == action) c.FillRound(r.x, r.y, r.w, r.h, 6, p.hover);
        c.DrawText(toolLabels[i], r.x, r.y, r.w, r.h, 11.5f, active ? p.accent : p.fg,
                   active, false, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        x += 28;
    }
    if (hiddenCount > 0) {
        if (!firstTool) x += 3;
        DRect r{ x, barY + 5, 28, barH - 10 };
        inkButtons_.push_back({ r, INK_MORE });
        moreButton = r;
        bool active = inkMoreOpen_ || !(mask & (1 << inkTool_));
        if (active) c.FillRound(r.x, r.y, r.w, r.h, 6, p.accentSoft);
        else if (inkHover_ == INK_MORE) c.FillRound(r.x, r.y, r.w, r.h, 6, p.hover);
        c.DrawText(L"⋯", r.x, r.y, r.w, r.h, 15, active ? p.accent : p.fg,
                   false, false, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        x += 28;
    }
    x += gap;

    for (int i = 0; i < (int)InkPalette().size(); i++) {
        float cw = compact ? 10.0f : 22.0f;
        float cg = compact ? 0.0f : 2.0f;
        float ring = compact ? 5.0f : 9.0f;
        float dot = compact ? 4.0f : 7.0f;
        DRect r{ x, barY + 5, cw, barH - 10 };
        inkButtons_.push_back({ r, INK_COLOR + i });
        float cx = x + cw / 2, cy = barY + barH / 2;
        if (i == inkColor_) c.FillCircle(cx, cy, ring, p.accent);
        else if (inkHover_ == INK_COLOR + i) c.FillCircle(cx, cy, ring, p.hover);
        if (i == 5) {
            c.FillCircle(cx, cy, dot - 1, p.muted);
            c.FillCircle(cx, cy, dot, InkPalette()[i]);
        } else {
            c.FillCircle(cx, cy, dot, InkPalette()[i]);
        }
        x += cw;
        if (i + 1 < (int)InkPalette().size()) x += cg;
    }
    x += gap;
    {
        float pw = compact ? 24.0f : 28.0f;
        DRect r{ x, barY + 5, pw, barH - 10 };
        inkButtons_.push_back({ r, INK_PALETTE });
        if (inkHover_ == INK_PALETTE) c.FillRound(r.x, r.y, r.w, r.h, 5, p.hover);
        c.DrawText(L"调", r.x, r.y, r.w, r.h, 11.5f, p.fg, false, false,
                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        x += pw + gap;
    }

    auto widthButton = [&](int action, float w, const std::wstring& label) {
        DRect r{ x, barY + 5, w, barH - 10 };
        inkButtons_.push_back({ r, action });
        if (inkHover_ == action) c.FillRound(r.x, r.y, r.w, r.h, 5, p.hover);
        c.DrawText(label, r.x, r.y, r.w, r.h, 13, p.fg, false, false,
                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        x += w + gap;
    };
    widthButton(INK_WIDTH_DOWN, 24, L"−");
    {
        DRect r{ x, barY + 5, 34, barH - 10 };
        inkButtons_.push_back({ r, 0 });
        c.DrawLine(x + 7, barY + barH / 2, x + 27, barY + barH / 2,
                   InkPalette()[(size_t)inkColor_], Clampf(inkWidth_, 1.0f, 10.0f));
        x += 34 + gap;
    }
    widthButton(INK_WIDTH_UP, 24, L"+");

    button(INK_UNDO, 40, L"撤销", false, !Ink().undo.empty());
    bool hasContent = !Ink().strokes.empty() || !Ink().shapes.empty() || !Ink().texts.empty();
    button(INK_CLEAR, 44, L"清空", false, hasContent);
    button(INK_DONE, 44, L"完成", true, true);

    inkSelectionBarRect_ = DRect{};
    if (inkTool_ == INK_TOOL_SELECT && (!inkSelection_.empty() || inkClipboardValid_)) {
        float bw = 6 * 48.0f + 16.0f, bh = 34.0f;
        float bx = std::max(8.0f, (c.widthDip() - bw) / 2);
        float by = barY + barH + 6;
        inkSelectionBarRect_ = { bx, by, bw, bh };
        c.FillRound(bx + 1, by + 2, bw, bh, 8, HexColor(L"#000000", 0.12f));
        c.FillRound(bx, by, bw, bh, 8, barBg);
        c.StrokeRound(bx, by, bw, bh, 8, p.border);
        struct SelAction { int action; const wchar_t* label; bool enabled; };
        bool hasGroup = false;
        for (auto& ref : inkSelection_) if (InkGroupOf(ref) != 0) { hasGroup = true; break; }
        SelAction actions[] = {
            { INK_COPY, L"复制", !inkSelection_.empty() },
            { INK_PASTE, L"粘贴", inkClipboardValid_ },
            { INK_GROUP, L"组合", inkSelection_.size() >= 2 },
            { INK_UNGROUP, L"拆组", hasGroup },
            { INK_ROTATE, L"旋转", !inkSelection_.empty() },
            { INK_DELETE, L"删除", !inkSelection_.empty() }
        };
        for (int i = 0; i < 6; i++) {
            DRect r{ bx + 8 + i * 48.0f, by + 4, 44, bh - 8 };
            if (actions[i].enabled) inkButtons_.push_back({ r, actions[i].action });
            bool active = inkHover_ == actions[i].action && actions[i].enabled;
            if (active) c.FillRound(r.x, r.y, r.w, r.h, 5, p.hover);
            Color fg = actions[i].enabled ? p.fg : p.muted;
            if (!actions[i].enabled) fg.a = 0.45f;
            c.DrawText(actions[i].label, r.x, r.y, r.w, r.h, 11.0f,
                       fg, false, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        }
    }

    if (inkMoreOpen_ && hiddenCount > 0) {
        float itemW = 34, panelW = hiddenCount * itemW + 8, panelH = 38;
        float px = 8.0f;
        if (moreButton.w > 0) {
            // Anchor under the “⋯” button, then clamp back into the toolbar
            // width; if there is no room, fall back to the far left.
            px = moreButton.x + (moreButton.w - panelW) / 2;
            px = std::min(px, c.widthDip() - panelW - 8);
            px = std::max(8.0f, px);
        }
        float py = barY + barH + 6;
        inkMorePanel_ = { px, py, panelW, panelH };
        c.FillRound(px + 1, py + 2, panelW, panelH, 8, HexColor(L"#000000", 0.12f));
        c.FillRound(px, py, panelW, panelH, 8, barBg);
        c.StrokeRound(px, py, panelW, panelH, 8, p.border);
        int j = 0;
        for (int i = 0; i < 7; i++) {
            if (i == 1 || (mask & (1 << i))) continue;
            DRect r{ px + 4 + j * itemW, py + 5, 28, 28 };
            inkButtons_.push_back({ r, INK_PEN + i });
            bool active = inkTool_ == i;
            if (active) c.FillRound(r.x, r.y, r.w, r.h, 6, p.accentSoft);
            else if (inkHover_ == INK_PEN + i) c.FillRound(r.x, r.y, r.w, r.h, 6, p.hover);
            c.DrawText(toolLabels[i], r.x, r.y, r.w, r.h, 11.5f, active ? p.accent : p.fg,
                       active, false, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
            j++;
        }
    }
}

bool DocView::InkHitButton(float x, float y, int& action) const {
    for (auto& b : inkButtons_) {
        if (b.rect.Contains(x, y)) {
            action = b.action;
            return true;
        }
    }
    return false;
}

void DocView::InkAction(int action) {
    if (action >= INK_COLOR) {
        inkColor_ = std::min(action - INK_COLOR, (int)InkPalette().size() - 1);
    } else if (action == INK_MORE) {
        inkMoreOpen_ = !inkMoreOpen_;
    } else if (action == INK_SELECT) {
        if (inkEdit_) InkCommitText();
        inkTool_ = INK_TOOL_SELECT;
        inkMoreOpen_ = false;
    } else if (action == INK_HIGHLIGHT) {
        if (inkEdit_) InkCommitText();
        inkTool_ = INK_TOOL_HIGHLIGHT;
        inkLassoing_ = false;
        inkLasso_.clear();
        inkMoreOpen_ = false;
    } else if (action == INK_PALETTE) {
        if (events_.openColorPicker) events_.openColorPicker(-1);
    } else if (action == INK_COPY) {
        CopyInkSelection();
    } else if (action == INK_PASTE) {
        PasteInkSelection();
    } else if (action == INK_GROUP) {
        GroupInkSelection(true);
    } else if (action == INK_UNGROUP) {
        GroupInkSelection(false);
    } else if (action == INK_ROTATE) {
        if (inkSelection_.empty()) return;
        float x0, y0, x1, y1;
        InkSelectionBounds(x0, y0, x1, y1);
        if (x1 > x0) {
            InkPushUndo();
            auto& layer = Ink();
            inkDragBase_.strokes = layer.strokes;
            inkDragBase_.shapes = layer.shapes;
            inkDragBase_.texts = layer.texts;
            inkDragPivot_ = { (x0 + x1) / 2, (y0 + y1) / 2 };
            InkApplySelectionTransform(3, 0, 0, 1.0f, 15.0f * 3.14159265f / 180.0f);
        }
    } else if (action == INK_DELETE) {
        DeleteInkSelection();
    } else if (action == INK_SAVE) {
        if (!CanSaveNotes() || !HasUnsavedNotes()) return;
        std::wstring err;
        if (!SaveNotes(&err))
            MessageBoxW(hwnd_, TranslateUi(err.empty() ? L"保存笔记失败" : err).c_str(), L"LitheMD", MB_OK | MB_ICONWARNING);
    } else if (action >= INK_PEN && action <= INK_TEXT) {
        if (inkEdit_) InkCommitText();
        inkTool_ = action - INK_PEN;
        inkSelection_.clear();
        inkLassoing_ = false;
        inkLasso_.clear();
        inkMoreOpen_ = false;
    } else {
        switch (action) {
        case INK_WIDTH_DOWN: inkWidth_ = std::max(1.0f, inkWidth_ - 1.0f); break;
        case INK_WIDTH_UP: inkWidth_ = std::min(12.0f, inkWidth_ + 1.0f); break;
        case INK_UNDO:
            if (inkEdit_) InkCommitText();
            InkUndo();
            break;
        case INK_CLEAR:
            if (inkEdit_) InkCancelText();
            InkClear();
            break;
        case INK_DONE:
            if (inkEdit_) InkCommitText();
            SetInkMode(false);
            return;
        default: break;
        }
    }
    InvalidateAll();
}

void DocView::InkBegin(float x, float y) {
    if (!CanAnnotate()) return;
    if (inkEdit_) InkCommitText();
    if (inkTool_ == INK_TOOL_ERASER) {
        inkErasing_ = true;
        inkErasePushed_ = false;
        SetCapture(hwnd_);
        InkEraseAt(x, y);
        return;
    }
    float cx = 0, cy = 0;
    if (!InkScreenToContent(x, y, cx, cy)) return;
    if (inkTool_ == INK_TOOL_TEXT) {
        InkOpenTextEditor(x, y);
        return;
    }
    if (inkTool_ == INK_TOOL_HIGHLIGHT) {
        // Text-aware highlight for rendered text; freehand fallback elsewhere.
        if (CanHighlightPdf()) return;
        if (BeginTextHighlight(x, y)) return;
        InkPushUndo();
        inkCurrent_ = InkStroke{};
        inkCurrent_.color = InkPalette()[(size_t)inkColor_];
        inkCurrent_.color.a = 1.0f;
        inkCurrent_.width = std::max(12.0f, inkWidth_ * 4.0f);
        inkCurrent_.highlighter = true;
        inkCurrent_.points.push_back({ cx, cy });
        inkDrawing_ = true;
        SetCapture(hwnd_);
        InvalidateAll();
        return;
    }
    if (inkTool_ == INK_TOOL_PEN) {
        InkPushUndo();
        inkCurrent_ = InkStroke{};
        inkCurrent_.color = InkPalette()[(size_t)inkColor_];
        inkCurrent_.width = inkWidth_;
        inkCurrent_.points.push_back({ cx, cy });
        inkDrawing_ = true;
        SetCapture(hwnd_);
        InvalidateAll();
        return;
    }
    inkCurrentShape_ = InkShape{};
    inkCurrentShape_.kind = inkTool_ == INK_TOOL_LINE ? INK_SHAPE_LINE
                         : inkTool_ == INK_TOOL_ARROW ? INK_SHAPE_ARROW
                         : inkTool_ == INK_TOOL_RECT ? INK_SHAPE_RECT : INK_SHAPE_ELLIPSE;
    inkCurrentShape_.a = { cx, cy };
    inkCurrentShape_.b = { cx, cy };
    inkCurrentShape_.color = InkPalette()[(size_t)inkColor_];
    inkCurrentShape_.width = inkWidth_;
    inkShapeDrawing_ = true;
    SetCapture(hwnd_);
    InvalidateAll();
}

void DocView::InkMove(float x, float y) {
    if (textHighlighting_) {
        UpdateTextHighlight(x, y);
        return;
    }
    if (inkDrawing_) {
        float cx = 0, cy = 0;
        if (!InkScreenToContent(x, y, cx, cy)) return;
        if (!inkCurrent_.points.empty()) {
            auto& last = inkCurrent_.points.back();
            float lx = 0, ly = 0;
            InkContentToScreen(last.x, last.y, lx, ly);
            float dx = x - lx, dy = y - ly;
            if (dx * dx + dy * dy < 1.5f * 1.5f) return;
        }
        inkCurrent_.points.push_back({ cx, cy });
        InvalidateAll();
    } else if (inkErasing_) {
        InkEraseAt(x, y);
    } else if (inkShapeDrawing_) {
        float cx = 0, cy = 0;
        if (!InkScreenToContent(x, y, cx, cy)) return;
        inkCurrentShape_.b = { cx, cy };
        InvalidateAll();
    }
}

void DocView::InkEraseAt(float x, float y) {
    auto& layer = Ink();
    if (layer.strokes.empty() && layer.shapes.empty() && layer.texts.empty()) return;
    float radius = std::max(6.0f, inkWidth_ * 2.0f);
    std::set<int> hitGroups;
    std::set<std::pair<int, int>> hitItems;
    auto hit = [&](int kind, int index, int group) {
        if (group) hitGroups.insert(group);
        else hitItems.insert({ kind, index });
    };

    for (int i = 0; i < (int)layer.strokes.size(); i++) {
        auto& s = layer.strokes[(size_t)i];
        bool touched = false;
        if (s.points.size() == 1) {
            float sx = 0, sy = 0;
            InkContentToScreen(s.points[0].x, s.points[0].y, sx, sy);
            touched = (sx - x) * (sx - x) + (sy - y) * (sy - y) <= radius * radius;
        } else if (s.points.size() > 1) {
            float ax = 0, ay = 0;
            InkContentToScreen(s.points[0].x, s.points[0].y, ax, ay);
            for (size_t k = 1; k < s.points.size(); k++) {
                float bx = 0, by = 0;
                InkContentToScreen(s.points[k].x, s.points[k].y, bx, by);
                if (DistanceToSegment(x, y, ax, ay, bx, by) <= radius) { touched = true; break; }
                ax = bx;
                ay = by;
            }
        }
        if (touched) hit(0, i, s.group);
    }

    for (int i = 0; i < (int)layer.shapes.size(); i++) {
        auto& s = layer.shapes[(size_t)i];
        float ax = 0, ay = 0, bx = 0, by = 0;
        InkContentToScreen(s.a.x, s.a.y, ax, ay);
        InkContentToScreen(s.b.x, s.b.y, bx, by);
        bool touched = false;
        if (s.kind == INK_SHAPE_LINE || s.kind == INK_SHAPE_ARROW) {
            touched = DistanceToSegment(x, y, ax, ay, bx, by) <= radius;
        } else if (s.kind == INK_SHAPE_RECT) {
            float x0 = std::min(ax, bx), y0 = std::min(ay, by);
            float x1 = std::max(ax, bx), y1 = std::max(ay, by);
            touched = DistanceToSegment(x, y, x0, y0, x1, y0) <= radius ||
                      DistanceToSegment(x, y, x1, y0, x1, y1) <= radius ||
                      DistanceToSegment(x, y, x1, y1, x0, y1) <= radius ||
                      DistanceToSegment(x, y, x0, y1, x0, y0) <= radius;
        } else {
            float cx = (ax + bx) / 2, cy = (ay + by) / 2;
            float rx = std::abs(bx - ax) / 2, ry = std::abs(by - ay) / 2;
            for (int k = 0; k < 48 && !touched; k++) {
                float a0 = (float)k / 48 * 6.2831853f;
                float a1 = (float)(k + 1) / 48 * 6.2831853f;
                float p0x = cx + rx * std::cos(a0), p0y = cy + ry * std::sin(a0);
                float p1x = cx + rx * std::cos(a1), p1y = cy + ry * std::sin(a1);
                touched = DistanceToSegment(x, y, p0x, p0y, p1x, p1y) <= radius;
            }
        }
        if (touched) hit(1, i, s.group);
    }

    for (int i = 0; i < (int)layer.texts.size(); i++) {
        auto& t = layer.texts[(size_t)i];
        float sx = 0, sy = 0;
        InkContentToScreen(t.at.x, t.at.y, sx, sy);
        float w = InkTextWidth(t.text, t.size) + 8;
        float h = t.size * 1.5f;
        if (x >= sx - radius && x <= sx + w + radius &&
            y >= sy - radius && y <= sy + h + radius) hit(2, i, t.group);
    }

    if (hitGroups.empty() && hitItems.empty()) return;
    if (!inkErasePushed_) {
        InkPushUndo();
        inkErasePushed_ = true;
    }
    auto remove = [&](int kind, int index, int group) {
        return (group && hitGroups.count(group)) || hitItems.count({ kind, index });
    };
    std::vector<InkStroke> keepStrokes;
    keepStrokes.reserve(layer.strokes.size());
    for (size_t i = 0; i < layer.strokes.size(); i++)
        if (!remove(0, (int)i, layer.strokes[i].group)) keepStrokes.push_back(layer.strokes[i]);
    std::vector<InkShape> keepShapes;
    keepShapes.reserve(layer.shapes.size());
    for (size_t i = 0; i < layer.shapes.size(); i++)
        if (!remove(1, (int)i, layer.shapes[i].group)) keepShapes.push_back(layer.shapes[i]);
    std::vector<InkText> keepTexts;
    keepTexts.reserve(layer.texts.size());
    for (size_t i = 0; i < layer.texts.size(); i++)
        if (!remove(2, (int)i, layer.texts[i].group)) keepTexts.push_back(layer.texts[i]);
    layer.strokes = std::move(keepStrokes);
    layer.shapes = std::move(keepShapes);
    layer.texts = std::move(keepTexts);
    inkSelection_.clear();
    InvalidateAll();
}

void DocView::InkEnd() {
    if (textHighlighting_) {
        FinishTextHighlight();
        return;
    }
    if (inkDrawing_) {
        if (!inkCurrent_.points.empty()) Ink().strokes.push_back(std::move(inkCurrent_));
        inkCurrent_ = InkStroke{};
        inkDrawing_ = false;
    }
    if (inkShapeDrawing_) {
        float ax = 0, ay = 0, bx = 0, by = 0;
        InkContentToScreen(inkCurrentShape_.a.x, inkCurrentShape_.a.y, ax, ay);
        InkContentToScreen(inkCurrentShape_.b.x, inkCurrentShape_.b.y, bx, by);
        if ((bx - ax) * (bx - ax) + (by - ay) * (by - ay) >= 9.0f) {
            InkPushUndo();
            Ink().shapes.push_back(inkCurrentShape_);
        }
        inkCurrentShape_ = InkShape{};
        inkShapeDrawing_ = false;
    }
    inkErasing_ = false;
    inkErasePushed_ = false;
    if (GetCapture() == hwnd_) ReleaseCapture();
    InvalidateAll();
}

void DocView::InkOpenTextEditor(float x, float y) {
    if (!CanAnnotate()) return;
    float cx = 0, cy = 0;
    if (!InkScreenToContent(x, y, cx, cy)) return;
    if (inkEdit_) InkCommitText();
    inkTextAnchor_ = { cx, cy };
    inkTextSize_ = Clampf(14.0f + inkWidth_ * 2.0f, 16.0f, 34.0f);

    UINT dpi = GetDpiForWindow(hwnd_);
    if (!dpi) dpi = 96;
    float sc = dpi / 96.0f;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int w = (int)std::lround(240 * sc);
    int h = (int)std::lround(30 * sc);
    int px = (int)std::lround(x * sc);
    int py = (int)std::lround(y * sc);
    if (px + w > rc.right - 6) px = std::max(4, (int)rc.right - w - 6);
    if (py + h > rc.bottom - 6) py = std::max(4, (int)rc.bottom - h - 6);

    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE);
    inkEdit_ = CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_BORDER,
                               px, py, w, h, hwnd_, (HMENU)9001, inst, nullptr);
    if (!inkEdit_) return;

    inkEditFont_ = CreateFontW(-MulDiv((int)std::lround(inkTextSize_), (int)dpi, 96), 0, 0, 0,
                               FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (inkEditFont_) SendMessageW(inkEdit_, WM_SETFONT, (WPARAM)inkEditFont_, TRUE);
    SendMessageW(inkEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(5, 5));
    SetWindowSubclass(inkEdit_, InkEditSubclassProc, 1, 0);
    SetFocus(inkEdit_);
    InvalidateAll();
}

void DocView::InkCommitText() {
    if (!inkEdit_) return;
    wchar_t buffer[4096] = {};
    int len = (int)SendMessageW(inkEdit_, WM_GETTEXT, (WPARAM)4096, (LPARAM)buffer);
    std::wstring text(buffer, len > 0 ? (size_t)len : 0);
    InkCloseTextEditor();
    if (!text.empty()) {
        InkPushUndo();
        InkText item;
        item.at = inkTextAnchor_;
        item.text = text;
        item.color = InkPalette()[(size_t)inkColor_];
        item.size = inkTextSize_;
        Ink().texts.push_back(std::move(item));
    }
    InvalidateAll();
}

void DocView::InkCancelText() {
    if (!inkEdit_) return;
    InkCloseTextEditor();
    InvalidateAll();
}

void DocView::InkCloseTextEditor() {
    if (inkEdit_) {
        DestroyWindow(inkEdit_);
        inkEdit_ = nullptr;
    }
    if (inkEditFont_) {
        DeleteObject(inkEditFont_);
        inkEditFont_ = nullptr;
    }
    if (hwnd_ && GetFocus() != hwnd_) SetFocus(hwnd_);
}

void DocView::SetInkMode(bool on) {
    if (on && !CanAnnotate()) return;
    if (inkMode_ == on) return;
    if (!on) {
        if (inkEdit_) InkCommitText();
        if (inkDrawing_ || inkErasing_ || inkShapeDrawing_) InkEnd();
        inkDragMode_ = 0;
        inkLassoing_ = false;
        inkLasso_.clear();
        pdfHighlighting_ = false;
        inkSelection_.clear();
        if (GetCapture() == hwnd_) ReleaseCapture();
    }
    inkMode_ = on;
    inkMoreOpen_ = false;
    inkMorePanel_ = DRect{};
    inkSelectionBarRect_ = DRect{};
    inkHover_ = 0;
    copyHover_.active = false;
    ClearCopyFeedback();
    SetCursor(LoadCursorW(nullptr, on ? IDC_CROSS : IDC_ARROW));
    if (events_.statusChanged) events_.statusChanged();
    InvalidateAll();
}

void DocView::UndoInk() {
    if (!inkMode_) return;
    InkUndo();
}

void DocView::RedoInk() {
    if (!inkMode_) return;
    InkRedo();
}

