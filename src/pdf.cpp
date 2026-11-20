// PDF rendering through the open-source PDFium engine (BSD licence).
// PDFium is embedded as an RCDATA resource and extracted beside the user's
// local app data on first use, so the distributed exe remains a single file.
#include "pdf.h"
#include "common.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

extern "C" {
#include "../vendor/pdfium/fpdfview.h"
}

namespace {

constexpr int kPdfiumResourceId = 2;
constexpr const wchar_t* kPdfiumVersion = L"8152";

using FPDFText_LoadPageFn = FPDF_TEXTPAGE(*)(FPDF_PAGE);
using FPDFText_ClosePageFn = void(*)(FPDF_TEXTPAGE);
using FPDFText_CountCharsFn = int(*)(FPDF_TEXTPAGE);
using FPDFText_GetCharBoxFn = int(*)(FPDF_TEXTPAGE, int, double*, double*, double*, double*);
using FPDFText_GetTextFn = int(*)(FPDF_TEXTPAGE, int, int, unsigned short*);

struct PdfiumApi {
    HMODULE module = nullptr;
    decltype(&FPDF_InitLibrary) init = nullptr;
    decltype(&FPDF_DestroyLibrary) destroy = nullptr;
    decltype(&FPDF_LoadDocument) load = nullptr;
    decltype(&FPDF_CloseDocument) closeDoc = nullptr;
    decltype(&FPDF_GetLastError) lastError = nullptr;
    decltype(&FPDF_GetPageCount) pageCount = nullptr;
    decltype(&FPDF_LoadPage) loadPage = nullptr;
    decltype(&FPDF_ClosePage) closePage = nullptr;
    decltype(&FPDF_GetPageWidthF) pageWidth = nullptr;
    decltype(&FPDF_GetPageHeightF) pageHeight = nullptr;
    decltype(&FPDFBitmap_CreateEx) bitmapCreate = nullptr;
    decltype(&FPDFBitmap_Destroy) bitmapDestroy = nullptr;
    decltype(&FPDFBitmap_FillRect) bitmapFill = nullptr;
    decltype(&FPDF_RenderPageBitmap) render = nullptr;
    FPDFText_LoadPageFn loadTextPage = nullptr;
    FPDFText_ClosePageFn closeTextPage = nullptr;
    FPDFText_CountCharsFn countChars = nullptr;
    FPDFText_GetCharBoxFn getCharBox = nullptr;
    FPDFText_GetTextFn getText = nullptr;
};

PdfiumApi g_api;
std::once_flag g_initOnce;

std::wstring EmbeddedDllPath() {
    std::wstring dir = LocalDataDir();
    EnsureDir(dir);
    return PathJoin(dir, std::wstring(L"pdfium-") + kPdfiumVersion + L".dll");
}

bool ExtractEmbeddedDll(const std::wstring& target) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(kPdfiumResourceId), RT_RCDATA);
    if (!res) return false;
    DWORD size = SizeofResource(nullptr, res);
    HGLOBAL handle = LoadResource(nullptr, res);
    if (!handle || !size) return false;
    const void* data = LockResource(handle);
    if (!data) return false;
    if (FileExists(target)) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &fad)) {
            uint64_t existing = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            if (existing == size) return true;
        }
    }
    std::vector<uint8_t> bytes((const uint8_t*)data, (const uint8_t*)data + size);
    return WriteFileBytes(target, bytes, nullptr);
}

template <class T>
bool Resolve(HMODULE mod, const char* name, T& fn) {
    fn = reinterpret_cast<T>(GetProcAddress(mod, name));
    return fn != nullptr;
}

bool LoadPdfium(std::wstring& error) {
    if (g_api.module) return true;
    std::wstring dll;
    std::wstring beside = PathJoin(ExeDir(), L"pdfium.dll");
    if (FileExists(beside)) dll = beside;
    else {
        dll = EmbeddedDllPath();
        if (!ExtractEmbeddedDll(dll)) {
            error = L"内置 PDF 引擎解压失败，无法打开 PDF 文件";
            return false;
        }
    }
    HMODULE mod = LoadLibraryW(dll.c_str());
    if (!mod) {
        error = L"无法加载 PDF 引擎（pdfium.dll，错误 " + std::to_wstring(GetLastError()) + L"）";
        return false;
    }
    bool ok = true;
    ok &= Resolve(mod, "FPDF_InitLibrary", g_api.init);
    ok &= Resolve(mod, "FPDF_DestroyLibrary", g_api.destroy);
    ok &= Resolve(mod, "FPDF_LoadDocument", g_api.load);
    ok &= Resolve(mod, "FPDF_CloseDocument", g_api.closeDoc);
    ok &= Resolve(mod, "FPDF_GetLastError", g_api.lastError);
    ok &= Resolve(mod, "FPDF_GetPageCount", g_api.pageCount);
    ok &= Resolve(mod, "FPDF_LoadPage", g_api.loadPage);
    ok &= Resolve(mod, "FPDF_ClosePage", g_api.closePage);
    ok &= Resolve(mod, "FPDF_GetPageWidthF", g_api.pageWidth);
    ok &= Resolve(mod, "FPDF_GetPageHeightF", g_api.pageHeight);
    ok &= Resolve(mod, "FPDFBitmap_CreateEx", g_api.bitmapCreate);
    ok &= Resolve(mod, "FPDFBitmap_Destroy", g_api.bitmapDestroy);
    ok &= Resolve(mod, "FPDFBitmap_FillRect", g_api.bitmapFill);
    ok &= Resolve(mod, "FPDF_RenderPageBitmap", g_api.render);
    // Text APIs are optional: rendering still works if a build lacks them.
    Resolve(mod, "FPDFText_LoadPage", g_api.loadTextPage);
    Resolve(mod, "FPDFText_ClosePage", g_api.closeTextPage);
    Resolve(mod, "FPDFText_CountChars", g_api.countChars);
    Resolve(mod, "FPDFText_GetCharBox", g_api.getCharBox);
    Resolve(mod, "FPDFText_GetText", g_api.getText);
    if (!ok) {
        FreeLibrary(mod);
        error = L"PDF 引擎版本不兼容（缺少所需的 FPDF 接口）";
        return false;
    }
    std::call_once(g_initOnce, [] { g_api.init(); });
    g_api.module = mod;
    return true;
}

std::wstring PdfErrorText(unsigned long code) {
    switch (code) {
    case FPDF_ERR_SUCCESS: return L"未知错误";
    case FPDF_ERR_UNKNOWN: return L"未知错误";
    case FPDF_ERR_FILE: return L"文件无法打开或已损坏";
    case FPDF_ERR_FORMAT: return L"不是有效的 PDF 文件";
    case FPDF_ERR_PASSWORD: return L"PDF 受密码保护";
    case FPDF_ERR_SECURITY: return L"PDF 使用了不支持的加密方式";
    case FPDF_ERR_PAGE: return L"页面内容无法读取";
    default: return L"PDF 引擎错误 " + std::to_wstring(code);
    }
}

// PDFium writes BGRA top-down; BMP files are bottom-up. Converting to BMP lets
// the existing WIC/D2D image cache decode PDF pages without a second renderer.
std::vector<uint8_t> MakeBmp(int w, int h, const uint8_t* bgra, int stride) {
    std::vector<uint8_t> bmp(54 + (size_t)w * h * 4);
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = 54;
    fh.bfSize = (DWORD)bmp.size();
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof(ih);
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    ih.biSizeImage = (DWORD)(w * h * 4);
    memcpy(bmp.data(), &fh, sizeof(fh));
    memcpy(bmp.data() + sizeof(fh), &ih, sizeof(ih));
    uint8_t* dst = bmp.data() + 54;
    for (int y = 0; y < h; y++) {
        const uint8_t* src = bgra + (size_t)(h - 1 - y) * stride;
        memcpy(dst + (size_t)y * w * 4, src, (size_t)w * 4);
        for (int x = 0; x < w; x++) dst[(size_t)y * w * 4 + (size_t)x * 4 + 3] = 255;
    }
    return bmp;
}

} // namespace

struct PdfDocument::Impl {
    FPDF_DOCUMENT doc = nullptr;
    std::vector<std::pair<float, float>> sizes;
    std::wstring error;
};

PdfDocument::PdfDocument() : impl_(new Impl()) {}
PdfDocument::~PdfDocument() {
    if (impl_->doc && g_api.closeDoc) g_api.closeDoc(impl_->doc);
    impl_->doc = nullptr;
}

bool PdfDocument::Load(const std::wstring& path, std::wstring& error) {
    error.clear();
    std::wstring engineError;
    if (!LoadPdfium(engineError)) {
        error = engineError;
        return false;
    }
    if (impl_->doc) g_api.closeDoc(impl_->doc);
    impl_->doc = nullptr;
    impl_->sizes.clear();
    std::string u = Utf8FromWide(path);
    impl_->doc = g_api.load(u.c_str(), nullptr);
    if (!impl_->doc) {
        unsigned long code = g_api.lastError ? g_api.lastError() : 0;
        error = L"无法打开 PDF：" + PdfErrorText(code);
        return false;
    }
    int pages = g_api.pageCount(impl_->doc);
    if (pages <= 0) {
        error = L"PDF 中没有页面";
        return false;
    }
    for (int i = 0; i < pages; i++) {
        FPDF_PAGE page = g_api.loadPage(impl_->doc, i);
        if (!page) {
            impl_->sizes.push_back({ 612.0f, 792.0f });
            continue;
        }
        float w = g_api.pageWidth(page);
        float h = g_api.pageHeight(page);
        g_api.closePage(page);
        if (w <= 1 || h <= 1) { w = 612; h = 792; }
        impl_->sizes.push_back({ w, h });
    }
    return true;
}

int PdfDocument::PageCount() const { return (int)impl_->sizes.size(); }

bool PdfDocument::PageSize(int index, float& width, float& height) const {
    if (index < 0 || index >= (int)impl_->sizes.size()) return false;
    width = impl_->sizes[(size_t)index].first;
    height = impl_->sizes[(size_t)index].second;
    return true;
}

bool PdfDocument::passwordProtected() const { return false; }

bool PdfDocument::PageText(int index, std::vector<PdfChar>& chars) {
    chars.clear();
    if (!impl_->doc || index < 0 || index >= (int)impl_->sizes.size()) return false;
    if (!g_api.loadTextPage || !g_api.closeTextPage || !g_api.countChars ||
        !g_api.getCharBox || !g_api.getText) return false;
    FPDF_PAGE page = g_api.loadPage(impl_->doc, index);
    if (!page) return false;
    FPDF_TEXTPAGE text = g_api.loadTextPage(page);
    if (!text) {
        g_api.closePage(page);
        return true; // scanned page: valid page, no text layer
    }
    int count = g_api.countChars(text);
    if (count > 0) {
        std::vector<unsigned short> buffer((size_t)count + 1, 0);
        int got = g_api.getText(text, 0, count, buffer.data());
        chars.reserve((size_t)std::min(count, got > 0 ? got : count));
        for (int i = 0; i < count; i++) {
            double left = 0, right = 0, bottom = 0, top = 0;
            if (!g_api.getCharBox(text, i, &left, &right, &bottom, &top)) continue;
            PdfChar ch;
            ch.left = (float)std::min(left, right);
            ch.right = (float)std::max(left, right);
            ch.bottom = (float)std::min(bottom, top);
            ch.top = (float)std::max(bottom, top);
            ch.ch = (i < got && buffer[(size_t)i]) ? (wchar_t)buffer[(size_t)i] : L' ';
            chars.push_back(ch);
        }
    }
    g_api.closeTextPage(text);
    g_api.closePage(page);
    return true;
}

bool PdfDocument::RenderPage(int index, int targetWidth, std::vector<uint8_t>& png, std::wstring& error) {
    png.clear();
    if (!impl_->doc || index < 0 || index >= (int)impl_->sizes.size()) {
        error = L"PDF 页面索引无效";
        return false;
    }
    FPDF_PAGE page = g_api.loadPage(impl_->doc, index);
    if (!page) { error = L"无法读取 PDF 页面"; return false; }
    float pw = impl_->sizes[(size_t)index].first;
    float ph = impl_->sizes[(size_t)index].second;
    if (targetWidth < 64) targetWidth = 64;
    if (targetWidth > 4000) targetWidth = 4000;
    int w = targetWidth;
    int h = (int)std::lround(targetWidth * (ph / std::max(1.0f, pw)));
    if (h < 64) h = 64;
    if (h > 6000) h = 6000;
    int stride = w * 4;
    std::vector<uint8_t> bgra((size_t)stride * h, 255);
    FPDF_BITMAP bitmap = g_api.bitmapCreate(w, h, FPDFBitmap_BGRA, bgra.data(), stride);
    if (!bitmap) {
        g_api.closePage(page);
        error = L"无法创建 PDF 渲染缓冲区";
        return false;
    }
    g_api.bitmapFill(bitmap, 0, 0, w, h, 0xFFFFFFFF);
    g_api.render(bitmap, page, 0, 0, w, h, 0, FPDF_ANNOT);
    g_api.bitmapDestroy(bitmap);
    g_api.closePage(page);
    png = MakeBmp(w, h, bgra.data(), stride);
    return !png.empty();
}
