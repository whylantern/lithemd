#include "workspace_view.h"
#include "render.h"
#include "i18n.h"
#include <algorithm>
#include <cmath>
#include <windowsx.h>

namespace {
constexpr float kHeaderH = 46.0f;
constexpr float kRowH = 26.0f;
constexpr float kIndent = 14.0f;
constexpr float kIconW = 18.0f;

std::wstring FitText(Canvas& c, const std::wstring& text, float size, bool bold, float maxWidth) {
    if (maxWidth <= 8) return L"";
    if (c.MeasureText(text, size, bold, false, 100000.0f, false) <= maxWidth) return text;
    std::wstring t = text;
    while (!t.empty()) {
        t.pop_back();
        std::wstring cand = t + L"…";
        if (c.MeasureText(cand, size, bold, false, 100000.0f, false) <= maxWidth) return cand;
    }
    return L"…";
}
}

void WorkspaceView::Register(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"LitheMdWorkspace";
    RegisterClassExW(&wc);
}

bool WorkspaceView::Create(HWND parent, int id) {
    hwnd_ = CreateWindowExW(0, L"LitheMdWorkspace", L"", WS_CHILD | WS_CLIPCHILDREN,
                            0, 0, 0, 0, parent, (HMENU)(INT_PTR)id,
                            (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), this);
    return hwnd_ != nullptr;
}

void WorkspaceView::ApplyTheme(const std::wstring& theme) {
    theme_ = theme;
    dark_ = theme == L"dark";
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

bool WorkspaceView::OpenRoot(const std::wstring& path, std::wstring* error) {
    if (!model_.SetRoot(path, error)) return false;
    activePath_.clear();
    scroll_ = 0;
    rows_ = model_.Rows();
    EnsureScanTimer();
    if (hwnd_) { InvalidateRect(hwnd_, nullptr, FALSE); }
    return true;
}

void WorkspaceView::Refresh() {
    if (!model_.Valid()) return;
    model_.Refresh();
    rows_ = model_.Rows();
    EnsureScanTimer();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void WorkspaceView::EnsureScanTimer() {
    if (hwnd_ && model_.Scanning()) SetTimer(hwnd_, 1, 150, nullptr);
}

void WorkspaceView::SetCallbacks(std::function<void(const std::wstring&)> openFile,
                                 std::function<void()> closeView) {
    openFile_ = std::move(openFile);
    closeView_ = std::move(closeView);
}

void WorkspaceView::SetActivePath(const std::wstring& path, bool reveal) {
    activePath_ = path;
    if (reveal && model_.Valid() && !path.empty()) {
        model_.Reveal(path);
        rows_ = model_.Rows();
    }
    if (hwnd_) {
        // Keep the active row inside the visible area.
        int index = -1;
        for (size_t i = 0; i < rows_.size(); i++)
            if (_wcsicmp(rows_[i].path.c_str(), path.c_str()) == 0) { index = (int)i; break; }
        if (index >= 0) {
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            float h = (float)rc.bottom / (GetDpiForWindow(hwnd_) / 96.0f);
            viewH_ = std::max(0.0f, h - kHeaderH);
            float y = index * kRowH;
            if (y < scroll_) scroll_ = y;
            if (y + kRowH > scroll_ + viewH_) scroll_ = y + kRowH - viewH_;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

DRect WorkspaceView::RowRect(int index) const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    float scale = GetDpiForWindow(hwnd_) / 96.0f;
    float w = rc.right / scale;
    return { 0, kHeaderH + index * kRowH - scroll_, w, kRowH };
}

int WorkspaceView::HitRow(float x, float y) const {
    if (y < kHeaderH) return -1;
    int index = (int)std::floor((y - kHeaderH + scroll_) / kRowH);
    if (index < 0 || index >= (int)rows_.size()) return -1;
    return index;
}

float WorkspaceView::ContentHeight() const {
    return rows_.size() * kRowH + 8.0f;
}

void WorkspaceView::UpdateScroll() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    float scale = GetDpiForWindow(hwnd_) / 96.0f;
    float h = rc.bottom / scale;
    viewH_ = std::max(0.0f, h - kHeaderH);
    contentH_ = ContentHeight();
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, contentH_ - viewH_));
}

void WorkspaceView::Paint() {
    Canvas c;
    if (!c.Begin(hwnd_, Palette::Get(theme_), false)) return;
    c.SetTranslateUi(true);
    auto& p = c.palette();
    float w = c.widthDip(), h = c.heightDip();
    c.FillRect(0, 0, w, h, p.surface);
    c.FillRect(w - 1, 0, 1, h, p.border);
    rows_ = model_.Rows();
    UpdateScroll();

    c.DrawText(L"工作区", 12, 5, w - 60, 18, 10.5f, p.muted, true);
    std::wstring rootName = model_.Valid() ? model_.RootName() : L"未打开文件夹";
    if (model_.BudgetExceeded()) rootName += L" · 已暂停";
    else if (model_.Scanning()) rootName += L" · 加载中";
    c.DrawText(FitText(c, rootName, 12.0f, true, w - 74), 12, 21, w - 74, 20, 12.0f, p.fg, true, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    DRect refresh{ w - 50, 12, 22, 22 }, close{ w - 27, 12, 22, 22 };
    c.DrawText(L"↻", refresh.x, refresh.y, refresh.w, refresh.h, 14.0f, p.muted, false, false,
               DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    c.DrawText(L"×", close.x, close.y, close.w, close.h, 16.0f, p.muted, false, false,
               DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    c.FillRect(0, kHeaderH - 1, w, 1, p.border);

    c.PushClip(0, kHeaderH, w, h - kHeaderH);
    for (size_t i = 0; i < rows_.size(); i++) {
        DRect r = RowRect((int)i);
        if (r.y + kRowH < kHeaderH || r.y > h) continue;
        bool active = !activePath_.empty() &&
                      _wcsicmp(rows_[i].path.c_str(), activePath_.c_str()) == 0;
        if (active) c.FillRound(4, r.y + 1, w - 12, kRowH - 2, 4, p.accentSoft);
        else if (hover_ == (int)i) c.FillRound(4, r.y + 1, w - 12, kRowH - 2, 4, p.hover);
        float x = 8 + rows_[i].depth * kIndent;
        if (rows_[i].loading) {
            c.DrawText(L"正在加载…", x, r.y, w - x - 12, kRowH, 10.5f, p.muted, false, false,
                       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        } else if (rows_[i].isDir) {
            c.DrawText(rows_[i].expanded ? L"▾" : L"▸", x, r.y, 14, kRowH, 10.0f,
                       active ? p.accent : p.muted, false, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
            x += kIconW;
            c.DrawText(FitText(c, rows_[i].name, 11.5f, false, w - x - 12), x, r.y,
                       w - x - 12, kRowH, 11.5f, active ? p.accent : p.fg, false, false,
                       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        } else {
            c.DrawText(L"·", x, r.y, 14, kRowH, 12.0f, p.muted, false, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
            x += kIconW;
            std::wstring ext = PathExt(rows_[i].name);
            bool archive = ext == L".zip" || ext == L".cbz";
            c.DrawText(FitText(c, rows_[i].name, 11.5f, false, w - x - 12), x, r.y,
                       w - x - 12, kRowH, 11.5f,
                       active ? p.accent : (archive ? p.accent : p.fg), false, false,
                       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        }
    }
    c.PopClip();

    if (contentH_ > viewH_ + 0.5f) {
        float trackX = w - 4;
        float thumbH = std::max(28.0f, viewH_ * viewH_ / contentH_);
        float maxScroll = std::max(0.0f, contentH_ - viewH_);
        float thumbY = kHeaderH + (maxScroll > 0 ? scroll_ / maxScroll * (viewH_ - thumbH) : 0);
        Color track = p.border; track.a = 0.4f;
        c.FillRound(trackX, kHeaderH, 2, viewH_, 1, track);
        Color thumb = p.muted; thumb.a = 0.55f;
        c.FillRound(trackX, thumbY, 2, thumbH, 1, thumb);
    }
    c.End();
}

LRESULT WorkspaceView::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        Paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_SIZE:
        UpdateScroll();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        float scale = GetDpiForWindow(hwnd_) / 96.0f;
        float x = GET_X_LPARAM(lp) / scale, y = GET_Y_LPARAM(lp) / scale;
        int hover = HitRow(x, y);
        if (hover != hover_) { hover_ = hover; InvalidateRect(hwnd_, nullptr, FALSE); }
        bool hand = hover >= 0 || (y < kHeaderH && x > 0);
        SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        hover_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        float scale = GetDpiForWindow(hwnd_) / 96.0f;
        float x = GET_X_LPARAM(lp) / scale, y = GET_Y_LPARAM(lp) / scale;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        float w = rc.right / scale;
        if (y < kHeaderH) {
            if (x >= w - 30 && closeView_) closeView_();
            else if (x >= w - 56) Refresh();
            return 0;
        }
        int index = HitRow(x, y);
        if (index < 0) return 0;
        const WorkspaceRow row = rows_[(size_t)index];
        if (row.loading) return 0;
        if (row.isDir) {
            model_.Toggle(row.path);
            rows_ = model_.Rows();
            EnsureScanTimer();
            UpdateScroll();
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (openFile_) {
            openFile_(row.path);
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {
            bool changed = model_.ApplyCache();
            if (changed) {
                rows_ = model_.Rows();
                UpdateScroll();
            }
            if (!model_.Scanning()) {
                LASTINPUTINFO input{ sizeof(input) };
                bool idle = GetLastInputInfo(&input) &&
                            (GetTickCount() - input.dwTime) >= 2000;
                if (idle && !model_.BudgetExceeded() && model_.ContinueIdle())
                    changed = true;
                else if (idle && !model_.Scanning())
                    KillTimer(hwnd_, 1);
            }
            if (changed || model_.Scanning()) InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_MOUSEWHEEL: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd_, &pt);
        float scale = GetDpiForWindow(hwnd_) / 96.0f;
        float x = pt.x / scale, y = pt.y / scale;
        if (y < kHeaderH) return 0;
        UpdateScroll();
        float delta = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -kRowH * 3 : kRowH * 3;
        scroll_ = std::clamp(scroll_ + delta, 0.0f, std::max(0.0f, contentH_ - viewH_));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

LRESULT CALLBACK WorkspaceView::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WorkspaceView* self = (WorkspaceView*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        self = (WorkspaceView*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->Handle(msg, wp, lp);
}
