#include "overlay.h"
#include "common.h"
#include "render.h"
#include "view.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include <windowsx.h>

namespace {

constexpr wchar_t kOverlayClass[] = L"LitheMdInkOverlay";
constexpr wchar_t kToolbarClass[] = L"LitheMdInkOverlayToolbar";
constexpr wchar_t kSizePopupClass[] = L"LitheMdInkSizePopup";
constexpr wchar_t kSelBarClass[] = L"LitheMdInkSelectionBar";
constexpr UINT WM_FS_EXIT = WM_APP + 70;

enum : int {
    FS_MOUSE = 1,
    FS_SELECT,
    FS_PEN,
    FS_ERASER,
    FS_UNDO,
    FS_REDO,
    FS_CLEAR,
    FS_COLOR1 = 10,
    FS_COLOR2,
    FS_COLOR3,
    FS_PALETTE = 20,
    FS_BACK = 30,
    FS_SETTINGS,
    FS_GRIP = 40,
    FS_COLLAPSE,
};

int ToolForCommand(int cmd) {
    switch (cmd) {
    case FS_MOUSE: return FS_MOUSE;
    case FS_SELECT: return FS_SELECT;
    case FS_PEN: return FS_PEN;
    case FS_ERASER: return FS_ERASER;
    default: return 0;
    }
}

float DistToSegment(float px, float py, float ax, float ay, float bx, float by) {
    float dx = bx - ax, dy = by - ay;
    float len2 = dx * dx + dy * dy;
    float t = 0.0f;
    if (len2 > 0.0001f)
        t = std::clamp(((px - ax) * dx + (py - ay) * dy) / len2, 0.0f, 1.0f);
    float qx = ax + dx * t, qy = ay + dy * t;
    return std::hypot(px - qx, py - qy);
}

bool RectIntersects(const RECT& a, const RECT& b) {
    return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
}

DRect NormalizedRect(float x1, float y1, float x2, float y2) {
    DRect r;
    r.x = std::min(x1, x2);
    r.y = std::min(y1, y2);
    r.w = std::abs(x2 - x1);
    r.h = std::abs(y2 - y1);
    return r;
}

int ScaleForWindow(HWND hwnd) {
    UINT dpi = GetDpiForWindow(hwnd);
    if (!dpi) dpi = 96;
    return (int)std::lround(dpi / 96.0f * 100.0f);
}

} // namespace

struct FullscreenInkOverlay::Impl {
    struct Entry {
        InkStroke ink;
        ComPtr<ID2D1PathGeometry> geo;
        float minX = 0, minY = 0, maxX = 0, maxY = 0;
    };

    struct Surface {
        HWND hwnd = nullptr;
        RECT mon{};
        int w = 0, h = 0;
        HDC dc = nullptr;
        HBITMAP bmp = nullptr;
        HBITMAP oldBmp = nullptr;
        void* bits = nullptr;
        ComPtr<ID2D1DCRenderTarget> rt;
        ComPtr<ID2D1SolidColorBrush> brush;
        ComPtr<ID2D1SolidColorBrush> accent;
    };

    struct ToolbarItem {
        int cmd = 0;
        DRect rect;
    };

    HWND owner_ = nullptr;
    Settings* settings_ = nullptr;
    Callbacks cb_;
    bool visible_ = false;
    int tool_ = FS_PEN;
    int colorSlot_ = 0;
    int hotkeysEnabled_ = 1;

    std::vector<Surface> surfaces_;
    HWND toolbar_ = nullptr;
    std::unique_ptr<Canvas> toolbarCanvas_;
    std::vector<ToolbarItem> toolbarItems_;
    std::vector<float> toolbarSeps_;
    float toolbarW_ = 0, toolbarH_ = 0;
    int hoverCmd_ = 0;
    bool draggingToolbar_ = false;
    bool gripPressed_ = false;
    bool gripMoved_ = false;
    bool collapsed_ = false;
    POINT dragStart_{};
    POINT gripStart_{};
    RECT dragOrigin_{};
    RECT expandedRect_{};
    HWND sizePopup_ = nullptr;
    std::unique_ptr<Canvas> sizeCanvas_;
    std::vector<DRect> sizeRects_;
    int sizeKind_ = FS_PEN;
    int sizeHover_ = -1;
    DRect sizeClearRect_{};
    bool sizeClearHover_ = false;

    std::vector<Entry> entries_;
    std::vector<std::vector<InkStroke>> undo_;
    std::vector<std::vector<InkStroke>> redo_;
    std::vector<int> selection_;
    std::vector<InkStroke> clipboard_;
    bool clipboardValid_ = false;
    int pasteSerial_ = 0;
    int groupSeq_ = 1;
    HWND selBar_ = nullptr;
    std::unique_ptr<Canvas> selCanvas_;
    int selHover_ = -1;
    std::vector<InkStroke> selOriginal_;
    int selDragMode_ = 0;
    bool selPushed_ = false;
    POINT selDragStart_{};
    POINT selPivot_{};
    float selStartDist_ = 1.0f;
    float selStartAngle_ = 0.0f;

    InkStroke current_;
    ComPtr<ID2D1PathGeometry> currentGeo_;
    bool drawing_ = false;
    bool erasing_ = false;
    bool erasePushed_ = false;
    bool lassoing_ = false;
    POINT lassoStart_{}, lassoEnd_{};
    POINT mouse_{};

    ComPtr<ID2D1StrokeStyle> strokeStyle_;

    static HHOOK hook_;
    static Impl* hookOwner_;

    ~Impl() { DestroyWindows(); }

    float ScaleForSurface(const Surface& s) const {
        UINT dpi = s.hwnd ? GetDpiForWindow(s.hwnd) : 96;
        if (!dpi) dpi = 96;
        return dpi / 96.0f;
    }

    int ActivePreset() const {
        if (!settings_) return 0;
        if (settings_->fullscreenInkPresets.empty()) return 0;
        int slot = std::clamp(colorSlot_, 0, (int)settings_->fullscreenInkPresets.size() - 1);
        return settings_->fullscreenInkPresets[(size_t)slot];
    }

    Color ActiveColor() const {
        if (!settings_ || settings_->inkPresets.empty()) return Color(0.12f, 0.14f, 0.18f, 1.0f);
        int index = std::clamp(ActivePreset(), 0, (int)settings_->inkPresets.size() - 1);
        return settings_->inkPresets[(size_t)index];
    }

    const Palette& ThemePalette() const {
        static const std::wstring fallback = L"blue";
        const std::wstring& theme =
            (settings_ && !settings_->theme.empty()) ? settings_->theme : fallback;
        return Palette::Get(theme);
    }

    std::vector<InkStroke> Snapshot() const {
        std::vector<InkStroke> out;
        out.reserve(entries_.size());
        for (auto& e : entries_) out.push_back(e.ink);
        return out;
    }

    void PushUndo() {
        undo_.push_back(Snapshot());
        if (undo_.size() > 80) undo_.erase(undo_.begin());
        redo_.clear();
    }

    void BuildEntry(Entry& e) {
        e.geo.Reset();
        if (e.ink.points.empty()) {
            e.minX = e.minY = e.maxX = e.maxY = 0;
            return;
        }
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (auto& p : e.ink.points) {
            minX = std::min(minX, p.x);
            minY = std::min(minY, p.y);
            maxX = std::max(maxX, p.x);
            maxY = std::max(maxY, p.y);
        }
        float pad = e.ink.width * 2.0f + 4.0f;
        e.minX = minX - pad; e.minY = minY - pad;
        e.maxX = maxX + pad; e.maxY = maxY + pad;
        if (e.ink.points.size() < 2 || !Engine::Instance().D2D()) return;
        ComPtr<ID2D1PathGeometry> geo;
        if (FAILED(Engine::Instance().D2D()->CreatePathGeometry(&geo)) || !geo) return;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geo->Open(&sink)) || !sink) return;
        sink->BeginFigure(D2D1::Point2F(e.ink.points[0].x, e.ink.points[0].y),
                          D2D1_FIGURE_BEGIN_FILLED);
        if (e.ink.points.size() > 2) {
            std::vector<D2D1_POINT_2F> pts;
            pts.reserve(e.ink.points.size() - 1);
            for (size_t i = 1; i < e.ink.points.size(); i++)
                pts.push_back(D2D1::Point2F(e.ink.points[i].x, e.ink.points[i].y));
            sink->AddLines(pts.data(), (UINT32)pts.size());
        } else {
            sink->AddLine(D2D1::Point2F(e.ink.points[1].x, e.ink.points[1].y));
        }
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        e.geo = geo;
    }

    void RebuildAll() {
        for (auto& e : entries_) BuildEntry(e);
    }

    void RebuildCurrent() {
        currentGeo_.Reset();
        if (current_.points.size() < 2 || !Engine::Instance().D2D()) return;
        ComPtr<ID2D1PathGeometry> geo;
        if (FAILED(Engine::Instance().D2D()->CreatePathGeometry(&geo)) || !geo) return;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(geo->Open(&sink)) || !sink) return;
        sink->BeginFigure(D2D1::Point2F(current_.points[0].x, current_.points[0].y),
                          D2D1_FIGURE_BEGIN_FILLED);
        std::vector<D2D1_POINT_2F> pts;
        pts.reserve(current_.points.size() - 1);
        for (size_t i = 1; i < current_.points.size(); i++)
            pts.push_back(D2D1::Point2F(current_.points[i].x, current_.points[i].y));
        sink->AddLines(pts.data(), (UINT32)pts.size());
        sink->EndFigure(D2D1_FIGURE_END_OPEN);
        sink->Close();
        currentGeo_ = geo;
    }

    DRect SelectionBounds() const {
        DRect out{};
        bool first = true;
        for (int index : selection_) {
            if (index < 0 || index >= (int)entries_.size()) continue;
            auto& e = entries_[(size_t)index];
            if (first) {
                out = { e.minX, e.minY, e.maxX - e.minX, e.maxY - e.minY };
                first = false;
            } else {
                float x2 = std::max(out.x + out.w, e.maxX);
                float y2 = std::max(out.y + out.h, e.maxY);
                out.x = std::min(out.x, e.minX);
                out.y = std::min(out.y, e.minY);
                out.w = x2 - out.x;
                out.h = y2 - out.y;
            }
        }
        return out;
    }

    void NormalizeSelection() {
        std::vector<int> valid;
        std::set<int> groups;
        for (int index : selection_) {
            if (index < 0 || index >= (int)entries_.size()) continue;
            if (std::find(valid.begin(), valid.end(), index) == valid.end())
                valid.push_back(index);
            if (entries_[(size_t)index].ink.group != 0)
                groups.insert(entries_[(size_t)index].ink.group);
        }
        if (!groups.empty()) {
            for (int i = 0; i < (int)entries_.size(); i++) {
                int g = entries_[(size_t)i].ink.group;
                if (g && groups.count(g) &&
                    std::find(valid.begin(), valid.end(), i) == valid.end())
                    valid.push_back(i);
            }
        }
        std::sort(valid.begin(), valid.end());
        selection_ = std::move(valid);
        UpdateSelectionBarWindow();
    }

    void SelectInRect(const DRect& r) {
        selection_.clear();
        for (int i = 0; i < (int)entries_.size(); i++) {
            auto& e = entries_[(size_t)i];
            if (e.maxX < r.x || e.minX > r.x + r.w || e.maxY < r.y || e.minY > r.y + r.h)
                continue;
            selection_.push_back(i);
        }
        NormalizeSelection();
    }

    void DeleteSelection() {
        NormalizeSelection();
        if (selection_.empty()) return;
        PushUndo();
        std::vector<int> sorted = selection_;
        std::sort(sorted.begin(), sorted.end(), std::greater<int>());
        for (int idx : sorted)
            if (idx >= 0 && idx < (int)entries_.size())
                entries_.erase(entries_.begin() + idx);
        selection_.clear();
        UpdateSelectionBarWindow();
        InvalidateAll();
    }

    void CancelCurrentStroke() {
        if (!drawing_) return;
        drawing_ = false;
        current_.points.clear();
        currentGeo_.Reset();
        if (GetCapture()) ReleaseCapture();
    }

    void Undo() {
        CancelCurrentStroke();
        if (undo_.empty()) return;
        redo_.push_back(Snapshot());
        auto state = std::move(undo_.back());
        undo_.pop_back();
        entries_.clear();
        entries_.resize(state.size());
        for (size_t i = 0; i < state.size(); i++) {
            entries_[i].ink = std::move(state[i]);
            BuildEntry(entries_[i]);
        }
        selection_.clear();
        UpdateSelectionBarWindow();
        InvalidateAll();
    }

    void Redo() {
        CancelCurrentStroke();
        if (redo_.empty()) return;
        undo_.push_back(Snapshot());
        auto state = std::move(redo_.back());
        redo_.pop_back();
        entries_.clear();
        entries_.resize(state.size());
        for (size_t i = 0; i < state.size(); i++) {
            entries_[i].ink = std::move(state[i]);
            BuildEntry(entries_[i]);
        }
        selection_.clear();
        UpdateSelectionBarWindow();
        InvalidateAll();
    }

    void ClearAll() {
        if (entries_.empty() && !drawing_) return;
        CancelCurrentStroke();
        PushUndo();
        entries_.clear();
        selection_.clear();
        current_.points.clear();
        currentGeo_.Reset();
        drawing_ = false;
        UpdateSelectionBarWindow();
        InvalidateAll();
    }

    int SurfaceScaleAt(POINT pt) const {
        for (auto& s : surfaces_) {
            if (PtInRect(&s.mon, pt)) return ScaleForWindow(s.hwnd);
        }
        return 100;
    }

    void EraseAt(POINT pt, int scale100) {
        float radius = (float)std::max(1, settings_ ? settings_->fullscreenEraserSize : 18) *
                       (float)scale100 / 100.0f;
        bool hit = false;
        for (int i = (int)entries_.size() - 1; i >= 0; i--) {
            auto& e = entries_[(size_t)i];
            bool touched = false;
            auto& pts = e.ink.points;
            if (pts.size() == 1) {
                touched = std::hypot(pts[0].x - pt.x, pts[0].y - pt.y) <= radius + e.ink.width;
            } else {
                for (size_t k = 1; k < pts.size(); k++) {
                    if (DistToSegment((float)pt.x, (float)pt.y, pts[k - 1].x, pts[k - 1].y,
                                      pts[k].x, pts[k].y) <= radius + e.ink.width) {
                        touched = true;
                        break;
                    }
                }
            }
            if (!touched) continue;
            if (!erasePushed_) {
                PushUndo();
                erasePushed_ = true;
            }
            entries_.erase(entries_.begin() + i);
            hit = true;
        }
        if (hit) {
            selection_.clear();
            UpdateSelectionBarWindow();
            InvalidateAll();
        }
    }

    bool PointInSelection(POINT pt) const {
        if (selection_.empty()) return false;
        DRect b = SelectionBounds();
        return pt.x >= b.x - 10 && pt.x <= b.x + b.w + 10 &&
               pt.y >= b.y - 10 && pt.y <= b.y + b.h + 10;
    }

    int HitSelectionHandle(POINT pt) const {
        if (selection_.empty()) return 0;
        DRect b = SelectionBounds();
        float cx = b.x + b.w / 2;
        auto closeTo = [&](float x, float y, float tol) {
            return std::abs((float)pt.x - x) <= tol && std::abs((float)pt.y - y) <= tol;
        };
        if (closeTo(cx, b.y - 22, 9)) return 3;
        if (closeTo(b.x, b.y, 8) || closeTo(b.x + b.w, b.y, 8) ||
            closeTo(b.x, b.y + b.h, 8) || closeTo(b.x + b.w, b.y + b.h, 8))
            return 2;
        if (pt.x >= b.x - 10 && pt.x <= b.x + b.w + 10 &&
            pt.y >= b.y - 10 && pt.y <= b.y + b.h + 10)
            return 1;
        return 0;
    }

    bool AnyGroupSelected() const {
        for (int idx : selection_)
            if (idx >= 0 && idx < (int)entries_.size() && entries_[(size_t)idx].ink.group != 0)
                return true;
        return false;
    }

    void BeginSelectionDrag(int mode, POINT pt) {
        NormalizeSelection();
        if (selection_.empty()) return;
        DRect b = SelectionBounds();
        selDragMode_ = mode;
        selPushed_ = false;
        selDragStart_ = pt;
        selPivot_ = { (LONG)std::lround(b.x + b.w / 2), (LONG)std::lround(b.y + b.h / 2) };
        selStartDist_ = std::max(1.0f, std::hypot((float)pt.x - selPivot_.x,
                                                  (float)pt.y - selPivot_.y));
        selStartAngle_ = std::atan2((float)pt.y - selPivot_.y, (float)pt.x - selPivot_.x);
        selOriginal_.clear();
        for (int idx : selection_)
            if (idx >= 0 && idx < (int)entries_.size())
                selOriginal_.push_back(entries_[(size_t)idx].ink);
    }

    void ApplySelectionDrag(POINT pt, bool started) {
        if (selDragMode_ == 0 || selection_.empty()) return;
        int dx = pt.x - selDragStart_.x, dy = pt.y - selDragStart_.y;
        if (!started && std::abs(dx) < 2 && std::abs(dy) < 2) return;
        float scale = 1.0f, angle = 0.0f;
        if (selDragMode_ == 2) {
            float d = std::hypot((float)pt.x - selPivot_.x, (float)pt.y - selPivot_.y);
            scale = std::clamp(d / selStartDist_, 0.1f, 10.0f);
        } else if (selDragMode_ == 3) {
            float a = std::atan2((float)pt.y - selPivot_.y, (float)pt.x - selPivot_.x);
            angle = a - selStartAngle_;
            while (angle > 3.14159265f) angle -= 6.2831853f;
            while (angle < -3.14159265f) angle += 6.2831853f;
        }
        for (size_t k = 0; k < selection_.size() && k < selOriginal_.size(); k++) {
            int idx = selection_[k];
            if (idx < 0 || idx >= (int)entries_.size()) continue;
            InkStroke& dst = entries_[(size_t)idx].ink;
            dst = selOriginal_[k];
            for (auto& p : dst.points) {
                if (selDragMode_ == 1) {
                    p.x += (float)dx;
                    p.y += (float)dy;
                } else if (selDragMode_ == 2) {
                    p.x = selPivot_.x + (p.x - selPivot_.x) * scale;
                    p.y = selPivot_.y + (p.y - selPivot_.y) * scale;
                } else {
                    float qx = p.x - selPivot_.x, qy = p.y - selPivot_.y;
                    p.x = selPivot_.x + qx * std::cos(angle) - qy * std::sin(angle);
                    p.y = selPivot_.y + qx * std::sin(angle) + qy * std::cos(angle);
                }
            }
            if (selDragMode_ == 2) dst.width = selOriginal_[k].width * scale;
            BuildEntry(entries_[(size_t)idx]);
        }
        UpdateSelectionBarWindow();
        InvalidateAll();
    }

    void EndSelectionDrag() {
        selDragMode_ = 0;
        selPushed_ = false;
        selOriginal_.clear();
        UpdateSelectionBarWindow();
    }

    void MoveSelectionBy(int dx, int dy) {
        NormalizeSelection();
        if (selection_.empty()) return;
        PushUndo();
        for (int idx : selection_) {
            if (idx < 0 || idx >= (int)entries_.size()) continue;
            for (auto& p : entries_[(size_t)idx].ink.points) {
                p.x += (float)dx;
                p.y += (float)dy;
            }
            BuildEntry(entries_[(size_t)idx]);
        }
        UpdateSelectionBarWindow();
        InvalidateAll();
    }

    void ScaleSelection(float factor) {
        NormalizeSelection();
        if (selection_.empty()) return;
        DRect b = SelectionBounds();
        POINT pivot{ (LONG)std::lround(b.x + b.w / 2), (LONG)std::lround(b.y + b.h / 2) };
        PushUndo();
        for (int idx : selection_) {
            if (idx < 0 || idx >= (int)entries_.size()) continue;
            auto& ink = entries_[(size_t)idx].ink;
            for (auto& p : ink.points) {
                p.x = pivot.x + (p.x - pivot.x) * factor;
                p.y = pivot.y + (p.y - pivot.y) * factor;
            }
            ink.width *= factor;
            BuildEntry(entries_[(size_t)idx]);
        }
        UpdateSelectionBarWindow();
        InvalidateAll();
    }

    void RotateSelection90() {
        NormalizeSelection();
        if (selection_.empty()) return;
        DRect b = SelectionBounds();
        POINT pivot{ (LONG)std::lround(b.x + b.w / 2), (LONG)std::lround(b.y + b.h / 2) };
        PushUndo();
        for (int idx : selection_) {
            if (idx < 0 || idx >= (int)entries_.size()) continue;
            for (auto& p : entries_[(size_t)idx].ink.points) {
                float qx = p.x - pivot.x, qy = p.y - pivot.y;
                p.x = pivot.x - qy;
                p.y = pivot.y + qx;
            }
            BuildEntry(entries_[(size_t)idx]);
        }
        UpdateSelectionBarWindow();
        InvalidateAll();
    }

    void CopySelection() {
        NormalizeSelection();
        if (selection_.empty()) return;
        clipboard_.clear();
        for (int idx : selection_)
            if (idx >= 0 && idx < (int)entries_.size())
                clipboard_.push_back(entries_[(size_t)idx].ink);
        clipboardValid_ = !clipboard_.empty();
        pasteSerial_ = 0;
        UpdateSelectionBarWindow();
        InvalidateSelectionBar();
    }

    void PasteSelection() {
        if (!clipboardValid_ || clipboard_.empty()) return;
        PushUndo();
        if (tool_ != FS_SELECT) {
            tool_ = FS_SELECT;
            UpdateClickThrough();
            InvalidateToolbar();
        }
        ++pasteSerial_;
        float offset = 24.0f * (float)pasteSerial_;
        std::map<int, int> groupMap;
        std::vector<int> added;
        for (auto& src : clipboard_) {
            InkStroke copy = src;
            for (auto& p : copy.points) {
                p.x += offset;
                p.y += offset;
            }
            if (copy.group != 0) {
                auto it = groupMap.find(copy.group);
                if (it == groupMap.end()) {
                    int gid = groupSeq_++;
                    groupMap[copy.group] = gid;
                    copy.group = gid;
                } else copy.group = it->second;
            }
            Entry e;
            e.ink = std::move(copy);
            BuildEntry(e);
            added.push_back((int)entries_.size());
            entries_.push_back(std::move(e));
        }
        selection_ = std::move(added);
        NormalizeSelection();
        UpdateSelectionBarWindow();
        InvalidateAll();
    }

    void GroupSelection(bool group) {
        NormalizeSelection();
        if (selection_.empty()) return;
        if (group && selection_.size() < 2) return;
        PushUndo();
        int gid = group ? groupSeq_++ : 0;
        for (int idx : selection_)
            if (idx >= 0 && idx < (int)entries_.size())
                entries_[(size_t)idx].ink.group = gid;
        NormalizeSelection();
        UpdateSelectionBarWindow();
        InvalidateAll();
    }

    void SelectAll() {
        selection_.clear();
        for (int i = 0; i < (int)entries_.size(); i++) selection_.push_back(i);
        if (tool_ != FS_SELECT) {
            tool_ = FS_SELECT;
            UpdateClickThrough();
            InvalidateToolbar();
        }
        NormalizeSelection();
        UpdateSelectionBarWindow();
        InvalidateAll();
    }

    bool SelectionActionEnabled(int action) const {
        switch (action) {
        case 0: return !selection_.empty();
        case 1: return clipboardValid_ && !clipboard_.empty();
        case 2: return selection_.size() >= 2;
        case 3: return AnyGroupSelected();
        case 4: return !selection_.empty();
        case 5: return !selection_.empty();
        default: return false;
        }
    }

    void InvalidateSelectionBar() {
        if (selBar_) InvalidateRect(selBar_, nullptr, FALSE);
    }

    void CloseSelectionBar() {
        if (!selBar_) return;
        HWND h = selBar_;
        selBar_ = nullptr;
        DestroyWindow(h);
        selCanvas_.reset();
        selHover_ = -1;
    }

    void CreateSelectionBar() {
        if (selBar_ || !toolbar_) return;
        UINT dpi = GetWindowDpi(toolbar_);
        float sc = dpi / 96.0f;
        selBar_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                  kSelBarClass, L"选择", WS_POPUP, 0, 0,
                                  (int)std::lround(286 * sc), (int)std::lround(34 * sc),
                                  toolbar_, nullptr, GetModuleHandleW(nullptr), this);
        if (!selBar_) return;
        int radius = (int)std::lround(8 * sc);
        HRGN rgn = CreateRoundRectRgn(0, 0, (int)std::lround(286 * sc) + 1,
                                      (int)std::lround(34 * sc) + 1, radius * 2, radius * 2);
        if (rgn) SetWindowRgn(selBar_, rgn, TRUE);
        selCanvas_ = std::make_unique<Canvas>();
    }

    void UpdateSelectionBarWindow() {
        if (!visible_ || collapsed_ || tool_ != FS_SELECT || selection_.empty()) {
            if (selBar_) ShowWindow(selBar_, SW_HIDE);
            return;
        }
        if (!selBar_) CreateSelectionBar();
        if (!selBar_) return;
        UINT dpi = GetWindowDpi(toolbar_);
        float sc = dpi / 96.0f;
        int ww = (int)std::lround(286 * sc);
        int wh = (int)std::lround(34 * sc);
        DRect b = SelectionBounds();
        if (b.w <= 0 || b.h <= 0) { ShowWindow(selBar_, SW_HIDE); return; }
        int x = (int)std::lround(b.x + b.w / 2) - ww / 2;
        int y = (int)std::lround(b.y) - wh - (int)std::lround(30 * sc);
        POINT center{ (LONG)std::lround(b.x + b.w / 2), (LONG)std::lround(b.y) };
        HMONITOR mon = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        RECT work{};
        if (mon && GetMonitorInfoW(mon, &mi)) work = mi.rcWork;
        else work = ToolbarWindowWorkArea();
        if (y < work.top + 4)
            y = (int)std::lround(b.y + b.h) + (int)std::lround(10 * sc);
        x = std::max((int)work.left + 4, std::min(x, (int)work.right - ww - 4));
        y = std::max((int)work.top + 4, std::min(y, (int)work.bottom - wh - 4));
        SetWindowPos(selBar_, HWND_TOPMOST, x, y, ww, wh,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(selBar_, nullptr, FALSE);
    }

    void PaintSelectionBar() {
        if (!selBar_) return;
        if (!selCanvas_) selCanvas_ = std::make_unique<Canvas>();
        Canvas& c = *selCanvas_;
        const Palette& p = ThemePalette();
        if (!c.Begin(selBar_, p, true)) return;
        c.SetTranslateUi(true);
        float w = c.widthDip(), h = c.heightDip();
        c.FillRect(0, 0, w, h, p.surface);
        c.StrokeRound(0.5f, 0.5f, std::max(1.0f, w - 1), std::max(1.0f, h - 1),
                      7, p.border, 1.0f);
        const wchar_t* labels[] = { L"复制", L"粘贴", L"组合", L"拆组", L"旋转", L"删除" };
        for (int i = 0; i < 6; i++) {
            DRect r{ 6.0f + i * 46.0f, 4.0f, 44.0f, h - 8 };
            bool enabled = SelectionActionEnabled(i);
            if (enabled && selHover_ == i)
                c.FillRound(r.x, r.y, r.w, r.h, 5, p.hover);
            Color fg = enabled ? p.fg : p.muted;
            if (!enabled) fg.a = 0.5f;
            c.DrawText(labels[i], r.x, r.y, r.w, r.h, 10.5f, fg, false, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        }
        c.End();
    }

    void ExecuteSelectionAction(int action) {
        switch (action) {
        case 0: CopySelection(); break;
        case 1: PasteSelection(); break;
        case 2: GroupSelection(true); break;
        case 3: GroupSelection(false); break;
        case 4: RotateSelection90(); break;
        case 5: DeleteSelection(); break;
        default: break;
        }
        UpdateSelectionBarWindow();
        InvalidateSelectionBar();
    }

    LRESULT HandleSelBar(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            PaintSelectionBar();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            UINT dpi = GetWindowDpi(hwnd);
            float sc = dpi / 96.0f;
            float x = GET_X_LPARAM(lp) / sc, y = GET_Y_LPARAM(lp) / sc;
            int hover = -1;
            if (y >= 4 && y <= 30) {
                int idx = (int)((x - 6) / 46);
                if (idx >= 0 && idx < 6 && x >= 6 + idx * 46 && x <= 50 + idx * 46)
                    hover = idx;
            }
            if (hover != selHover_) {
                selHover_ = hover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            selHover_ = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            UINT dpi = GetWindowDpi(hwnd);
            float sc = dpi / 96.0f;
            float x = GET_X_LPARAM(lp) / sc, y = GET_Y_LPARAM(lp) / sc;
            if (y >= 4 && y <= 30) {
                int idx = (int)((x - 6) / 46);
                if (idx >= 0 && idx < 6 && x >= 6 + idx * 46 && x <= 50 + idx * 46) {
                    if (SelectionActionEnabled(idx)) ExecuteSelectionAction(idx);
                    return 0;
                }
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, selHover_ >= 0 ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static LRESULT CALLBACK SelBarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        Impl* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = reinterpret_cast<Impl*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        } else {
            self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        return self->HandleSelBar(hwnd, msg, wp, lp);
    }

    // ------------------------------------------------------------- surfaces
    void RegisterClasses() {
        static bool overlayRegistered = false;
        if (!overlayRegistered) {
            WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = SurfaceProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
            wc.lpszClassName = kOverlayClass;
            RegisterClassExW(&wc);
            overlayRegistered = true;
        }
        static bool toolbarRegistered = false;
        if (!toolbarRegistered) {
            WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = ToolbarProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kToolbarClass;
            RegisterClassExW(&wc);
            toolbarRegistered = true;
        }
        static bool sizeRegistered = false;
        if (!sizeRegistered) {
            WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = SizePopupProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
            wc.lpszClassName = kSizePopupClass;
            RegisterClassExW(&wc);
            sizeRegistered = true;
        }
        static bool selRegistered = false;
        if (!selRegistered) {
            WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc);
            wc.style = CS_HREDRAW | CS_VREDRAW;
            wc.lpfnWndProc = SelBarProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
            wc.lpszClassName = kSelBarClass;
            RegisterClassExW(&wc);
            selRegistered = true;
        }
    }

    void EnumerateMonitors(std::vector<RECT>& out) {
        out.clear();
        EnumDisplayMonitors(nullptr, nullptr,
            [](HMONITOR mon, HDC, LPRECT, LPARAM data) -> BOOL {
                MONITORINFO mi{}; mi.cbSize = sizeof(mi);
                if (GetMonitorInfoW(mon, &mi)) {
                    auto* list = reinterpret_cast<std::vector<RECT>*>(data);
                    list->push_back(mi.rcMonitor);
                }
                return TRUE;
            }, (LPARAM)&out);
    }

    bool CreateSurface(const RECT& mon) {
        Surface s;
        s.mon = mon;
        s.w = mon.right - mon.left;
        s.h = mon.bottom - mon.top;
        if (s.w <= 0 || s.h <= 0) return false;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = s.w;
        bi.bmiHeader.biHeight = -s.h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        s.bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &s.bits, nullptr, 0);
        if (!s.bmp) return false;
        s.dc = CreateCompatibleDC(nullptr);
        if (!s.dc) {
            DeleteObject(s.bmp);
            return false;
        }
        s.oldBmp = (HBITMAP)SelectObject(s.dc, s.bmp);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
        if (FAILED(Engine::Instance().D2D()->CreateDCRenderTarget(&props, &s.rt)) || !s.rt) {
            SelectObject(s.dc, s.oldBmp);
            DeleteDC(s.dc);
            DeleteObject(s.bmp);
            return false;
        }
        LONG_PTR ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
        if (tool_ == FS_MOUSE) ex |= WS_EX_TRANSPARENT;
        s.hwnd = CreateWindowExW((DWORD)ex, kOverlayClass, L"", WS_POPUP,
                                 mon.left, mon.top, s.w, s.h, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), this);
        if (!s.hwnd) {
            SelectObject(s.dc, s.oldBmp);
            DeleteDC(s.dc);
            DeleteObject(s.bmp);
            return false;
        }
        surfaces_.push_back(std::move(s));
        return true;
    }

    bool CreateSurfaces() {
        if (!Engine::Instance().D2D()) return false;
        std::vector<RECT> monitors;
        EnumerateMonitors(monitors);
        if (monitors.empty()) return false;
        for (auto& mon : monitors) {
            if (!CreateSurface(mon)) return false;
        }
        return true;
    }

    void DestroyToolbar() {
        CloseSizePopup();
        CloseSelectionBar();
        if (toolbar_) {
            DestroyWindow(toolbar_);
            toolbar_ = nullptr;
        }
        toolbarCanvas_.reset();
        toolbarItems_.clear();
        toolbarSeps_.clear();
        hoverCmd_ = 0;
        draggingToolbar_ = gripPressed_ = gripMoved_ = false;
    }

    void DestroyWindows() {
        DestroyToolbar();
        for (auto& s : surfaces_) {
            if (s.hwnd) DestroyWindow(s.hwnd);
            if (s.dc) {
                if (s.oldBmp) SelectObject(s.dc, s.oldBmp);
                DeleteDC(s.dc);
            }
            if (s.bmp) DeleteObject(s.bmp);
        }
        surfaces_.clear();
    }

    void UpdateClickThrough() {
        bool transparent = (tool_ == FS_MOUSE);
        for (auto& s : surfaces_) {
            if (!s.hwnd) continue;
            LONG_PTR ex = GetWindowLongPtrW(s.hwnd, GWL_EXSTYLE);
            LONG_PTR want = transparent ? (ex | WS_EX_TRANSPARENT)
                                        : (ex & ~WS_EX_TRANSPARENT);
            if (want != ex)
                SetWindowLongPtrW(s.hwnd, GWL_EXSTYLE, want);
            SetWindowPos(s.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        // Raising the annotation surfaces keeps them above the toolbar in the
        // topmost band; raise the toolbar again so its buttons stay clickable.
        if (toolbar_) {
            SetWindowPos(toolbar_, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    // ---------------------------------------------------------------- render
    void RenderSurface(Surface& s) {
        if (!s.hwnd || !s.rt || !s.brush) return;
        RECT rc{ 0, 0, s.w, s.h };
        if (FAILED(s.rt->BindDC(s.dc, &rc))) return;
        s.rt->BeginDraw();
        s.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        // A fully transparent layered window is transparent to hit-testing as
        // well: clicks on empty desktop would fall through and no pen stroke
        // could start. Keep an almost invisible (1/255) black wash while a
        // drawing/selection tool is active so the whole overlay is clickable;
        // the mouse tool clears it and enables WS_EX_TRANSPARENT.
        if (tool_ != FS_MOUSE) {
            s.brush->SetColor(D2D1::ColorF(0, 0, 0, 1.0f / 255.0f));
            s.rt->FillRectangle(D2D1::RectF(0, 0, (float)s.w, (float)s.h), s.brush.Get());
        }
        s.rt->SetTransform(D2D1::Matrix3x2F::Translation((float)-s.mon.left,
                                                         (float)-s.mon.top));
        float scale = ScaleForSurface(s);
        auto drawInk = [&](const InkStroke& ink, ID2D1PathGeometry* geo) {
            if (ink.color.a <= 0.001f) return;
            s.brush->SetColor(ToD2D(ink.color));
            float width = std::max(1.0f, ink.width * scale);
            if (ink.points.size() == 1) {
                s.rt->FillEllipse(D2D1::Ellipse(
                    D2D1::Point2F(ink.points[0].x, ink.points[0].y),
                    width / 2, width / 2), s.brush.Get());
            } else if (geo) {
                s.rt->DrawGeometry(geo, s.brush.Get(), width, strokeStyle_.Get());
            }
        };
        for (auto& e : entries_) {
            RECT er{ (LONG)std::floor(e.minX), (LONG)std::floor(e.minY),
                     (LONG)std::ceil(e.maxX), (LONG)std::ceil(e.maxY) };
            if (!RectIntersects(er, s.mon)) continue;
            drawInk(e.ink, e.geo.Get());
        }
        if (drawing_ && !current_.points.empty()) {
            ID2D1PathGeometry* geo = currentGeo_.Get();
            drawInk(current_, geo);
        }

        // Selection frame and handles.
        if (!selection_.empty()) {
            DRect b = SelectionBounds();
            s.accent->SetColor(D2D1::ColorF(0.25f, 0.55f, 1.0f, 0.95f));
            s.rt->DrawRectangle(
                D2D1::RectF(b.x, b.y, b.x + b.w, b.y + b.h), s.accent.Get(), 1.5f);
            const float hs = 5.0f;
            for (auto pt : { D2D1::Point2F(b.x, b.y), D2D1::Point2F(b.x + b.w, b.y),
                             D2D1::Point2F(b.x, b.y + b.h), D2D1::Point2F(b.x + b.w, b.y + b.h) }) {
                s.rt->FillRectangle(
                    D2D1::RectF(pt.x - hs, pt.y - hs, pt.x + hs, pt.y + hs), s.accent.Get());
            }
            float cx = b.x + b.w / 2;
            s.rt->DrawLine(D2D1::Point2F(cx, b.y), D2D1::Point2F(cx, b.y - 22),
                           s.accent.Get(), 1.2f);
            s.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, b.y - 22), 5, 5),
                              s.accent.Get());
        }
        if (lassoing_) {
            DRect r = NormalizedRect((float)lassoStart_.x, (float)lassoStart_.y,
                                     (float)lassoEnd_.x, (float)lassoEnd_.y);
            s.brush->SetColor(D2D1::ColorF(0.25f, 0.55f, 1.0f, 0.12f));
            s.rt->FillRectangle(D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h), s.brush.Get());
            s.accent->SetColor(D2D1::ColorF(0.25f, 0.55f, 1.0f, 0.95f));
            s.rt->DrawRectangle(D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h),
                                s.accent.Get(), 1.0f);
        }
        if (tool_ == FS_ERASER && PtInRect(&s.mon, mouse_)) {
            float radius = (float)std::max(1, settings_ ? settings_->fullscreenEraserSize : 18) * scale;
            s.accent->SetColor(D2D1::ColorF(0, 0, 0, 0.55f));
            s.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((float)mouse_.x, (float)mouse_.y),
                                          radius, radius), s.accent.Get(), 2.5f);
            s.accent->SetColor(D2D1::ColorF(1, 1, 1, 0.9f));
            s.rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F((float)mouse_.x, (float)mouse_.y),
                                          radius, radius), s.accent.Get(), 1.0f);
        }

        s.rt->SetTransform(D2D1::Matrix3x2F::Identity());
        HRESULT hr = s.rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) return;
        POINT dst{ s.mon.left, s.mon.top };
        SIZE size{ s.w, s.h };
        POINT src{ 0, 0 };
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(s.hwnd, nullptr, &dst, &size, s.dc, &src, 0, &blend, ULW_ALPHA);
    }

    void RenderAll() {
        for (auto& s : surfaces_) {
            if (!s.brush && s.rt) {
                s.rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &s.brush);
                s.rt->CreateSolidColorBrush(D2D1::ColorF(0.25f, 0.55f, 1.0f, 0.95f), &s.accent);
                if (!s.accent) s.accent = s.brush;
            }
            RenderSurface(s);
        }
    }

    void InvalidateAll() { RenderAll(); }

    void InvalidateRectGlobal(const RECT& rc) {
        for (auto& s : surfaces_)
            if (RectIntersects(rc, s.mon)) RenderSurface(s);
    }

    // -------------------------------------------------------------- toolbar
    void EnsureStrokeStyle() {
        if (strokeStyle_ || !Engine::Instance().D2D()) return;
        D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
            D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
        Engine::Instance().D2D()->CreateStrokeStyle(props, nullptr, 0, &strokeStyle_);
    }

    RECT ToolbarWorkArea() const {
        POINT pt{};
        GetCursorPos(&pt);
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{}; mi.cbSize = sizeof(mi);
        if (mon && GetMonitorInfoW(mon, &mi)) return mi.rcWork;
        RECT r{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        return r;
    }

    RECT ToolbarWindowWorkArea() const {
        if (toolbar_) {
            HMONITOR mon = MonitorFromWindow(toolbar_, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{}; mi.cbSize = sizeof(mi);
            if (mon && GetMonitorInfoW(mon, &mi)) return mi.rcWork;
        }
        return ToolbarWorkArea();
    }

    void BuildToolbarLayout(float workWidthDip) {
        toolbarItems_.clear();
        toolbarSeps_.clear();
        if (collapsed_) {
            toolbarW_ = 32.0f;
            toolbarH_ = 58.0f;
            return;
        }
        const float pad = 5, gap = 2, h = 40;
        struct Item { int cmd; float w; };
        std::vector<Item> row1 = {
            { FS_GRIP, 16 },
            { FS_MOUSE, 32 }, { FS_SELECT, 32 }, { FS_PEN, 32 }, { FS_ERASER, 32 },
            { 0, 6 }, { FS_UNDO, 26 }, { FS_REDO, 26 }
        };
        std::vector<Item> row2 = {
            { 0, 6 }, { FS_COLOR1, 20 }, { FS_COLOR2, 20 }, { FS_COLOR3, 20 },
            { FS_PALETTE, 34 }, { 0, 6 }, { FS_BACK, 34 }, { FS_SETTINGS, 34 },
            { FS_COLLAPSE, 16 }
        };
        auto widthOf = [&](const std::vector<Item>& row) {
            float w = 0;
            for (size_t i = 0; i < row.size(); i++) {
                w += row[i].w;
                if (i + 1 < row.size()) w += gap;
            }
            return w;
        };
        bool narrow = workWidthDip < 460.0f;
        if (narrow) {
            toolbarW_ = std::max(widthOf(row1), widthOf(row2)) + pad * 2;
            toolbarH_ = pad * 2 + h * 2 + 6;
        } else {
            std::vector<Item> one = row1;
            for (auto& it : row2) one.push_back(it);
            toolbarW_ = widthOf(one) + pad * 2;
            toolbarH_ = pad * 2 + h;
        }
        auto placeRow = [&](const std::vector<Item>& row, float y) {
            float x = (toolbarW_ - widthOf(row)) / 2;
            for (size_t i = 0; i < row.size(); i++) {
                if (row[i].cmd == 0) {
                    toolbarSeps_.push_back(x + row[i].w / 2);
                } else {
                    toolbarItems_.push_back({ row[i].cmd, { x, y, row[i].w, h } });
                }
                x += row[i].w + gap;
            }
        };
        if (narrow) {
            placeRow(row1, pad);
            placeRow(row2, pad + h + 6);
        } else {
            std::vector<Item> one = row1;
            for (auto& it : row2) one.push_back(it);
            placeRow(one, pad);
        }
    }

    void ApplyToolbarRegion() {
        if (!toolbar_) return;
        RECT cr{};
        GetClientRect(toolbar_, &cr);
        int w = cr.right - cr.left, h = cr.bottom - cr.top;
        if (w <= 0 || h <= 0) return;
        UINT dpi = GetWindowDpi(toolbar_);
        int radius = (int)std::lround(12.0 * dpi / 96.0);
        radius = std::min(radius, std::max(2, std::min(w, h) / 4));
        HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, radius * 2, radius * 2);
        if (rgn) SetWindowRgn(toolbar_, rgn, TRUE);
    }

    void RaiseToolbar() {
        if (!toolbar_) return;
        SetWindowPos(toolbar_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    void ToggleCollapsed() {
        if (!toolbar_) return;
        UINT dpi = GetWindowDpi(toolbar_);
        float sc = dpi / 96.0f;
        RECT work = ToolbarWindowWorkArea();
        if (!collapsed_) {
            GetWindowRect(toolbar_, &expandedRect_);
            collapsed_ = true;
            BuildToolbarLayout((work.right - work.left) / sc);
            int ww = (int)std::lround(toolbarW_ * sc);
            int wh = (int)std::lround(toolbarH_ * sc);
            int center = (expandedRect_.left + expandedRect_.right) / 2;
            bool leftSide = center < (work.left + work.right) / 2;
            int x = leftSide ? work.left + (int)std::lround(8 * sc)
                             : work.right - ww - (int)std::lround(8 * sc);
            int y = (expandedRect_.top + expandedRect_.bottom) / 2 - wh / 2;
            y = std::max((int)work.top + 8, std::min(y, (int)work.bottom - wh - 8));
            SetWindowPos(toolbar_, HWND_TOPMOST, x, y, ww, wh,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            collapsed_ = false;
            BuildToolbarLayout((work.right - work.left) / sc);
            int ww = (int)std::lround(toolbarW_ * sc);
            int wh = (int)std::lround(toolbarH_ * sc);
            int x = work.left + ((work.right - work.left) - ww) / 2;
            int y = (settings_ && settings_->fullscreenToolbarTop)
                        ? work.top + (int)std::lround(16 * sc)
                        : work.bottom - wh - (int)std::lround(16 * sc);
            if (expandedRect_.right > expandedRect_.left) {
                x = expandedRect_.left;
                y = expandedRect_.top;
                x = std::max((int)work.left + 4, std::min(x, (int)work.right - ww - 4));
                y = std::max((int)work.top + 4, std::min(y, (int)work.bottom - wh - 4));
            }
            SetWindowPos(toolbar_, HWND_TOPMOST, x, y, ww, wh,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        ApplyToolbarRegion();
        UpdateSelectionBarWindow();
        InvalidateRect(toolbar_, nullptr, FALSE);
    }

    static void SizePresets(int kind, const int*& values, int& count) {
        static const int pen[] = { 1, 2, 3, 5, 8, 12 };
        static const int eraser[] = { 8, 12, 18, 26, 36, 48 };
        if (kind == FS_ERASER) {
            values = eraser;
            count = (int)(sizeof(eraser) / sizeof(eraser[0]));
        } else {
            values = pen;
            count = (int)(sizeof(pen) / sizeof(pen[0]));
        }
    }

    void CloseSizePopup() {
        if (!sizePopup_) return;
        HWND h = sizePopup_;
        sizePopup_ = nullptr;
        DestroyWindow(h);
        sizeCanvas_.reset();
        sizeRects_.clear();
        sizeClearRect_ = DRect{};
        sizeClearHover_ = false;
        sizeHover_ = -1;
    }

    void ShowSizePopup(int kind) {
        CloseSizePopup();
        if (!toolbar_ || !settings_) return;
        sizeKind_ = (kind == FS_ERASER) ? FS_ERASER : FS_PEN;
        UINT dpi = GetWindowDpi(toolbar_);
        float sc = dpi / 96.0f;
        int count = 0;
        const int* values = nullptr;
        SizePresets(sizeKind_, values, count);
        (void)values;
        const int wDip = 224, hDip = 62;
        int ww = (int)std::lround(wDip * sc);
        int wh = (int)std::lround(hDip * sc);
        RECT tr{};
        GetWindowRect(toolbar_, &tr);
        int x = (tr.left + tr.right - ww) / 2;
        int y = tr.top - wh - (int)std::lround(8 * sc);
        RECT work = ToolbarWindowWorkArea();
        if (y < work.top + 4) y = tr.bottom + (int)std::lround(8 * sc);
        x = std::max((int)work.left + 4, std::min(x, (int)work.right - ww - 4));
        y = std::max((int)work.top + 4, std::min(y, (int)work.bottom - wh - 4));
        sizePopup_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kSizePopupClass,
                                     L"粗细", WS_POPUP, x, y, ww, wh, toolbar_, nullptr,
                                     GetModuleHandleW(nullptr), this);
        if (!sizePopup_) return;
        UINT radius = (UINT)std::lround(10 * sc);
        HRGN rgn = CreateRoundRectRgn(0, 0, ww + 1, wh + 1, radius * 2, radius * 2);
        if (rgn) SetWindowRgn(sizePopup_, rgn, TRUE);
        sizeCanvas_ = std::make_unique<Canvas>();
        ShowWindow(sizePopup_, SW_SHOWNORMAL);
        SetForegroundWindow(sizePopup_);
        InvalidateRect(sizePopup_, nullptr, FALSE);
    }

    void PaintSizePopup() {
        if (!sizePopup_) return;
        if (!sizeCanvas_) sizeCanvas_ = std::make_unique<Canvas>();
        Canvas& c = *sizeCanvas_;
        const Palette& p = ThemePalette();
        if (!c.Begin(sizePopup_, p, true)) return;
        c.SetTranslateUi(true);
        float w = c.widthDip(), h = c.heightDip();
        c.FillRect(0, 0, w, h, p.surface);
        c.StrokeRound(0.5f, 0.5f, std::max(1.0f, w - 1), std::max(1.0f, h - 1),
                      9, p.border, 1.0f);
        c.DrawText(sizeKind_ == FS_ERASER ? L"橡皮大小" : L"画笔粗细",
                   10, 5, w - 20, 16, 10.5f, p.muted);
        sizeClearRect_ = DRect{};
        if (sizeKind_ == FS_ERASER) {
            sizeClearRect_ = DRect{ w - 62, 4, 52, 18 };
            if (sizeClearHover_)
                c.FillRound(sizeClearRect_.x, sizeClearRect_.y, sizeClearRect_.w,
                            sizeClearRect_.h, 5, p.dangerSoft);
            c.StrokeRound(sizeClearRect_.x, sizeClearRect_.y, sizeClearRect_.w,
                          sizeClearRect_.h, 5, p.danger, 1.0f);
            c.DrawText(L"清屏", sizeClearRect_.x, sizeClearRect_.y, sizeClearRect_.w,
                       sizeClearRect_.h, 10, p.danger, true, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        }
        int count = 0;
        const int* values = nullptr;
        SizePresets(sizeKind_, values, count);
        sizeRects_.clear();
        float pad = 10, cell = (w - pad * 2) / count;
        int current = sizeKind_ == FS_ERASER ? settings_->fullscreenEraserSize
                                             : settings_->fullscreenPenWidth;
        int selected = 0;
        int best = 1 << 30;
        for (int i = 0; i < count; i++) {
            if (std::abs(values[i] - current) < best) {
                best = std::abs(values[i] - current);
                selected = i;
            }
        }
        for (int i = 0; i < count; i++) {
            DRect cellRect{ pad + i * cell, 22, cell, 34 };
            sizeRects_.push_back(cellRect);
            if (sizeHover_ == i)
                c.FillRound(cellRect.x + 1, cellRect.y, cellRect.w - 2, cellRect.h, 6, p.hover);
            float t = count > 1 ? (float)i / (float)(count - 1) : 1.0f;
            float radius = 3.0f + t * 9.0f;
            float cx = cellRect.x + cellRect.w / 2;
            float cy = cellRect.y + cellRect.h / 2;
            Color fill = (i == selected) ? p.accent : p.fg;
            c.FillCircle(cx, cy, radius, fill);
            if (i == selected)
                c.DrawEllipse(cx, cy, radius + 2.5f, radius + 2.5f, p.accent, 1.5f);
        }
        c.End();
    }

    void ApplySizePreset(int index) {
        if (!settings_) return;
        int count = 0;
        const int* values = nullptr;
        SizePresets(sizeKind_, values, count);
        if (index < 0 || index >= count) return;
        if (sizeKind_ == FS_ERASER) {
            settings_->fullscreenEraserSize = std::clamp(
                values[index], Settings::MinFullscreenEraserSize, Settings::MaxFullscreenEraserSize);
        } else {
            settings_->fullscreenPenWidth = std::clamp(
                values[index], Settings::MinFullscreenPenWidth, Settings::MaxFullscreenPenWidth);
        }
        settings_->Save();
        CloseSizePopup();
        InvalidateToolbar();
        InvalidateAll();
    }

    LRESULT HandleSizePopup(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            PaintSizePopup();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            UINT dpi = GetWindowDpi(hwnd);
            float sc = dpi / 96.0f;
            float x = GET_X_LPARAM(lp) / sc, y = GET_Y_LPARAM(lp) / sc;
            int hover = -1;
            for (int i = 0; i < (int)sizeRects_.size(); i++)
                if (sizeRects_[(size_t)i].Contains(x, y)) { hover = i; break; }
            bool clearHover = sizeKind_ == FS_ERASER && sizeClearRect_.Contains(x, y);
            if (clearHover) hover = -1;
            if (hover != sizeHover_ || clearHover != sizeClearHover_) {
                sizeHover_ = hover;
                sizeClearHover_ = clearHover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            sizeHover_ = -1;
            sizeClearHover_ = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            UINT dpi = GetWindowDpi(hwnd);
            float sc = dpi / 96.0f;
            float x = GET_X_LPARAM(lp) / sc, y = GET_Y_LPARAM(lp) / sc;
            if (sizeKind_ == FS_ERASER && sizeClearRect_.Contains(x, y)) {
                ClearAll();
                CloseSizePopup();
                InvalidateToolbar();
                return 0;
            }
            for (int i = 0; i < (int)sizeRects_.size(); i++) {
                if (sizeRects_[(size_t)i].Contains(x, y)) {
                    ApplySizePreset(i);
                    return 0;
                }
            }
            CloseSizePopup();
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) CloseSizePopup();
            return 0;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { CloseSizePopup(); return 0; }
            break;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursorW(nullptr, (sizeHover_ >= 0 || sizeClearHover_)
                                                    ? IDC_HAND : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static LRESULT CALLBACK SizePopupProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        Impl* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = reinterpret_cast<Impl*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        } else {
            self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        return self->HandleSizePopup(hwnd, msg, wp, lp);
    }

    void PositionToolbar(bool reset) {
        if (!toolbar_) return;
        UINT dpi = GetDpiForWindow(toolbar_);
        if (!dpi) dpi = 96;
        float sc = dpi / 96.0f;
        RECT work = ToolbarWorkArea();
        int ww = (int)std::lround(toolbarW_ * sc);
        int wh = (int)std::lround(toolbarH_ * sc);
        int ox = work.left + ((work.right - work.left) - ww) / 2;
        int oy = (settings_ && settings_->fullscreenToolbarTop)
                     ? work.top + (int)std::lround(16 * sc)
                     : work.bottom - wh - (int)std::lround(16 * sc);
        if (reset) {
            SetWindowPos(toolbar_, HWND_TOPMOST, ox, oy, ww, wh,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            RECT cur{};
            GetWindowRect(toolbar_, &cur);
            SetWindowPos(toolbar_, HWND_TOPMOST, cur.left, cur.top, ww, wh,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        ApplyToolbarRegion();
    }

    // The window can move to a monitor with a different DPI, and the initial
    // DPI may only settle after the window has been shown. Recompute the
    // toolbar's DIP layout against its current DPI before painting so no
    // buttons get clipped on the right.
    void SyncToolbarSize(bool reposition) {
        if (!toolbar_) return;
        UINT dpi = GetWindowDpi(toolbar_);
        float sc = dpi / 96.0f;
        RECT work = ToolbarWindowWorkArea();
        BuildToolbarLayout((work.right - work.left) / sc);
        int ww = (int)std::lround(toolbarW_ * sc);
        int wh = (int)std::lround(toolbarH_ * sc);
        if (reposition && !collapsed_) {
            PositionToolbar(true);
            return;
        }
        RECT cr{};
        GetClientRect(toolbar_, &cr);
        if (std::abs((cr.right - cr.left) - ww) > 1 ||
            std::abs((cr.bottom - cr.top) - wh) > 1) {
            RECT wr{};
            GetWindowRect(toolbar_, &wr);
            SetWindowPos(toolbar_, HWND_TOPMOST, wr.left, wr.top, ww, wh,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        ApplyToolbarRegion();
    }

    bool CreateToolbar() {
        UINT dpi = GetDpiForWindow(owner_);
        if (!dpi) dpi = 96;
        float sc = dpi / 96.0f;
        toolbar_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kToolbarClass,
                                   L"全屏笔迹", WS_POPUP, 0, 0,
                                   (int)std::lround(480 * sc), (int)std::lround(54 * sc),
                                   nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!toolbar_) return false;
        // Lay out against the toolbar's real DPI. The owner window may live on
        // a monitor with a different scale factor, and using its DPI here makes
        // the right half of the toolbar fall outside the client area.
        UINT toolbarDpi = GetWindowDpi(toolbar_);
        float toolbarScale = toolbarDpi / 96.0f;
        RECT work = ToolbarWorkArea();
        BuildToolbarLayout((work.right - work.left) / toolbarScale);
        int ww = (int)std::lround(toolbarW_ * toolbarScale);
        int wh = (int)std::lround(toolbarH_ * toolbarScale);
        SetWindowPos(toolbar_, HWND_TOPMOST, 0, 0, ww, wh,
                     SWP_NOMOVE | SWP_NOACTIVATE);
        ApplyToolbarRegion();
        toolbarCanvas_ = std::make_unique<Canvas>();
        PositionToolbar(true);
        ShowWindow(toolbar_, SW_SHOWNOACTIVATE);
        SyncToolbarSize(false);
        InvalidateRect(toolbar_, nullptr, FALSE);
        return true;
    }

    void PaintToolbar() {
        if (!toolbar_) return;
        SyncToolbarSize(false);
        if (!toolbarCanvas_) toolbarCanvas_ = std::make_unique<Canvas>();
        Canvas& c = *toolbarCanvas_;
        const Palette& p = ThemePalette();
        if (!c.Begin(toolbar_, p, true)) return;
        c.SetTranslateUi(true);
        float w = c.widthDip(), h = c.heightDip();
        Color bar = p.surface;
        bar.a = 1.0f;
        c.FillRect(0, 0, w, h, bar);
        c.StrokeRound(0.5f, 0.5f, std::max(1.0f, w - 1), std::max(1.0f, h - 1),
                      10, p.border, 1.0f);
        RECT toolbarRect{};
        GetWindowRect(toolbar_, &toolbarRect);
        RECT workArea = ToolbarWindowWorkArea();
        bool leftSide = (toolbarRect.left + toolbarRect.right) / 2 <
                        (workArea.left + workArea.right) / 2;
        if (collapsed_) {
            Color grip = p.muted;
            grip.a = 0.85f;
            for (int i = 0; i < 3; i++)
                c.FillRound(w / 2 - 5, 10 + i * 4, 10, 2, 1, grip);
            c.DrawText(L"笔", 0, h / 2 - 10, w, 22, 15, p.accent, true, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
            c.DrawText(leftSide ? L"›" : L"‹", 0, h - 20, w, 16, 14, p.muted,
                       false, false, DWRITE_TEXT_ALIGNMENT_CENTER,
                       DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
            c.End();
            return;
        }
        for (float sx : toolbarSeps_) c.FillRect(sx, 10, 1, h - 20, p.border);
        for (auto& item : toolbarItems_) {
            bool active = (item.cmd == FS_MOUSE && tool_ == FS_MOUSE) ||
                          (item.cmd == FS_SELECT && tool_ == FS_SELECT) ||
                          (item.cmd == FS_PEN && tool_ == FS_PEN) ||
                          (item.cmd == FS_ERASER && tool_ == FS_ERASER);
            bool hover = hoverCmd_ == item.cmd;
            if (item.cmd == FS_COLLAPSE) {
                if (hover) c.FillRound(item.rect.x, item.rect.y, item.rect.w, item.rect.h, 6, p.hover);
                c.DrawText(leftSide ? L"‹" : L"›", item.rect.x, item.rect.y,
                           item.rect.w, item.rect.h, 14, p.muted, false, false,
                           DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
                continue;
            }
            if (item.cmd == FS_GRIP) {
                if (hover) c.FillRound(item.rect.x, item.rect.y, item.rect.w, item.rect.h, 6, p.hover);
                c.FillRound(item.rect.x + 2, item.rect.y + 6, item.rect.w - 4,
                            item.rect.h - 12, 5, p.accentSoft);
                Color dot = p.accent;
                dot.a = 0.8f;
                float cy = item.rect.y + item.rect.h / 2;
                for (int i = -1; i <= 1; i++)
                    c.FillCircle(item.rect.x + item.rect.w / 2, cy + i * 4.0f, 1.2f, dot);
                continue;
            }
            if (item.cmd >= FS_COLOR1 && item.cmd <= FS_COLOR3) {
                int slot = item.cmd - FS_COLOR1;
                bool selected = colorSlot_ == slot;
                float cx = item.rect.x + item.rect.w / 2;
                float cy = item.rect.y + item.rect.h / 2;
                if (hover) c.FillRound(item.rect.x, item.rect.y, item.rect.w, item.rect.h, 6, p.hover);
                Color col = Color(0.1f, 0.1f, 0.1f, 1.0f);
                if (settings_ && !settings_->inkPresets.empty()) {
                    int idx = slot < (int)settings_->fullscreenInkPresets.size()
                                  ? settings_->fullscreenInkPresets[(size_t)slot] : 0;
                    idx = std::clamp(idx, 0, (int)settings_->inkPresets.size() - 1);
                    col = settings_->inkPresets[(size_t)idx];
                }
                c.FillCircle(cx, cy, 6.5f, col);
                c.DrawEllipse(cx, cy, 6.5f, 6.5f,
                              selected ? p.accent : p.muted,
                              selected ? 2.0f : 1.0f);
            } else {
                Color bg = active ? p.accent : (hover ? p.hover : Color(0, 0, 0, 0));
                if (bg.a > 0)
                    c.FillRound(item.rect.x, item.rect.y, item.rect.w, item.rect.h, 6, bg);
                Color fg = active ? Color(1, 1, 1, 1) : p.fg;
                const wchar_t* label = L"";
                switch (item.cmd) {
                case FS_MOUSE: label = L"鼠标"; break;
                case FS_SELECT: label = L"选择"; break;
                case FS_PEN: label = L"画笔"; break;
                case FS_ERASER: label = L"橡皮"; break;
                case FS_UNDO: label = L"↶"; break;
                case FS_REDO: label = L"↷"; break;
                case FS_PALETTE: label = L"调色盘"; break;
                case FS_BACK: label = L"返回"; break;
                case FS_SETTINGS: label = L"设置"; break;
                default: break;
                }
                float size = (item.cmd == FS_UNDO || item.cmd == FS_REDO)
                                 ? 16.0f : (item.cmd == FS_PALETTE ? 9.5f : 10.5f);
                c.DrawText(label, item.rect.x, item.rect.y, item.rect.w, item.rect.h,
                           size, fg, item.cmd == FS_BACK || item.cmd == FS_SETTINGS,
                           false, DWRITE_TEXT_ALIGNMENT_CENTER,
                           DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
            }
        }
        c.End();
    }

    int HitToolbar(int x, int y) const {
        // Mouse messages carry physical client pixels while toolbarItems_ are
        // laid out in DIPs. Convert before hit-testing, otherwise clicks land
        // on the wrong button on scaled monitors.
        UINT dpi = GetDpiForWindow(toolbar_);
        if (!dpi) dpi = 96;
        float scale = dpi / 96.0f;
        float dx = x / scale, dy = y / scale;
        for (auto& item : toolbarItems_)
            if (item.rect.Contains(dx, dy)) return item.cmd;
        return 0;
    }

    void ActivateToolbarCommand(int cmd) {
        int tool = ToolForCommand(cmd);
        if (tool) { SetTool(tool); return; }
        if (cmd == FS_UNDO) { Undo(); InvalidateToolbar(); return; }
        if (cmd == FS_REDO) { Redo(); InvalidateToolbar(); return; }
        if (cmd == FS_CLEAR) { ClearAll(); InvalidateToolbar(); return; }
        if (cmd >= FS_COLOR1 && cmd <= FS_COLOR3) {
            colorSlot_ = cmd - FS_COLOR1;
            InvalidateToolbar();
            return;
        }
        if (cmd == FS_PALETTE) {
            if (cb_.palette) cb_.palette(ActivePreset());
            return;
        }
        if (cmd == FS_COLLAPSE) { ToggleCollapsed(); return; }
        if (cmd == FS_BACK) {
            if (toolbar_) PostMessageW(toolbar_, WM_FS_EXIT, 0, 0);
            else RequestExit();
            return;
        }
        if (cmd == FS_SETTINGS) {
            if (cb_.settings) cb_.settings();
            return;
        }
    }

    void InvalidateToolbar() {
        if (toolbar_) InvalidateRect(toolbar_, nullptr, FALSE);
    }

    void SetTool(int tool) {
        if (tool < FS_MOUSE || tool > FS_ERASER) return;
        tool_ = tool;
        if (tool != FS_PEN && drawing_) {
            drawing_ = false;
            current_.points.clear();
            currentGeo_.Reset();
            if (GetCapture()) ReleaseCapture();
        }
        if (tool != FS_ERASER) {
            erasing_ = false;
            erasePushed_ = false;
        }
        if (tool != FS_SELECT) {
            lassoing_ = false;
            selection_.clear();
        }
        UpdateClickThrough();
        UpdateSelectionBarWindow();
        InvalidateToolbar();
        InvalidateAll();
    }

    void RequestExit() {
        if (cb_.exit) cb_.exit();
        else Hide();
    }

    void Show() {
        if (visible_) return;
        if (!Engine::Instance().D2D()) return;
        collapsed_ = false;
        expandedRect_ = {};
        if (surfaces_.empty() && !CreateSurfaces()) {
            DestroyWindows();
            return;
        }
        EnsureStrokeStyle();
        if (!toolbar_ && !CreateToolbar()) {
            DestroyWindows();
            return;
        }
        if (settings_) {
            settings_->NormalizeInkPresets();
            tool_ = FS_PEN;
            colorSlot_ = 0;
        }
        selection_.clear();
        current_.points.clear();
        currentGeo_.Reset();
        drawing_ = erasing_ = lassoing_ = false;
        selDragMode_ = 0;
        selPushed_ = false;
        selOriginal_.clear();
        erasePushed_ = false;
        hoverCmd_ = 0;
        draggingToolbar_ = gripPressed_ = gripMoved_ = false;
        visible_ = true;
        UpdateClickThrough();
        for (auto& s : surfaces_) {
            ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
        }
        ShowWindow(toolbar_, SW_SHOWNOACTIVATE);
        // Showing the surfaces brings them to the top of the topmost band;
        // raise the toolbar again so it stays clickable above the annotation
        // layer (which is intentionally the lowest topmost window).
        SetWindowPos(toolbar_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SyncToolbarSize(true);
        InvalidateToolbar();
        RenderAll();
        if (!hook_ && hotkeysEnabled_) {
            hookOwner_ = this;
            hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, HookProc, GetModuleHandleW(nullptr), 0);
        }
    }

    void Hide() {
        if (!visible_ && !toolbar_ && surfaces_.empty()) return;
        visible_ = false;
        if (hook_) {
            UnhookWindowsHookEx(hook_);
            hook_ = nullptr;
            hookOwner_ = nullptr;
        }
        CloseSizePopup();
        CloseSelectionBar();
        drawing_ = false;
        erasing_ = false;
        lassoing_ = false;
        selDragMode_ = 0;
        selPushed_ = false;
        selOriginal_.clear();
        selection_.clear();
        current_.points.clear();
        currentGeo_.Reset();
        if (GetCapture()) ReleaseCapture();
        // Hide all overlay windows but keep the stroke data, undo history and
        // clipboard. Re-entering full-screen ink recreates the windows and
        // restores the same annotations.
        DestroyWindows();
        if (owner_ && IsWindow(owner_)) {
            if (IsIconic(owner_)) ShowWindow(owner_, SW_RESTORE);
            SetForegroundWindow(owner_);
        }
    }

    void ApplySettings() {
        if (!settings_) return;
        settings_->NormalizeInkPresets();
        colorSlot_ = std::clamp(colorSlot_, 0, std::max(0, (int)settings_->fullscreenInkPresets.size() - 1));
        if (toolbar_) {
            SyncToolbarSize(true);
            InvalidateToolbar();
        }
        UpdateSelectionBarWindow();
        InvalidateSelectionBar();
        InvalidateAll();
    }

    UINT GetWindowDpi(HWND hwnd) const {
        UINT dpi = GetDpiForWindow(hwnd);
        return dpi ? dpi : 96;
    }

    void ApplyPalettePreset(int presetIndex) {
        if (!settings_) return;
        settings_->NormalizeInkPresets();
        if (presetIndex < 0 || presetIndex >= (int)settings_->inkPresets.size()) return;
        auto& slots = settings_->fullscreenInkPresets;
        if (slots.empty()) return;
        int slot = std::clamp(colorSlot_, 0, (int)slots.size() - 1);
        int old = slots[(size_t)slot];
        for (size_t i = 0; i < slots.size(); i++) {
            if ((int)i != slot && slots[i] == presetIndex) slots[i] = old;
        }
        slots[(size_t)slot] = presetIndex;
        settings_->NormalizeInkPresets();
        settings_->Save();
        InvalidateToolbar();
        if (cb_.paletteChanged) cb_.paletteChanged();
    }

    // ------------------------------------------------------------- messaging
    static LRESULT CALLBACK SurfaceProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        Impl* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = reinterpret_cast<Impl*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        } else {
            self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        return self->HandleSurface(hwnd, msg, wp, lp);
    }

    static LRESULT CALLBACK ToolbarProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        Impl* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = reinterpret_cast<Impl*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        } else {
            self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        return self->HandleToolbar(hwnd, msg, wp, lp);
    }

    LRESULT HandleSurface(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_NCHITTEST:
            if (tool_ == FS_MOUSE) return HTTRANSPARENT;
            return HTCLIENT;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                if (tool_ == FS_SELECT) {
                    POINT pt{};
                    GetCursorPos(&pt);
                    int mode = HitSelectionHandle(pt);
                    SetCursor(LoadCursorW(nullptr,
                        mode == 3 ? IDC_HAND : (mode ? IDC_SIZEALL : IDC_ARROW)));
                } else if (tool_ == FS_PEN || tool_ == FS_ERASER) {
                    SetCursor(LoadCursorW(nullptr, IDC_CROSS));
                } else {
                    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
                }
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN: {
            if (tool_ == FS_MOUSE) return 0;
            RaiseToolbar();
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(hwnd, &pt);
            mouse_ = pt;
            SetCapture(hwnd);
            if (tool_ == FS_ERASER) {
                erasing_ = true;
                erasePushed_ = false;
                EraseAt(pt, SurfaceScaleAt(pt));
                return 0;
            }
            if (tool_ == FS_SELECT) {
                NormalizeSelection();
                int mode = HitSelectionHandle(pt);
                if (mode != 0) {
                    BeginSelectionDrag(mode, pt);
                    return 0;
                }
                selection_.clear();
                UpdateSelectionBarWindow();
                lassoing_ = true;
                lassoStart_ = lassoEnd_ = pt;
                InvalidateAll();
                return 0;
            }
            // Pen.
            undo_.push_back(Snapshot());
            if (undo_.size() > 80) undo_.erase(undo_.begin());
            redo_.clear();
            current_ = InkStroke{};
            current_.color = ActiveColor();
            current_.width = (float)std::max(1, settings_ ? settings_->fullscreenPenWidth : 3);
            current_.points.push_back({ (float)pt.x, (float)pt.y });
            currentGeo_.Reset();
            drawing_ = true;
            InvalidateRectGlobal(RECT{ pt.x - 8, pt.y - 8, pt.x + 8, pt.y + 8 });
            return 0;
        }
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(hwnd, &pt);
            mouse_ = pt;
            if (drawing_) {
                if (!current_.points.empty()) {
                    auto& last = current_.points.back();
                    float dx = (float)pt.x - last.x, dy = (float)pt.y - last.y;
                    if (dx * dx + dy * dy < 1.0f) return 0;
                }
                current_.points.push_back({ (float)pt.x, (float)pt.y });
                RebuildCurrent();
                RECT er{ pt.x - 24, pt.y - 24, pt.x + 24, pt.y + 24 };
                InvalidateRectGlobal(er);
                return 0;
            }
            if (erasing_) {
                EraseAt(pt, SurfaceScaleAt(pt));
                InvalidateAll();
                return 0;
            }
            if (lassoing_) {
                lassoEnd_ = pt;
                InvalidateAll();
                return 0;
            }
            if (selDragMode_) {
                int dx = pt.x - selDragStart_.x, dy = pt.y - selDragStart_.y;
                if (!selPushed_) {
                    if (std::abs(dx) < 2 && std::abs(dy) < 2) return 0;
                    PushUndo();
                    selPushed_ = true;
                }
                ApplySelectionDrag(pt, true);
                return 0;
            }
            if (tool_ == FS_ERASER) InvalidateAll();
            return 0;
        }
        case WM_MOUSELEAVE:
            mouse_.x = -1000000;
            mouse_.y = -1000000;
            if (tool_ == FS_ERASER) InvalidateAll();
            return 0;
        case WM_LBUTTONUP: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(hwnd, &pt);
            mouse_ = pt;
            if (drawing_) {
                if (!current_.points.empty()) {
                    Entry e;
                    e.ink = current_;
                    BuildEntry(e);
                    entries_.push_back(std::move(e));
                }
                current_.points.clear();
                currentGeo_.Reset();
                drawing_ = false;
            }
            if (lassoing_) {
                lassoEnd_ = pt;
                DRect r = NormalizedRect((float)lassoStart_.x, (float)lassoStart_.y,
                                         (float)lassoEnd_.x, (float)lassoEnd_.y);
                if (r.w >= 3 || r.h >= 3) SelectInRect(r);
                lassoing_ = false;
                UpdateSelectionBarWindow();
            }
            if (selDragMode_) EndSelectionDrag();
            erasing_ = false;
            erasePushed_ = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            InvalidateAll();
            return 0;
        }
        case WM_RBUTTONUP:
            if (tool_ == FS_SELECT && !selection_.empty()) {
                selection_.clear();
                UpdateSelectionBarWindow();
                InvalidateAll();
            }
            return 0;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (tool_ == FS_SELECT && !selection_.empty() && settings_ && delta != 0) {
                float notches = (float)delta / (float)WHEEL_DELTA;
                float factor = std::clamp(1.0f + notches * 0.02f * (float)settings_->wheelStep,
                                          0.5f, 2.0f);
                ScaleSelection(factor);
            }
            // Two-finger scrolling must never add ink. The overlay intentionally
            // consumes the wheel so a pen stroke cannot be started by the
            // horizontal/vertical precision-touchpad gesture.
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { RequestExit(); return 0; }
            if (wp == VK_DELETE || wp == VK_BACK) { DeleteSelection(); return 0; }
            break;
        case WM_CAPTURECHANGED:
            if ((HWND)lp != hwnd) {
                if (drawing_ && !current_.points.empty()) {
                    Entry e;
                    e.ink = current_;
                    BuildEntry(e);
                    entries_.push_back(std::move(e));
                }
                drawing_ = erasing_ = lassoing_ = false;
                selDragMode_ = 0;
                selPushed_ = false;
                selOriginal_.clear();
                current_.points.clear();
                currentGeo_.Reset();
                if (visible_) InvalidateAll();
            }
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE) RaiseToolbar();
            return 0;
        case WM_MOUSEACTIVATE:
            RaiseToolbar();
            return MA_ACTIVATE;
        case WM_FS_EXIT:
            RequestExit();
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT HandleToolbar(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd, &ps);
            PaintToolbar();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            int cmd = draggingToolbar_ ? 0 : HitToolbar(x, y);
            if (cmd != hoverCmd_) {
                hoverCmd_ = cmd;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (draggingToolbar_) {
                POINT pt{ x, y };
                ClientToScreen(hwnd, &pt);
                int dx = pt.x - dragStart_.x, dy = pt.y - dragStart_.y;
                if (std::abs(pt.x - gripStart_.x) > 3 || std::abs(pt.y - gripStart_.y) > 3)
                    gripMoved_ = true;
                RECT wr{}, work{};
                GetWindowRect(hwnd, &wr);
                POINT cur{};
                GetCursorPos(&cur);
                HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi{}; mi.cbSize = sizeof(mi);
                if (mon && GetMonitorInfoW(mon, &mi)) work = mi.rcWork;
                else work = ToolbarWindowWorkArea();
                int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
                int nx = dragOrigin_.left + dx;
                int ny = dragOrigin_.top + dy;
                nx = std::max((int)work.left, std::min(nx, (int)work.right - ww));
                ny = std::max((int)work.top, std::min(ny, (int)work.bottom - wh));
                SetWindowPos(hwnd, HWND_TOPMOST, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            } else {
                bool grip = (cmd == FS_GRIP) || collapsed_;
                SetCursor(LoadCursorW(nullptr, grip ? IDC_SIZEALL : (cmd ? IDC_HAND : IDC_ARROW)));
            }
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            hoverCmd_ = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_RBUTTONUP: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            int cmd = HitToolbar(x, y);
            if (cmd == FS_PEN || cmd == FS_ERASER) ShowSizePopup(cmd);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            int cmd = HitToolbar(x, y);
            if (cmd && cmd != FS_GRIP) {
                ActivateToolbarCommand(cmd);
                return 0;
            }
            draggingToolbar_ = true;
            gripPressed_ = collapsed_;
            gripMoved_ = false;
            dragStart_ = { x, y };
            ClientToScreen(hwnd, &dragStart_);
            gripStart_ = dragStart_;
            GetWindowRect(hwnd, &dragOrigin_);
            SetCapture(hwnd);
            return 0;
        }
        case WM_LBUTTONUP:
            if (draggingToolbar_) {
                draggingToolbar_ = false;
                bool toggle = gripPressed_ && !gripMoved_;
                gripPressed_ = false;
                gripMoved_ = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                if (toggle) ToggleCollapsed();
            }
            return 0;
        case WM_CAPTURECHANGED:
            if ((HWND)lp != hwnd) {
                draggingToolbar_ = false;
                gripPressed_ = false;
                gripMoved_ = false;
            }
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                int cmd = HitToolbar(pt.x, pt.y);
                bool grip = (cmd == FS_GRIP) || collapsed_;
                SetCursor(LoadCursorW(nullptr, grip ? IDC_SIZEALL : (cmd ? IDC_HAND : IDC_ARROW)));
                return TRUE;
            }
            break;
        case WM_MOUSEACTIVATE:
            return MA_ACTIVATE;
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) { RequestExit(); return 0; }
            if (wp == VK_DELETE || wp == VK_BACK) { DeleteSelection(); return 0; }
            break;
        case WM_FS_EXIT:
            RequestExit();
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static LRESULT CALLBACK HookProc(int code, WPARAM wp, LPARAM lp) {
        if (code == HC_ACTION && hookOwner_ && hookOwner_->visible_ && hookOwner_->hotkeysEnabled_ &&
            (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
            auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
            DWORD vk = kb ? kb->vkCode : 0;
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            HWND fg = GetForegroundWindow();
            wchar_t cls[64]{};
            if (fg) GetClassNameW(fg, cls, 64);
            bool modalOwned = _wcsicmp(cls, L"LitheMdSettings") == 0 ||
                              _wcsicmp(cls, L"LitheMdColorPicker") == 0 ||
                              _wcsicmp(cls, L"LitheMdShortcuts") == 0 ||
                              _wcsicmp(cls, kSizePopupClass) == 0;
            bool ours = _wcsicmp(cls, kOverlayClass) == 0 ||
                        _wcsicmp(cls, kToolbarClass) == 0 ||
                        _wcsicmp(cls, L"LitheMdMain") == 0;
            if (vk == VK_ESCAPE) {
                if (modalOwned) return CallNextHookEx(hook_, code, wp, lp);
                if (hookOwner_->surfaces_.empty())
                    hookOwner_->RequestExit();
                else
                    PostMessageW(hookOwner_->surfaces_[0].hwnd, WM_FS_EXIT, 0, 0);
                return 1;
            }
            if (!ours) return CallNextHookEx(hook_, code, wp, lp);
            if (ctrl && (vk == 'Z' || vk == 'Y')) {
                if (vk == 'Z') hookOwner_->Undo();
                else hookOwner_->Redo();
                hookOwner_->InvalidateToolbar();
                return 1;
            }
            if (ctrl && (vk == 'C' || vk == 'V' || vk == 'A' || vk == 'D')) {
                if (vk == 'C') hookOwner_->CopySelection();
                else if (vk == 'V') hookOwner_->PasteSelection();
                else if (vk == 'A') hookOwner_->SelectAll();
                else {
                    hookOwner_->CopySelection();
                    hookOwner_->PasteSelection();
                }
                return 1;
            }
            if (vk == VK_DELETE || vk == VK_BACK) {
                hookOwner_->DeleteSelection();
                return 1;
            }
            if (hookOwner_->tool_ == FS_SELECT && !hookOwner_->selection_.empty() &&
                (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN)) {
                bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                int step = shift ? 10 : 1;
                int dx = vk == VK_LEFT ? -step : (vk == VK_RIGHT ? step : 0);
                int dy = vk == VK_UP ? -step : (vk == VK_DOWN ? step : 0);
                hookOwner_->MoveSelectionBy(dx, dy);
                return 1;
            }
        }
        return CallNextHookEx(hook_, code, wp, lp);
    }
};

HHOOK FullscreenInkOverlay::Impl::hook_ = nullptr;
FullscreenInkOverlay::Impl* FullscreenInkOverlay::Impl::hookOwner_ = nullptr;

FullscreenInkOverlay::FullscreenInkOverlay(HWND owner, Settings* settings, Callbacks callbacks)
    : impl_(std::make_unique<Impl>()) {
    impl_->owner_ = owner;
    impl_->settings_ = settings;
    impl_->cb_ = std::move(callbacks);
    impl_->RegisterClasses();
}

FullscreenInkOverlay::~FullscreenInkOverlay() {
    if (impl_) impl_->Hide();
}

bool FullscreenInkOverlay::Show() {
    if (!impl_) return false;
    impl_->Show();
    return impl_->visible_;
}

void FullscreenInkOverlay::Hide() {
    if (impl_) impl_->Hide();
}

bool FullscreenInkOverlay::visible() const {
    return impl_ && impl_->visible_;
}

HWND FullscreenInkOverlay::toolbarHwnd() const {
    return impl_ ? impl_->toolbar_ : nullptr;
}

void FullscreenInkOverlay::ApplySettings() {
    if (impl_) impl_->ApplySettings();
}

void FullscreenInkOverlay::UsePalettePreset(int presetIndex) {
    if (impl_) impl_->ApplyPalettePreset(presetIndex);
}

void FullscreenInkOverlay::SetHotkeysEnabled(bool enabled) {
    if (impl_) impl_->hotkeysEnabled_ = enabled ? 1 : 0;
}

void FullscreenInkOverlay::SetTool(int tool) {
    if (impl_) impl_->SetTool(tool);
}
