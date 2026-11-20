#include "colorpicker.h"
#include "render.h"
#include "i18n.h"
#include <algorithm>
#include <cmath>
#include <windowsx.h>

namespace {

int ToByte(float v) {
    return (int)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f);
}

bool ParseHexColor(const std::wstring& text, int& r, int& g, int& b, int& a) {
    std::wstring s = Trim(text);
    if (!s.empty() && s[0] == L'#') s.erase(s.begin());
    if (s.size() != 6 && s.size() != 8) return false;
    auto nib = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    unsigned v = 0;
    for (wchar_t c : s) {
        int n = nib(c);
        if (n < 0) return false;
        v = (v << 4) | (unsigned)n;
    }
    if (s.size() == 8) {
        a = (int)((v >> 24) & 0xFF);
        r = (int)((v >> 16) & 0xFF);
        g = (int)((v >> 8) & 0xFF);
        b = (int)(v & 0xFF);
    } else {
        a = 255;
        r = (int)((v >> 16) & 0xFF);
        g = (int)((v >> 8) & 0xFF);
        b = (int)(v & 0xFF);
    }
    return true;
}

} // namespace

ColorPickerWindow::ColorPickerWindow(HWND owner, Settings* settings, int slot, Color initial,
                                     ApplyFn onApply)
    : owner_(owner), settings_(settings), slot_(std::clamp(slot, 0, 1)),
      onApply_(std::move(onApply)) {
    r_ = ToByte(initial.r);
    g_ = ToByte(initial.g);
    b_ = ToByte(initial.b);
    a_ = ToByte(initial.a);
}

ColorPickerWindow::~ColorPickerWindow() = default;

Color ColorPickerWindow::Current() const {
    return Color(r_ / 255.0f, g_ / 255.0f, b_ / 255.0f, a_ / 255.0f);
}

bool ColorPickerWindow::Create() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"OpenMdColorPicker";
        RegisterClassExW(&wc);
        registered = true;
    }
    UINT dpi = GetDpiForWindow(owner_);
    if (!dpi) dpi = 96;
    float sc = dpi / 96.0f;
    RECT rc{ 0, 0, (LONG)std::lround(640 * sc), (LONG)std::lround(420 * sc) };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    RECT orc{};
    GetWindowRect(owner_, &orc);
    int ox = orc.left + ((orc.right - orc.left) - ww) / 2;
    int oy = orc.top + ((orc.bottom - orc.top) - wh) / 2;
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        ox = std::max((int)work.left + 8, std::min(ox, (int)work.right - ww - 8));
        oy = std::max((int)work.top + 8, std::min(oy, (int)work.bottom - wh - 8));
    }
    EnableWindow(owner_, FALSE);
    hwnd_ = CreateWindowExW(0, L"OpenMdColorPicker", TranslateUi(L"自定义颜色").c_str(), style, ox, oy, ww, wh,
                            owner_, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        EnableWindow(owner_, TRUE);
        return false;
    }
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    return true;
}

LRESULT CALLBACK ColorPickerWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ColorPickerWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<ColorPickerWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = reinterpret_cast<ColorPickerWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->Handle(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ColorPickerWindow::SyncEdits() {
    if (!hwnd_) return;
    updating_ = true;
    auto set = [&](int i, const std::wstring& text) {
        if (edits_[i]) SetWindowTextW(edits_[i], text.c_str());
    };
    set(0, std::to_wstring(r_));
    set(1, std::to_wstring(g_));
    set(2, std::to_wstring(b_));
    set(3, std::to_wstring(a_));
    set(4, ColorToHex(Current()));
    updating_ = false;
}

void ColorPickerWindow::LayoutEdits() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    UINT dpi = GetDpiForWindow(hwnd_);
    if (!dpi) dpi = 96;
    float sc = dpi / 96.0f;
    float w = rc.right / sc;
    float rightX = std::max(238.0f, w - 360.0f);
    for (int i = 0; i < 4; i++) {
        int x = (int)std::lround((rightX + 252) * sc);
        int y = (int)std::lround((74 + i * 42) * sc);
        int cw = (int)std::lround(66 * sc);
        int ch = (int)std::lround(24 * sc);
        if (edits_[i]) SetWindowPos(edits_[i], nullptr, x, y, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (edits_[4])
        SetWindowPos(edits_[4], nullptr, (int)std::lround((rightX + 52) * sc),
                     (int)std::lround(274 * sc), (int)std::lround(170 * sc),
                     (int)std::lround(25 * sc), SWP_NOZORDER | SWP_NOACTIVATE);
}

void ColorPickerWindow::SetChannel(int channel, int value) {
    value = std::clamp(value, 0, 255);
    if (channel == 0) r_ = value;
    else if (channel == 1) g_ = value;
    else if (channel == 2) b_ = value;
    else a_ = value;
    SyncEdits();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ColorPickerWindow::ParseHexEdit() {
    if (updating_ || !edits_[4]) return;
    wchar_t buf[64]{};
    GetWindowTextW(edits_[4], buf, 64);
    int r = r_, g = g_, b = b_, a = a_;
    if (!ParseHexColor(buf, r, g, b, a)) return;
    r_ = r; g_ = g; b_ = b; a_ = a;
    SyncEdits();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ColorPickerWindow::Paint() {
    if (!hwnd_) return;
    if (!canvas_) canvas_ = std::make_unique<Canvas>();
    Canvas& c = *canvas_;
    bool dark = settings_ && settings_->dark;
    if (!c.Begin(hwnd_, Palette::Get(dark))) return;
    c.SetTranslateUi(true);
    auto& p = c.palette();
    float w = c.widthDip(), h = c.heightDip();
    c.FillRect(0, 0, w, h, p.bg);
    c.DrawText(L"自定义颜色", 18, 12, w - 36, 26, 17, p.fg, true, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);

    previewRect_ = { 18, 52, 180, 120 };
    c.FillRound(previewRect_.x, previewRect_.y, previewRect_.w, previewRect_.h, 7, p.surface);
    c.StrokeRound(previewRect_.x, previewRect_.y, previewRect_.w, previewRect_.h, 7, p.border);
    // Checkerboard behind the color so alpha is visible.
    for (int yy = 0; yy < 120; yy += 10)
        for (int xx = 0; xx < 180; xx += 10) {
            Color q = ((xx / 10 + yy / 10) & 1) ? Color(0.82f, 0.82f, 0.82f) : Color(0.95f, 0.95f, 0.95f);
            c.FillRect(previewRect_.x + xx, previewRect_.y + yy, 10, 10, q);
        }
    Color cur = Current();
    c.FillRect(previewRect_.x, previewRect_.y, previewRect_.w, previewRect_.h, cur);

    c.DrawText(L"自选颜色槽", 18, 188, 220, 20, 11.5f, p.muted, false, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    for (int i = 0; i < 2; i++) {
        slotRects_[i] = { 18.0f + i * 96.0f, 210, 86, 42 };
        bool sel = slot_ == i;
        c.FillRound(slotRects_[i].x, slotRects_[i].y, slotRects_[i].w, slotRects_[i].h, 6,
                    sel ? p.accentSoft : p.surface);
        c.StrokeRound(slotRects_[i].x, slotRects_[i].y, slotRects_[i].w, slotRects_[i].h, 6,
                      sel ? p.accent : p.border);
        Color sc = settings_ ? settings_->customColors[i] : cur;
        c.FillCircle(slotRects_[i].x + 20, slotRects_[i].y + 21, 12, sc);
        c.DrawText(L"自选 " + std::to_wstring(i + 1), slotRects_[i].x + 38, slotRects_[i].y,
                   slotRects_[i].w - 42, slotRects_[i].h, 11.0f, p.fg, sel, false,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    }

    float rightX = std::max(238.0f, w - 360.0f);
    const wchar_t* labels[4] = { L"R", L"G", L"B", L"A" };
    int values[4] = { r_, g_, b_, a_ };
    for (int i = 0; i < 4; i++) {
        float y = 74 + i * 42;
        c.DrawText(labels[i], rightX, y, 18, 24, 12, p.fg, true, false,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        sliderRects_[i] = { rightX + 28, y + 6, 210, 12 };
        c.FillRound(sliderRects_[i].x, sliderRects_[i].y, sliderRects_[i].w, sliderRects_[i].h,
                    5, p.border);
        float t = values[i] / 255.0f;
        c.FillRound(sliderRects_[i].x, sliderRects_[i].y, sliderRects_[i].w * t,
                    sliderRects_[i].h, 5, i == 3 ? p.muted : p.accent);
        float thumbX = sliderRects_[i].x + sliderRects_[i].w * t;
        c.FillCircle(thumbX, sliderRects_[i].y + 6, 6, p.fg);
    }
    c.DrawText(L"十六进制", rightX, 278, 60, 20, 11.0f, p.fg, false, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    c.DrawText(L"支持 #RRGGBB / #AARRGGBB", rightX + 54, 300, 240, 18, 10.0f, p.muted, false, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);

    applyRect_ = { w - 300, h - 52, 140, 34 };
    cancelRect_ = { w - 150, h - 52, 132, 34 };
    c.FillRound(applyRect_.x, applyRect_.y, applyRect_.w, applyRect_.h, 7, p.accent);
    c.DrawText(L"使用并保存", applyRect_.x, applyRect_.y, applyRect_.w, applyRect_.h, 12,
               Color(1, 1, 1, 1), true, false, DWRITE_TEXT_ALIGNMENT_CENTER,
               DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    c.FillRound(cancelRect_.x, cancelRect_.y, cancelRect_.w, cancelRect_.h, 7, p.surface);
    c.StrokeRound(cancelRect_.x, cancelRect_.y, cancelRect_.w, cancelRect_.h, 7, p.border);
    c.DrawText(L"取消", cancelRect_.x, cancelRect_.y, cancelRect_.w, cancelRect_.h, 12,
               p.fg, false, false, DWRITE_TEXT_ALIGNMENT_CENTER,
               DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    c.End();
    LayoutEdits();
}

void ColorPickerWindow::Apply() {
    if (onApply_) onApply_(slot_, Current());
    Close();
}

void ColorPickerWindow::Close() {
    DestroyWindow(hwnd_);
}

LRESULT ColorPickerWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        for (int i = 0; i < 5; i++) {
            DWORD style = WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL;
            if (i < 4) style |= ES_NUMBER;
            edits_[i] = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", style, 0, 0, 60, 24,
                                        hwnd_, (HMENU)(INT_PTR)(2001 + i), GetModuleHandleW(nullptr), nullptr);
            if (edits_[i]) SendMessageW(edits_[i], WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        }
        SyncEdits();
        return 0;
    }
    case WM_SIZE:
        LayoutEdits();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        Paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (HIWORD(wp) == EN_CHANGE && !updating_) {
            if (id >= 2001 && id <= 2004) {
                wchar_t buf[32]{};
                GetWindowTextW(edits_[id - 2001], buf, 32);
                wchar_t* end = nullptr;
                long v = wcstol(buf, &end, 10);
                if (end && end != buf && v >= 0 && v <= 255)
                    SetChannel(id - 2001, (int)v);
            } else if (id == 2005) {
                ParseHexEdit();
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        float sc = GetDpiForWindow(hwnd_) / 96.0f;
        float x = GET_X_LPARAM(lp) / sc, y = GET_Y_LPARAM(lp) / sc;
        for (int i = 0; i < 2; i++) {
            if (slotRects_[i].Contains(x, y)) {
                slot_ = i;
                if (settings_) {
                    Color c = settings_->customColors[i];
                    r_ = ToByte(c.r); g_ = ToByte(c.g); b_ = ToByte(c.b); a_ = ToByte(c.a);
                }
                SyncEdits();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        for (int i = 0; i < 4; i++) {
            if (sliderRects_[i].Contains(x, y) || (y >= sliderRects_[i].y - 4 && y <= sliderRects_[i].y + 16 &&
                                                   x >= sliderRects_[i].x && x <= sliderRects_[i].x + sliderRects_[i].w)) {
                drag_ = i;
                SetCapture(hwnd_);
                float t = std::clamp((x - sliderRects_[i].x) / sliderRects_[i].w, 0.0f, 1.0f);
                SetChannel(i, (int)std::lround(t * 255.0f));
                return 0;
            }
        }
        if (applyRect_.Contains(x, y)) { Apply(); return 0; }
        if (cancelRect_.Contains(x, y)) { Close(); return 0; }
        break;
    }
    case WM_MOUSEMOVE:
        if (drag_ >= 0) {
            float sc = GetDpiForWindow(hwnd_) / 96.0f;
            float x = GET_X_LPARAM(lp) / sc;
            float t = std::clamp((x - sliderRects_[drag_].x) / sliderRects_[drag_].w, 0.0f, 1.0f);
            SetChannel(drag_, (int)std::lround(t * 255.0f));
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (drag_ >= 0) {
            drag_ = -1;
            if (GetCapture() == hwnd_) ReleaseCapture();
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { Close(); return 0; }
        if (wp == VK_RETURN) { Apply(); return 0; }
        break;
    case WM_CLOSE:
        Close();
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        if (owner_) {
            EnableWindow(owner_, TRUE);
            SetForegroundWindow(owner_);
        }
        delete this;
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
