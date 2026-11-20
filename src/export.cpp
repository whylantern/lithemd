#include "export.h"
#include "view.h"
#include "i18n.h"
#include "../vendor/miniz.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <commdlg.h>
#include <shlwapi.h>
#include <wincodec.h>
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

bool DecodePngRgb(const std::vector<uint8_t>& png, std::vector<uint8_t>& rgb,
                  UINT& width, UINT& height, std::wstring* error) {
    rgb.clear();
    ComPtr<IStream> stream;
    stream.Attach(SHCreateMemStream(png.data(), (UINT)png.size()));
    if (!stream) {
        if (error) *error = L"无法读取导出页面";
        return false;
    }
    auto* wic = Engine::Instance().WIC();
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                            &decoder))) {
        if (error) *error = L"无法读取导出页面";
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) return false;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter))) return false;
    if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom)))
        return false;
    if (FAILED(converter->GetSize(&width, &height)) || !width || !height) return false;
    const UINT stride = width * 3;
    std::vector<uint8_t> bgr((size_t)stride * height);
    if (FAILED(converter->CopyPixels(nullptr, stride, (UINT)bgr.size(), bgr.data()))) return false;
    rgb.resize((size_t)width * height * 3);
    for (size_t i = 0, n = (size_t)width * height; i < n; i++) {
        rgb[i * 3 + 0] = bgr[i * 3 + 2];
        rgb[i * 3 + 1] = bgr[i * 3 + 1];
        rgb[i * 3 + 2] = bgr[i * 3 + 0];
    }
    return true;
}

struct PdfRasterPage {
    UINT width = 0;
    UINT height = 0;
    std::vector<uint8_t> compressed;
};

bool RgbIsBlank(const std::vector<uint8_t>& rgb) {
    if (rgb.size() < 3) return true;
    uint8_t r = rgb[0], g = rgb[1], b = rgb[2];
    for (size_t i = 3; i + 2 < rgb.size(); i += 3) {
        if (std::abs((int)rgb[i] - r) > 4 || std::abs((int)rgb[i + 1] - g) > 4 ||
            std::abs((int)rgb[i + 2] - b) > 4)
            return false;
    }
    return true;
}

bool CompressRgb(const std::vector<uint8_t>& rgb, std::vector<uint8_t>& out) {
    mz_ulong bound = mz_compressBound((mz_ulong)rgb.size());
    out.resize((size_t)bound);
    mz_ulong length = bound;
    int result = mz_compress2(out.data(), &length, rgb.data(), (mz_ulong)rgb.size(), 6);
    if (result != MZ_OK) {
        out.clear();
        return false;
    }
    out.resize((size_t)length);
    return true;
}

bool WriteRasterPdf(const std::wstring& path, const std::vector<PdfRasterPage>& pages,
                    float pageWidthPt, float pageHeightPt, std::wstring* error) {
    if (pages.empty()) {
        if (error) *error = L"当前没有可导出的内容";
        return false;
    }
    std::string out = "%PDF-1.4\n";
    const int objectCount = 2 + (int)pages.size() * 3;
    std::vector<size_t> offsets((size_t)objectCount + 1, 0);
    auto appendNumber = [](std::string& s, long long value) { s += std::to_string(value); };
    auto addObject = [&](int id, const std::string& body) {
        offsets[(size_t)id] = out.size();
        appendNumber(out, id);
        out += " 0 obj\n";
        out += body;
        out += "\nendobj\n";
    };
    addObject(1, "<< /Type /Catalog /Pages 2 0 R >>");
    std::string kids = "<< /Type /Pages /Count ";
    kids += std::to_string(pages.size());
    kids += " /Kids [";
    for (size_t i = 0; i < pages.size(); i++) {
        if (i) kids += " ";
        kids += std::to_string(3 + (int)i * 3) + " 0 R";
    }
    kids += "] >>";
    addObject(2, kids);
    char pageBox[128];
    snprintf(pageBox, sizeof(pageBox), "/MediaBox [0 0 %.2f %.2f]", pageWidthPt, pageHeightPt);
    char contentStream[192];
    snprintf(contentStream, sizeof(contentStream),
             "q %.2f 0 0 %.2f 0 0 cm /Im0 Do Q", pageWidthPt, pageHeightPt);
    for (size_t i = 0; i < pages.size(); i++) {
        int pageId = 3 + (int)i * 3;
        int imageId = pageId + 1;
        int contentId = pageId + 2;
        std::string pageBody = "<< /Type /Page /Parent 2 0 R ";
        pageBody += pageBox;
        pageBody += " /Resources << /XObject << /Im0 ";
        pageBody += std::to_string(imageId) + " 0 R >> >> /Contents ";
        pageBody += std::to_string(contentId) + " 0 R >>";
        addObject(pageId, pageBody);

        std::string imageBody = "<< /Type /XObject /Subtype /Image /Width ";
        imageBody += std::to_string(pages[i].width) + " /Height ";
        imageBody += std::to_string(pages[i].height);
        imageBody += " /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /FlateDecode /Length ";
        imageBody += std::to_string(pages[i].compressed.size()) + " >>\nstream\n";
        offsets[(size_t)imageId] = out.size();
        appendNumber(out, imageId);
        out += " 0 obj\n";
        out += imageBody;
        out.append((const char*)pages[i].compressed.data(), pages[i].compressed.size());
        out += "\nendstream\nendobj\n";

        std::string contentBody = "<< /Length ";
        contentBody += std::to_string(strlen(contentStream)) + " >>\nstream\n";
        contentBody += contentStream;
        contentBody += "\nendstream";
        addObject(contentId, contentBody);
    }
    size_t xref = out.size();
    out += "xref\n0 " + std::to_string(objectCount + 1) + "\n";
    out += "0000000000 65535 f \n";
    char row[32];
    for (int i = 1; i <= objectCount; i++) {
        snprintf(row, sizeof(row), "%010llu 00000 n \n", (unsigned long long)offsets[(size_t)i]);
        out += row;
    }
    out += "trailer\n<< /Size " + std::to_string(objectCount + 1) + " /Root 1 0 R >>\n";
    out += "startxref\n" + std::to_string(xref) + "\n%%EOF\n";
    std::vector<uint8_t> bytes(out.begin(), out.end());
    return WriteFileBytes(path, bytes, error);
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
        wc.lpszClassName = L"LitheMdExport";
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
    hwnd_ = CreateWindowExW(0, L"LitheMdExport", TranslateUi(L"导出").c_str(), style, ox, oy, ww, wh,
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
    ShowWindow(widthEdit_, (!formatPdf_ && !a4Pages_) ? SW_SHOW : SW_HIDE);
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
    if (!c.Begin(hwnd_, Palette::Get(settings_->theme))) return;
    c.SetTranslateUi(true);
    auto& p = c.palette();
    float w = c.widthDip(), h = c.heightDip();
    c.FillRect(0, 0, w, h, p.bg);
    c.DrawText(L"导出", 16, 12, w - 32, 28, 18, p.fg, true, false,
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
    c.DrawText(L"输出设置", rightX, 66, rightW, 22, 13, p.fg, true, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    bool showWidth = !formatPdf_ && !a4Pages_;
    if (showWidth) {
        c.DrawText(L"宽度 (px)", rightX, 100, 120, 20, 11.5f, p.muted, false, false,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
        std::wstring info = L"实际输出：" + std::to_wstring(outWidth_) + L" × " +
                            std::to_wstring(outHeight_) + L" px";
        c.DrawText(info, rightX, 154, rightW, 20, 12, p.fg, true, false,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    } else {
        int pageW = (int)std::lround(595.28 * dpi_ / 72.0);
        int pageH = (int)std::lround(841.89 * dpi_ / 72.0);
        std::wstring info = a4Pages_
            ? L"A4 " + std::to_wstring(dpi_) + L" DPI  " + std::to_wstring(pageW) + L" × " +
              std::to_wstring(pageH) + L" px"
            : L"单张长页";
        c.DrawText(info, rightX, 100, rightW, 42, 12, p.fg, false, false,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
    }

    if (showWidth) {
        c.DrawText(L"快捷比例", rightX, 186, rightW, 20, 12, p.fg, true, false,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
        const wchar_t* labels[4] = { L"1×", L"2×", L"3×", L"4×" };
        for (int i = 0; i < 4; i++) {
            DRect r{ rightX + i * 64.0f, 210, 56, 34 };
            presetRects_[i] = r;
            c.FillRound(r.x, r.y, r.w, r.h, 6, p.surface);
            c.StrokeRound(r.x, r.y, r.w, r.h, 6, p.border);
            c.DrawText(labels[i], r.x, r.y, r.w, r.h, 12, p.fg, false, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        }
    }

    auto toggle = [&](DRect& rect, float x, float y, float width, const wchar_t* label, bool active) {
        rect = { x, y, width, 32 };
        c.FillRound(rect.x, rect.y, rect.w, rect.h, 6, active ? p.accentSoft : p.surface);
        c.StrokeRound(rect.x, rect.y, rect.w, rect.h, 6, active ? p.accent : p.border);
        c.DrawText(label, rect.x, rect.y, rect.w, rect.h, 11.5f, active ? p.accent : p.fg,
                   active, false, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    };

    c.DrawText(L"格式", rightX, 258, 80, 18, 11.5f, p.muted, false, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    toggle(formatPngRect_, rightX, 278, 84, L"PNG", !formatPdf_);
    toggle(formatPdfRect_, rightX + 92, 278, 84, L"PDF", formatPdf_);

    c.DrawText(L"页面", rightX, 320, 80, 18, 11.5f, p.muted, false, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    toggle(layoutA4Rect_, rightX, 340, 104, L"A4 分页", a4Pages_);
    toggle(layoutLongRect_, rightX + 112, 340, 104, L"单张长页", !a4Pages_);

    c.DrawText(L"清晰度", rightX, 382, 80, 18, 11.5f, p.muted, false, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
    toggle(dpi150Rect_, rightX, 402, 74, L"150 DPI", dpi_ == 150);
    toggle(dpi300Rect_, rightX + 82, 402, 74, L"300 DPI", dpi_ == 300);

    if (!notice_.empty())
        c.DrawText(notice_, rightX, 442, rightW, 44, 11, p.muted, false, false,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);

    exportRect_ = { rightX, h - 56, 150, 36 };
    cancelRect_ = { rightX + 162, h - 56, 90, 36 };
    c.FillRound(exportRect_.x, exportRect_.y, exportRect_.w, exportRect_.h, 7, p.accent);
    c.DrawText(formatPdf_ ? L"导出 PDF" : L"导出 PNG", exportRect_.x, exportRect_.y,
               exportRect_.w, exportRect_.h, 13, Color(1, 1, 1, 1), true, false,
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
    if (!view_ || !view_->Doc()) return;
    if (formatPdf_ && !a4Pages_) {
        float pageHeightPt = 595.28f * (float)viewH_ / (float)std::max(1, viewW_);
        if (pageHeightPt > 14400.0f) a4Pages_ = true;
    }
    wchar_t file[32768] = L"";
    std::wstring title = view_->Doc()->title.empty() ? L"导出图片" : view_->Doc()->title;
    std::wstring name = SafeFileName(title);
    if (name.size() >= 4) name.resize(name.size() - 4); // drop .png
    if (formatPdf_) name += L".pdf";
    else name += L".png";
    wcsncpy_s(file, name.c_str(), _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    std::wstring saveFilter;
    std::wstring saveTitle;
    if (formatPdf_) {
        saveFilter = TranslateUi(L"PDF 文件 (*.pdf)") + L'\0' + L"*.pdf\0" +
                     TranslateUi(L"所有文件 (*.*)") + L"\0*.*\0" + L'\0';
        saveTitle = TranslateUi(L"导出 PDF");
        ofn.lpstrDefExt = L"pdf";
    } else {
        saveFilter = TranslateUi(L"PNG 图片 (*.png)") + L'\0' + L"*.png\0" +
                     TranslateUi(L"所有文件 (*.*)") + L"\0*.*\0" + L'\0';
        saveTitle = TranslateUi(L"导出 PNG");
        ofn.lpstrDefExt = L"png";
    }
    ofn.lpstrFilter = saveFilter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = 32768;
    ofn.lpstrTitle = saveTitle.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;

    std::wstring err;
    if (!a4Pages_) {
        if (formatPdf_) {
            float pageWpx = 595.28f * dpi_ / 72.0f;
            float scale = pageWpx / (float)std::max(1, viewW_);
            std::vector<uint8_t> png;
            int ow = 0, oh = 0;
            if (!view_->ExportPng(scale, png, &err, &ow, &oh)) {
                notice_ = err.empty() ? L"导出失败" : err;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            std::vector<uint8_t> rgb;
            UINT rw = 0, rh = 0;
            if (!DecodePngRgb(png, rgb, rw, rh, &err)) {
                notice_ = err.empty() ? L"无法生成导出图片" : err;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            PdfRasterPage page;
            page.width = rw;
            page.height = rh;
            if (!CompressRgb(rgb, page.compressed)) {
                notice_ = L"无法生成导出图片";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            float pageHpt = 595.28f * (float)viewH_ / (float)std::max(1, viewW_);
            if (!WriteRasterPdf(file, { page }, 595.28f, pageHpt, &err)) {
                notice_ = err.empty() ? L"无法写入文件" : err;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            notice_ = L"已导出 PDF：" + std::to_wstring(ow) + L" × " + std::to_wstring(oh) + L" px";
        } else {
            float scale = (float)outWidth_ / (float)std::max(1, viewW_);
            std::vector<uint8_t> png;
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
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    int pageWpx = (int)std::lround(595.28 * dpi_ / 72.0);
    int pageHpx = (int)std::lround(841.89 * dpi_ / 72.0);
    float scale = (float)pageWpx / (float)std::max(1, viewW_);
    float pageHeightDip = (float)pageHpx / scale;
    int pageCount = std::max(1, (int)std::ceil((float)viewH_ / pageHeightDip));
    if (pageCount > 500) {
        notice_ = L"页数过多，已取消导出";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    std::vector<PdfRasterPage> pdfPages;
    int writtenPages = 0;
    std::wstring base = file;
    size_t dot = base.rfind(L'.');
    if (dot != std::wstring::npos) base.resize(dot);
    for (int pageIndex = 0; pageIndex < pageCount; pageIndex++) {
        std::vector<uint8_t> png;
        int ow = 0, oh = 0;
        if (!view_->ExportPngPage(scale, pageIndex * pageHeightDip, pageHeightDip,
                                  png, &err, &ow, &oh)) {
            notice_ = err.empty() ? L"导出失败" : err;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (formatPdf_) {
            std::vector<uint8_t> rgb;
            UINT rw = 0, rh = 0;
            if (!DecodePngRgb(png, rgb, rw, rh, &err)) {
                notice_ = err.empty() ? L"无法生成导出图片" : err;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            if (pageIndex == pageCount - 1 && RgbIsBlank(rgb)) continue;
            PdfRasterPage page;
            page.width = rw;
            page.height = rh;
            if (!CompressRgb(rgb, page.compressed)) {
                notice_ = L"无法生成导出图片";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            pdfPages.push_back(std::move(page));
        } else {
            if (pageIndex == pageCount - 1) {
                std::vector<uint8_t> rgb;
                UINT rw = 0, rh = 0;
                if (DecodePngRgb(png, rgb, rw, rh, &err) && RgbIsBlank(rgb)) continue;
            }
            wchar_t suffix[32];
            swprintf(suffix, 32, L"-%02d.png", pageIndex + 1);
            std::wstring pagePath = base + suffix;
            if (!WriteFileBytes(pagePath, png, &err)) {
                notice_ = err.empty() ? L"无法写入文件" : err;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            writtenPages++;
        }
    }
    if (formatPdf_) {
        if (!WriteRasterPdf(file, pdfPages, 595.28f, 841.89f, &err)) {
            notice_ = err.empty() ? L"无法写入文件" : err;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        notice_ = L"已导出 PDF：" + std::to_wstring(pdfPages.size()) + L" 页";
    } else {
        notice_ = L"已导出页面：" + std::to_wstring(writtenPages) + L" 张";
    }
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
        if (formatPngRect_.Contains(x, y)) { formatPdf_ = false; LayoutEdit(); InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
        if (formatPdfRect_.Contains(x, y)) { formatPdf_ = true; LayoutEdit(); InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
        if (layoutA4Rect_.Contains(x, y)) { a4Pages_ = true; LayoutEdit(); InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
        if (layoutLongRect_.Contains(x, y)) { a4Pages_ = false; LayoutEdit(); InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
        if (dpi150Rect_.Contains(x, y)) { dpi_ = 150; InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
        if (dpi300Rect_.Contains(x, y)) { dpi_ = 300; InvalidateRect(hwnd_, nullptr, FALSE); return 0; }
        for (int i = 0; i < 4; i++) {
            if (!formatPdf_ && !a4Pages_ && presetRects_[i].Contains(x, y)) {
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
