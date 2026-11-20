#include "view.h"
#include "render.h"
#include <algorithm>
#include <cmath>
#include <windowsx.h>

namespace {
constexpr float kHeaderH = 46.0f;
constexpr float kRowH = 27.0f;

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
} // namespace

void OutlineView::Register(HINSTANCE inst) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"OpenMdOutline";
    RegisterClassExW(&wc);
}

bool OutlineView::Create(HWND parent, int id) {
    hwnd_ = CreateWindowExW(0, L"OpenMdOutline", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                            0, 0, 100, 100, parent, (HMENU)(INT_PTR)id,
                            (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), this);
    return hwnd_ != nullptr;
}

void OutlineView::SetItems(const std::vector<Heading>& items) {
    items_ = items;
    active_ = -1;
    hover_ = -1;
    scroll_ = 0;
    UpdateScroll();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void OutlineView::SetActive(int block) {
    if (active_ == block) return;
    active_ = block;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void OutlineView::ApplyTheme(bool dark) {
    dark_ = dark;
    ApplyDarkWindowTheme(hwnd_, dark);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void OutlineView::UpdateScroll() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    float scale = GetDpiForWindow(hwnd_) / 96.0f;
    float h = (rc.bottom - rc.top) / scale;
    float total = kHeaderH + items_.size() * kRowH + 24;
    int maxScroll = (int)std::max(0.0f, total - h);
    scroll_ = std::max(0, std::min(scroll_, maxScroll));
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = (int)std::max(total, h);
    si.nPage = (int)h;
    si.nPos = scroll_;
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
    ShowScrollBar(hwnd_, SB_VERT, maxScroll > 0);
}

void OutlineView::Paint() {
    if (!hwnd_) return;
    Canvas c;
    if (!c.Begin(hwnd_, Palette::Get(dark_))) return;
    auto& p = c.palette();
    c.FillRect(0, 0, c.widthDip(), c.heightDip(), p.bg);
    c.FillRect(0, 0, 1, c.heightDip(), p.border);
    c.DrawText(L"大纲", 16, 11, c.widthDip() - 32, 24, 13.5f, p.fg, true);
    float y = kHeaderH - scroll_;
    if (items_.empty()) {
        c.DrawText(L"本文档没有标题大纲", 16, kHeaderH, c.widthDip() - 32, 24, 11.5f, p.muted);
    } else {
        c.PushClip(0, kHeaderH - 2, c.widthDip(), c.heightDip() - kHeaderH + 2);
        for (size_t i = 0; i < items_.size(); i++) {
            auto& h = items_[(size_t)i];
            float rowY = y + i * kRowH;
            if (rowY + kRowH < kHeaderH - 2 || rowY > c.heightDip()) continue;
            float ind = 8.0f + std::max(0, h.level - 1) * 13.0f;
            if ((int)i == hover_) c.FillRound(6, rowY + 1, c.widthDip() - 12, kRowH - 2, 5, p.hover);
            if (h.block == active_) c.FillRound(6, rowY + 1, c.widthDip() - 12, kRowH - 2, 5, p.accentSoft);
            std::wstring text = FitText(c, h.text, 12.0f, h.block == active_, c.widthDip() - ind - 20);
            c.DrawText(text, ind, rowY, c.widthDip() - ind - 12, kRowH,
                       12.0f, h.block == active_ ? p.accent : p.fg, h.block == active_, false,
                       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        }
        c.PopClip();
    }
    c.End();
}

LRESULT CALLBACK OutlineView::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    OutlineView* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        self = (OutlineView*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = hwnd;
    } else {
        self = (OutlineView*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        self->UpdateScroll();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        self->Paint();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_VSCROLL: {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= (int)kRowH; break;
        case SB_LINEDOWN: pos += (int)kRowH; break;
        case SB_PAGEUP: pos -= (int)si.nPage; break;
        case SB_PAGEDOWN: pos += (int)si.nPage; break;
        case SB_THUMBTRACK: pos = si.nTrackPos; break;
        }
        self->scroll_ = std::max(0, pos);
        self->UpdateScroll();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        self->scroll_ -= delta / 120 * 64;
        if (self->scroll_ < 0) self->scroll_ = 0;
        self->UpdateScroll();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        float scale = GetDpiForWindow(hwnd) / 96.0f;
        int y = (int)(GET_Y_LPARAM(lp) / scale);
        int row = -1;
        if (y >= kHeaderH) row = (int)((y - kHeaderH + self->scroll_) / kRowH);
        if (row < 0 || row >= (int)self->items_.size()) row = -1;
        if (row != self->hover_) {
            self->hover_ = row;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        SetCursor(LoadCursorW(nullptr, row >= 0 ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE:
        self->hover_ = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        float scale = GetDpiForWindow(hwnd) / 96.0f;
        int y = (int)(GET_Y_LPARAM(lp) / scale);
        if (y >= kHeaderH) {
            int row = (int)((y - kHeaderH + self->scroll_) / kRowH);
            if (row >= 0 && row < (int)self->items_.size()) {
                int block = self->items_[(size_t)row].block;
                self->active_ = block;
                InvalidateRect(hwnd, nullptr, FALSE);
                if (self->callback_) self->callback_(block);
            }
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
