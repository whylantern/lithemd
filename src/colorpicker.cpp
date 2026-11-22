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

void RgbToHsv(int r, int g, int b, int& hue, float& sat, float& val) {
    float rf = r / 255.0f, gf = g / 255.0f, bf = b / 255.0f;
    float mx = std::max(rf, std::max(gf, bf));
    float mn = std::min(rf, std::min(gf, bf));
    float delta = mx - mn;
    val = mx;
    sat = mx <= 0.0001f ? 0.0f : delta / mx;
    float h = 0.0f;
    if (delta > 0.0001f) {
        if (mx == rf) h = (gf - bf) / delta;
        else if (mx == gf) h = 2.0f + (bf - rf) / delta;
        else h = 4.0f + (rf - gf) / delta;
        h *= 60.0f;
        if (h < 0) h += 360.0f;
    }
    hue = (int)std::lround(h) % 360;
}

Color HsvToColor(int hue, float sat, float val, int alpha) {
    float h = (float)(hue % 360) / 60.0f;
    float c = val * sat;
    float x = c * (1.0f - std::abs(std::fmod(h, 2.0f) - 1.0f));
    float m = val - c;
    float rf = 0, gf = 0, bf = 0;
    int sector = (int)h;
    switch (sector) {
    case 0: rf = c; gf = x; break;
    case 1: rf = x; gf = c; break;
    case 2: gf = c; bf = x; break;
    case 3: gf = x; bf = c; break;
    case 4: rf = x; bf = c; break;
    default: rf = c; bf = x; break;
    }
    return Color(rf + m, gf + m, bf + m, alpha / 255.0f);
}

} // namespace

ColorPickerWindow::ColorPickerWindow(HWND owner, Settings* settings, int presetIndex, Color initial,
                                     ChangedFn onChanged, AppliedFn onApplied)
    : owner_(owner), settings_(settings), presetIndex_(presetIndex),
      onChanged_(std::move(onChanged)), onApplied_(std::move(onApplied)) {
    if (settings_ && settings_->inkPresets.empty())
        settings_->inkPresets = DefaultInkPresets();
    r_ = ToByte(initial.r);
    g_ = ToByte(initial.g);
    b_ = ToByte(initial.b);
    a_ = ToByte(initial.a);
    UpdateHsvFromRgb();
    if (presetIndex_ < 0 || !settings_ || presetIndex_ >= (int)settings_->inkPresets.size())
        presetIndex_ = 0;
}

ColorPickerWindow::~ColorPickerWindow() = default;

Color ColorPickerWindow::Current() const {
    return Color(r_ / 255.0f, g_ / 255.0f, b_ / 255.0f, a_ / 255.0f);
}

void ColorPickerWindow::UpdateHsvFromRgb() {
    RgbToHsv(r_, g_, b_, hue_, sat_, val_);
}

void ColorPickerWindow::SetFromHsv() {
    Color color = HsvToColor(hue_, sat_, val_, a_);
    r_ = ToByte(color.r);
    g_ = ToByte(color.g);
    b_ = ToByte(color.b);
    SyncEdits();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool ColorPickerWindow::Create() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"LitheMdColorPicker";
        RegisterClassExW(&wc);
        registered = true;
    }
    UINT dpi = GetDpiForWindow(owner_);
    if (!dpi) dpi = 96;
    float sc = dpi / 96.0f;
    RECT rc{ 0, 0, (LONG)std::lround(780 * sc), (LONG)std::lround(560 * sc) };
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
    hwnd_ = CreateWindowExW(0, L"LitheMdColorPicker", TranslateUi(L"调色盘").c_str(), style, ox, oy, ww, wh,
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
    int y = 326;
    for (int i = 0; i < 4; i++) {
        int x = 410 + i * 54;
        if (edits_[i]) SetWindowPos(edits_[i], nullptr,
            (int)std::lround(x * sc), (int)std::lround(y * sc),
            (int)std::lround(44 * sc), (int)std::lround(24 * sc), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (edits_[4])
        SetWindowPos(edits_[4], nullptr, (int)std::lround(410 * sc),
                     (int)std::lround(382 * sc), (int)std::lround(170 * sc),
                     (int)std::lround(25 * sc), SWP_NOZORDER | SWP_NOACTIVATE);
}

void ColorPickerWindow::SetChannel(int channel, int value) {
    value = std::clamp(value, 0, 255);
    if (channel == 0) r_ = value;
    else if (channel == 1) g_ = value;
    else if (channel == 2) b_ = value;
    else a_ = value;
    if (channel < 3) UpdateHsvFromRgb();
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
    UpdateHsvFromRgb();
    SyncEdits();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

bool ColorPickerWindow::ToolbarContains(int index) const {
    if (!settings_) return false;
    for (int value : settings_->inkToolbarPresets)
        if (value == index) return true;
    return false;
}

int ColorPickerWindow::ToolbarCount() const {
    if (!settings_) return 0;
    return (int)settings_->inkToolbarPresets.size();
}

void ColorPickerWindow::SelectPreset(int index) {
    if (!settings_ || index < 0 || index >= (int)settings_->inkPresets.size()) return;
    creatingNew_ = false;
    presetIndex_ = index;
    Color color = settings_->inkPresets[(size_t)index];
    r_ = ToByte(color.r); g_ = ToByte(color.g); b_ = ToByte(color.b); a_ = ToByte(color.a);
    UpdateHsvFromRgb();
    SyncEdits();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ColorPickerWindow::NewPreset() {
    if (!settings_ || settings_->inkPresets.size() >= 12) return;
    creatingNew_ = true;
    presetIndex_ = -1;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ColorPickerWindow::ResetPresets() {
    if (!settings_) return;
    settings_->inkPresets = DefaultInkPresets();
    settings_->inkToolbarPresets = { 0, 1, 2, 3, 4 };
    settings_->Save();
    presetIndex_ = 0;
    creatingNew_ = false;
    Color color = settings_->inkPresets[0];
    r_ = ToByte(color.r); g_ = ToByte(color.g); b_ = ToByte(color.b); a_ = ToByte(color.a);
    UpdateHsvFromRgb();
    SyncEdits();
    if (onChanged_) onChanged_();
    if (onApplied_) onApplied_(0);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ColorPickerWindow::ReplacePreset(int index) {
    if (!settings_ || index < 0 || index >= (int)settings_->inkPresets.size()) return;
    settings_->inkPresets[(size_t)index] = Current();
    settings_->Save();
    if (onChanged_) onChanged_();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ColorPickerWindow::ToggleToolbarPreset(int index) {
    if (!settings_ || index < 0 || index >= (int)settings_->inkPresets.size()) return;
    auto& slots = settings_->inkToolbarPresets;
    auto it = std::find(slots.begin(), slots.end(), index);
    if (it != slots.end()) {
        if (slots.size() > 1) slots.erase(it);
    } else if (slots.size() < 5) {
        slots.push_back(index);
    } else {
        return;
    }
    settings_->Save();
    if (onChanged_) onChanged_();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ColorPickerWindow::Apply() {
    Color color = Current();
    if (settings_) {
        if (creatingNew_ || presetIndex_ < 0 ||
            presetIndex_ >= (int)settings_->inkPresets.size()) {
            if (settings_->inkPresets.size() >= 12) return;
            settings_->inkPresets.push_back(color);
            presetIndex_ = (int)settings_->inkPresets.size() - 1;
        } else {
            settings_->inkPresets[(size_t)presetIndex_] = color;
        }
        creatingNew_ = false;
        settings_->Save();
    }
    if (onChanged_) onChanged_();
    if (onApplied_) onApplied_(presetIndex_);
    Close();
}

void ColorPickerWindow::Close() {
    DestroyWindow(hwnd_);
}

void ColorPickerWindow::Paint() {
    if (!hwnd_) return;
    if (!canvas_) canvas_ = std::make_unique<Canvas>();
    Canvas& c = *canvas_;
    if (!c.Begin(hwnd_, Palette::Get(settings_ ? settings_->theme : L"blue"))) return;
    c.SetTranslateUi(true);
    auto& p = c.palette();
    float w = c.widthDip(), h = c.heightDip();
    c.FillRect(0, 0, w, h, p.bg);
    c.DrawText(L"调色盘", 18, 12, w - 36, 26, 17, p.fg, true, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);

    c.DrawText(L"预设颜色", 18, 48, 200, 20, 11.5f, p.muted, false, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    const auto& presets = settings_ ? settings_->inkPresets : DefaultInkPresets();
    int count = std::min(12, (int)presets.size());
    for (int i = 0; i < 12; i++) {
        int col = i % 3, row = i / 3;
        DRect r{ 18.0f + col * 78.0f, 74.0f + row * 62.0f, 72, 56 };
        presetRects_[i] = r;
        pinRects_[i] = { r.x + r.w - 18, r.y + 4, 16, 16 };
        if (i >= count) continue;
        bool selected = !creatingNew_ && presetIndex_ == i;
        c.FillRound(r.x, r.y, r.w, r.h, 7, selected ? p.accentSoft : p.surface);
        c.StrokeRound(r.x, r.y, r.w, r.h, 7, selected ? p.accent : p.border);
        Color color = presets[(size_t)i];
        c.FillCircle(r.x + 22, r.y + 28, 12, color);
        c.StrokeRound(pinRects_[i].x, pinRects_[i].y, 16, 16, 4,
                      ToolbarContains(i) ? p.accent : p.border);
        if (ToolbarContains(i))
            c.DrawText(L"✓", pinRects_[i].x, pinRects_[i].y - 1, 16, 16, 10, p.accent, true, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        c.DrawText(std::to_wstring(i + 1), r.x + 40, r.y + 18, 24, 20, 11, p.muted, false, false,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    newRect_ = { 18, 334, 120, 32 };
    bool canNew = !settings_ || settings_->inkPresets.size() < 12;
    c.FillRound(newRect_.x, newRect_.y, newRect_.w, newRect_.h, 7, p.surface);
    c.StrokeRound(newRect_.x, newRect_.y, newRect_.w, newRect_.h, 7, canNew ? p.accent : p.border);
    c.DrawText(L"新建", newRect_.x, newRect_.y, newRect_.w, newRect_.h, 12,
               canNew ? p.fg : p.muted, false, false,
               DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    resetRect_ = { 146, 334, 120, 32 };
    c.FillRound(resetRect_.x, resetRect_.y, resetRect_.w, resetRect_.h, 7, p.surface);
    c.StrokeRound(resetRect_.x, resetRect_.y, resetRect_.w, resetRect_.h, 7, p.border);
    c.DrawText(L"恢复默认", resetRect_.x, resetRect_.y, resetRect_.w, resetRect_.h, 12,
               p.fg, false, false, DWRITE_TEXT_ALIGNMENT_CENTER,
               DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    c.DrawText(L"主栏颜色 " + std::to_wstring(ToolbarCount()) + L"/5", 18, 376, 200, 18, 10.5f,
               p.muted, false, false, DWRITE_TEXT_ALIGNMENT_LEADING,
               DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);

    // Saturation/value square.
    svRect_ = { 280, 74, 220, 180 };
    for (int yy = 0; yy < 45; yy++) {
        float v = 1.0f - yy / 44.0f;
        for (int xx = 0; xx < 55; xx++) {
            float s = xx / 54.0f;
            c.FillRect(svRect_.x + xx * 4, svRect_.y + yy * 4, 4, 4,
                       HsvToColor(hue_, s, v, 255));
        }
    }
    c.StrokeRound(svRect_.x, svRect_.y, svRect_.w, svRect_.h, 0, p.border);
    float cx = svRect_.x + sat_ * svRect_.w;
    float cy = svRect_.y + (1.0f - val_) * svRect_.h;
    c.FillCircle(cx, cy, 6, Color(1, 1, 1, 1));
    c.FillCircle(cx, cy, 4, Current());

    // Hue strip.
    hueRect_ = { 508, 74, 18, 180 };
    for (int i = 0; i < 60; i++) {
        int hue = i * 6;
        c.FillRect(hueRect_.x, hueRect_.y + i * 3, hueRect_.w, 3, HsvToColor(hue, 1, 1, 255));
    }
    c.StrokeRound(hueRect_.x, hueRect_.y, hueRect_.w, hueRect_.h, 0, p.border);
    float hy = hueRect_.y + (hue_ / 360.0f) * hueRect_.h;
    c.FillRect(hueRect_.x - 2, hy - 2, hueRect_.w + 4, 4, Color(1, 1, 1, 1));

    // Alpha slider with checkerboard.
    alphaRect_ = { 280, 272, 246, 14 };
    for (int i = 0; i < 82; i++) {
        Color q = (i & 1) ? Color(0.82f, 0.82f, 0.82f) : Color(0.95f, 0.95f, 0.95f);
        c.FillRect(alphaRect_.x + i * 3, alphaRect_.y, 3, alphaRect_.h, q);
    }
    for (int i = 0; i < 82; i++) {
        float t = i / 81.0f;
        Color color = Color(r_ / 255.0f, g_ / 255.0f, b_ / 255.0f, t);
        c.FillRect(alphaRect_.x + i * 3, alphaRect_.y, 3, alphaRect_.h, color);
    }
    c.StrokeRound(alphaRect_.x, alphaRect_.y, alphaRect_.w, alphaRect_.h, 4, p.border);
    float ax = alphaRect_.x + (a_ / 255.0f) * alphaRect_.w;
    c.FillCircle(ax, alphaRect_.y + 7, 6, p.fg);

    // Preview with checkerboard.
    previewRect_ = { 280, 300, 110, 66 };
    for (int yy = 0; yy < 66; yy += 8)
        for (int xx = 0; xx < 110; xx += 8) {
            Color q = ((xx / 8 + yy / 8) & 1) ? Color(0.82f, 0.82f, 0.82f) : Color(0.95f, 0.95f, 0.95f);
            c.FillRect(previewRect_.x + xx, previewRect_.y + yy, 8, 8, q);
        }
    c.FillRect(previewRect_.x, previewRect_.y, previewRect_.w, previewRect_.h, Current());
    c.StrokeRound(previewRect_.x, previewRect_.y, previewRect_.w, previewRect_.h, 6, p.border);

    const wchar_t* labels[4] = { L"R", L"G", L"B", L"A" };
    for (int i = 0; i < 4; i++)
        c.DrawText(labels[i], 410 + i * 54, 306, 44, 18, 11, p.fg, true, false,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    c.DrawText(L"透明度", 280, 250, 100, 18, 10.5f, p.muted, false, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    c.DrawText(L"十六进制", 410, 362, 100, 18, 11, p.fg, false, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);

    applyRect_ = { w - 286, h - 54, 130, 36 };
    cancelRect_ = { w - 144, h - 54, 126, 36 };
    c.FillRound(applyRect_.x, applyRect_.y, applyRect_.w, applyRect_.h, 7, p.accent);
    c.DrawText(L"确定", applyRect_.x, applyRect_.y, applyRect_.w, applyRect_.h, 12.5f,
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
        int count = settings_ ? (int)settings_->inkPresets.size() : 0;
        for (int i = 0; i < std::min(12, count); i++) {
            if (!presetRects_[i].Contains(x, y)) continue;
            if (pinRects_[i].Contains(x, y)) ToggleToolbarPreset(i);
            else SelectPreset(i);
            return 0;
        }
        if (newRect_.Contains(x, y)) { NewPreset(); return 0; }
        if (resetRect_.Contains(x, y)) { ResetPresets(); return 0; }
        if (svRect_.Contains(x, y)) {
            drag_ = 0;
            sat_ = std::clamp((x - svRect_.x) / svRect_.w, 0.0f, 1.0f);
            val_ = std::clamp(1.0f - (y - svRect_.y) / svRect_.h, 0.0f, 1.0f);
            SetCapture(hwnd_);
            SetFromHsv();
            return 0;
        }
        if (hueRect_.Contains(x, y)) {
            drag_ = 1;
            hue_ = (int)std::clamp((y - hueRect_.y) / hueRect_.h, 0.0f, 0.999f) * 360;
            SetCapture(hwnd_);
            SetFromHsv();
            return 0;
        }
        if (x >= alphaRect_.x - 4 && x <= alphaRect_.x + alphaRect_.w + 4 &&
            y >= alphaRect_.y - 5 && y <= alphaRect_.y + alphaRect_.h + 5) {
            drag_ = 2;
            SetCapture(hwnd_);
            SetChannel(3, (int)std::lround(std::clamp((x - alphaRect_.x) / alphaRect_.w, 0.0f, 1.0f) * 255.0f));
            return 0;
        }
        if (applyRect_.Contains(x, y)) { Apply(); return 0; }
        if (cancelRect_.Contains(x, y)) { Close(); return 0; }
        break;
    }
    case WM_RBUTTONUP: {
        float sc = GetDpiForWindow(hwnd_) / 96.0f;
        float x = GET_X_LPARAM(lp) / sc, y = GET_Y_LPARAM(lp) / sc;
        int count = settings_ ? (int)settings_->inkPresets.size() : 0;
        for (int i = 0; i < std::min(12, count); i++)
            if (presetRects_[i].Contains(x, y)) { ReplacePreset(i); return 0; }
        break;
    }
    case WM_MOUSEMOVE:
        if (drag_ >= 0) {
            float sc = GetDpiForWindow(hwnd_) / 96.0f;
            float x = GET_X_LPARAM(lp) / sc, y = GET_Y_LPARAM(lp) / sc;
            if (drag_ == 0) {
                sat_ = std::clamp((x - svRect_.x) / svRect_.w, 0.0f, 1.0f);
                val_ = std::clamp(1.0f - (y - svRect_.y) / svRect_.h, 0.0f, 1.0f);
                SetFromHsv();
            } else if (drag_ == 1) {
                hue_ = (int)(std::clamp((y - hueRect_.y) / hueRect_.h, 0.0f, 0.999f) * 360);
                SetFromHsv();
            } else {
                SetChannel(3, (int)std::lround(std::clamp((x - alphaRect_.x) / alphaRect_.w, 0.0f, 1.0f) * 255.0f));
            }
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
