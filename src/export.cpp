#include "export.h"
#include "view.h"
#include "i18n.h"
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <commdlg.h>
#include <windowsx.h>

namespace {

constexpr int kClientW = 900;
constexpr int kClientH = 560;

std::wstring SafeFileName(const std::wstring& title) {
    std::wstring out = title.empty() ? L"导出图片" : title;
    for (wchar_t& c : out) {
        if (wcschr(L"\\/:*?\"<>|", c) || c < 32) c = L'_';
    }
    if (out.empty()) out = L"导出图片";
    return out + L".png";
}

// Rendering the whole document into the preview is costly for very large
// files. Skip the preview instead of freezing the dialog for a long time.
constexpr uint64_t kPreviewMaxBytes = 64ull * 1024 * 1024;
constexpr float kPreviewMaxHeightDip = 120000.0f;
constexpr size_t kPreviewMaxBlocks = 20000;
constexpr int kPreviewMaxPdfPages = 400;
constexpr DWORD kPreviewMaxMs = 1500;

bool PreviewTooLarge(const Document* doc, int contentHeightDip) {
    if (!doc) return false;
    // A single image is cheap to draw even when the file itself is large.
    if (doc->kind == DocKind::Image) return false;
    if (doc->size > kPreviewMaxBytes) return true;
    if ((float)contentHeightDip > kPreviewMaxHeightDip) return true;
    if (doc->blocks.size() > kPreviewMaxBlocks) return true;
    if (doc->kind == DocKind::Pdf && doc->pdfPages > kPreviewMaxPdfPages) return true;
    return false;
}

} // namespace

ExportWindow::ExportWindow(DocView* view, Settings* settings) : view_(view), settings_(settings) {}

ExportWindow::~ExportWindow() = default;

bool ExportWindow::Create(HWND owner) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"OpenMdExport";
        RegisterClassExW(&wc);
        registered = true;
    }
    owner_ = owner;
    if (!view_ || !settings_) return false;
    viewW_ = (int)std::lround(view_->ViewWidthDip());
    viewH_ = (int)std::lround(view_->ContentHeightDip());
    if (viewW_ < 1 || viewH_ < 1) return false;
    previewTooLarge_ = PreviewTooLarge(view_->Doc(), viewH_);
    outWidth_ = std::max(64, viewW_ * 2);
    // Keep the default preview/export within the hard pixel limits even for
    // long documents. The user can still lower the width manually.
    int maxWidthForHeight = (int)std::floor(20000.0 * viewW_ / std::max(1, viewH_));
    if (maxWidthForHeight > 0) outWidth_ = std::min(outWidth_, maxWidthForHeight);
    outWidth_ = std::max(64, outWidth_);
    outHeight_ = std::max(1, (int)std::lround((double)outWidth_ * viewH_ / viewW_));

    UINT dpi = GetDpiForWindow(owner);
    if (!dpi) dpi = 96;
    float sc = dpi / 96.0f;
    RECT rc{ 0, 0, (LONG)std::lround(kClientW * sc), (LONG)std::lround(kClientH * sc) };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        ww = std::min(ww, (int)(work.right - work.left - 24));
        wh = std::min(wh, (int)(work.bottom - work.top - 24));
    }
    RECT orc{};
    GetWindowRect(owner, &orc);
    int ox = orc.left + ((orc.right - orc.left) - ww) / 2;
    int oy = orc.top + ((orc.bottom - orc.top) - wh) / 2;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        ox = std::max((int)work.left + 8, std::min(ox, (int)work.right - ww - 8));
        oy = std::max((int)work.top + 8, std::min(oy, (int)work.bottom - wh - 8));
    }
    EnableWindow(owner, FALSE);
    hwnd_ = CreateWindowExW(0, L"OpenMdExport", TranslateUi(L"导出位图").c_str(), style, ox, oy, ww, wh,
                            owner, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
        EnableWindow(owner, TRUE);
        return false;
    }
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    return true;
}

LRESULT CALLBACK ExportWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ExportWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<ExportWindow*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = reinterpret_cast<ExportWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->Handle(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ExportWindow::LayoutEdit() {
    if (!hwnd_ || !widthEdit_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    UINT dpi = GetDpiForWindow(hwnd_);
    if (!dpi) dpi = 96;
    float sc = dpi / 96.0f;
    float w = rc.right / sc;
    float previewW = std::min(500.0f, std::max(260.0f, w - 350.0f));
    float rightX = 16 + previewW + 16;
    int x = (int)std::lround((rightX + 132) * sc);
    int y = (int)std::lround(104 * sc);
    int cw = (int)std::lround(90 * sc);
    int ch = (int)std::lround(25 * sc);
    SetWindowPos(widthEdit_, nullptr, x, y, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
}

void ExportWindow::SchedulePreview() {
    if (!hwnd_) return;
    KillTimer(hwnd_, 1);
    SetTimer(hwnd_, 1, 220, nullptr);
}

void ExportWindow::UpdatePreview() {
    if (!view_ || viewW_ <= 0 || viewH_ <= 0) return;
    if (previewTooLarge_) {
        if (hasPreview_) {
            hasPreview_ = false;
            previewPng_.clear();
            previewImage_.reset();
            if (canvas_) canvas_->ClearImageCache();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    UINT dpi = GetDpiForWindow(hwnd_);
    if (!dpi) dpi = 96;
    float sc = dpi / 96.0f;
    float clientW = rc.right / sc;
    float clientH = rc.bottom / sc;
    float previewW = std::min(500.0f, std::max(260.0f, clientW - 350.0f));
    float paneW = std::max(64.0f, previewW - 24.0f);
    float paneH = std::max(64.0f, clientH - 98.0f);
    float scale = std::min(paneW / (float)viewW_, paneH / (float)viewH_);
    scale = std::clamp(scale, 0.01f, 4.0f);
    std::vector<uint8_t> png;
    std::wstring err;
    int ow = 0, oh = 0;
    DWORD started = GetTickCount();
    if (!view_->ExportPng(scale, png, &err, &ow, &oh)) {
        hasPreview_ = false;
        notice_ = err.empty() ? L"无法生成预览" : err;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (GetTickCount() - started > kPreviewMaxMs) {
        previewTooLarge_ = true;
        hasPreview_ = false;
        previewPng_.clear();
        previewImage_.reset();
        if (canvas_) canvas_->ClearImageCache();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    previewPng_ = std::move(png);
    previewImage_ = std::make_shared<ImageRef>();
    previewImage_->bytes = previewPng_;
    previewImage_->mime = L"image/png";
    previewImage_->cacheKey = L"export-preview-" + std::to_wstring(++previewSeq_);
    if (canvas_) canvas_->ClearImageCache();
    hasPreview_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ExportWindow::Paint() {
    if (!hwnd_) return;
    if (!canvas_) canvas_ = std::make_unique<Canvas>();
    Canvas& c = *canvas_;
    if (!c.Begin(hwnd_, Palette::Get(settings_->dark))) return;
    c.SetTranslateUi(true);
    auto& p = c.palette();
    float w = c.widthDip(), h = c.heightDip();
    c.FillRect(0, 0, w, h, p.bg);
    c.DrawText(L"导出位图", 16, 12, w - 32, 28, 18, p.fg, true, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);

    float previewW = std::min(500.0f, std::max(260.0f, w - 350.0f));
    previewRect_ = { 16, 66, previewW, std::max(120.0f, h - 82) };
    c.FillRound(previewRect_.x, previewRect_.y, previewRect_.w, previewRect_.h, 8, p.surface);
    c.StrokeRound(previewRect_.x, previewRect_.y, previewRect_.w, previewRect_.h, 8, p.border);
    c.PushClip(previewRect_.x + 8, previewRect_.y + 8, previewRect_.w - 16, previewRect_.h - 16);
    if (hasPreview_ && previewImage_) {
        if (auto* bmp = c.Image(*previewImage_)) {
            float availW = previewRect_.w - 16, availH = previewRect_.h - 16;
            float iw = (float)viewW_, ih = (float)viewH_;
            float fit = std::min(availW / iw, availH / ih);
            float dw = iw * fit, dh = ih * fit;
            float dx = previewRect_.x + 8 + (availW - dw) / 2;
            float dy = previewRect_.y + 8 + (availH - dh) / 2;
            c.FillRect(dx, dy, dw, dh, p.codeBg);
            c.target()->DrawBitmap(bmp, D2D1::RectF(dx, dy, dx + dw, dy + dh), 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
    } else {
        c.DrawText(previewTooLarge_ ? L"文件过大，不支持预览" : L"正在生成预览…",
                   previewRect_.x + 8, previewRect_.y + previewRect_.h / 2 - 12,
                   previewRect_.w - 16, 24, 13, p.muted, false, false,
                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    }
    c.PopClip();

    float rightX = previewRect_.x + previewRect_.w + 16;
    float rightW = w - rightX - 16;
    c.DrawText(L"输出分辨率", rightX, 66, rightW, 22, 13, p.fg, true, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    c.DrawText(L"宽度 (px)", rightX, 106, 120, 20, 11.5f, p.muted, false, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    std::wstring info = L"实际输出：" + std::to_wstring(outWidth_) + L" × " + std::to_wstring(outHeight_) + L" px";
    c.DrawText(info, rightX, 162, rightW, 20, 12, p.fg, true, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);

    c.DrawText(L"快捷比例", rightX, 202, rightW, 20, 12, p.fg, true, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    const wchar_t* labels[4] = { L"1×", L"2×", L"3×", L"4×" };
    for (int i = 0; i < 4; i++) {
        DRect r{ rightX + i * 64.0f, 226, 56, 34 };
        presetRects_[i] = r;
        c.FillRound(r.x, r.y, r.w, r.h, 6, p.surface);
        c.StrokeRound(r.x, r.y, r.w, r.h, 6, p.border);
        c.DrawText(labels[i], r.x, r.y, r.w, r.h, 12, p.fg, false, false,
                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    }
    if (!notice_.empty())
        c.DrawText(notice_, rightX, 280, rightW, 26, 11, p.muted, false, false,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);

    exportRect_ = { rightX, h - 56, 150, 36 };
    cancelRect_ = { rightX + 162, h - 56, 90, 36 };
    c.FillRound(exportRect_.x, exportRect_.y, exportRect_.w, exportRect_.h, 7, p.accent);
    c.DrawText(L"导出 PNG", exportRect_.x, exportRect_.y, exportRect_.w, exportRect_.h,
               13, Color(1, 1, 1, 1), true, false,
               DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    c.FillRound(cancelRect_.x, cancelRect_.y, cancelRect_.w, cancelRect_.h, 7, p.surface);
    c.StrokeRound(cancelRect_.x, cancelRect_.y, cancelRect_.w, cancelRect_.h, 7, p.border);
    c.DrawText(L"取消", cancelRect_.x, cancelRect_.y, cancelRect_.w, cancelRect_.h,
               12, p.fg, false, false, DWRITE_TEXT_ALIGNMENT_CENTER,
               DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    c.End();
    LayoutEdit();
}

void ExportWindow::Export() {
    if (!view_ || !view_->Doc() || outWidth_ < 64) return;
    wchar_t file[32768] = L"";
    std::wstring name = SafeFileName(view_->Doc()->title.empty() ? L"导出图片" : view_->Doc()->title);
    wcsncpy_s(file, name.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    std::wstring saveFilter = TranslateUi(L"PNG 图片 (*.png)") + L'\0' + L"*.png\0" +
                              TranslateUi(L"所有文件 (*.*)") + L"\0*.*\0" + L'\0';
    std::wstring saveTitle = TranslateUi(L"导出 PNG");
    ofn.lpstrFilter = saveFilter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = 32768;
    ofn.lpstrDefExt = L"png";
    ofn.lpstrTitle = saveTitle.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    float scale = (float)outWidth_ / (float)std::max(1, viewW_);
    std::vector<uint8_t> png;
    std::wstring err;
    int ow = 0, oh = 0;
    if (!view_->ExportPng(scale, png, &err, &ow, &oh)) {
        notice_ = err.empty() ? L"导出失败" : err;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (!WriteFileBytes(file, png, &err)) {
        notice_ = err.empty() ? L"无法写入文件" : err;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    notice_ = L"已导出：" + std::to_wstring(ow) + L" × " + std::to_wstring(oh) + L" px";
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ExportWindow::Close() {
    DestroyWindow(hwnd_);
}

LRESULT ExportWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        widthEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
                                     0, 0, 100, 24, hwnd_, (HMENU)1001,
                                     GetModuleHandleW(nullptr), nullptr);
        if (widthEdit_) {
            SendMessageW(widthEdit_, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            SetWindowTextW(widthEdit_, std::to_wstring(outWidth_).c_str());
        }
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        UINT dpi = GetDpiForWindow(hwnd_);
        if (!dpi) dpi = 96;
        float sc = dpi / 96.0f;
        (void)rc;
        (void)sc;
        SchedulePreview();
        return 0;
    }
    case WM_SIZE:
        LayoutEdit();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        Paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {
            KillTimer(hwnd_, 1);
            timer_ = 0;
            UpdatePreview();
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if ((HWND)lp == widthEdit_ && code == EN_CHANGE && !updating_) {
            wchar_t buf[64]{};
            GetWindowTextW(widthEdit_, buf, 64);
            wchar_t* end = nullptr;
            long v = wcstol(buf, &end, 10);
            if (end && end != buf && v >= 64 && v <= 20000) {
                outWidth_ = (int)v;
                outHeight_ = std::max(1, (int)std::lround((double)outWidth_ * viewH_ / viewW_));
                SchedulePreview();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        (void)id;
        return 0;
    }
    case WM_LBUTTONDOWN: {
        float sc = GetDpiForWindow(hwnd_) / 96.0f;
        float x = GET_X_LPARAM(lp) / sc, y = GET_Y_LPARAM(lp) / sc;
        for (int i = 0; i < 4; i++) {
            if (presetRects_[i].Contains(x, y)) {
                outWidth_ = std::max(64, (int)std::lround(viewW_ * (i + 1)));
                outHeight_ = std::max(1, (int)std::lround((double)outWidth_ * viewH_ / viewW_));
                updating_ = true;
                SetWindowTextW(widthEdit_, std::to_wstring(outWidth_).c_str());
                updating_ = false;
                SchedulePreview();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        if (exportRect_.Contains(x, y)) { Export(); return 0; }
        if (cancelRect_.Contains(x, y)) { Close(); return 0; }
        break;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { Close(); return 0; }
        if (wp == VK_RETURN) { Export(); return 0; }
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
