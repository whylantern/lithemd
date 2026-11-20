// PDF rendering through the open-source PDFium engine (BSD licence), embedded
// into the executable as an RCDATA resource. No .NET dependency.
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

struct PdfChar {
    float left = 0, top = 0, right = 0, bottom = 0;
    wchar_t ch = 0;
};

class PdfDocument {
public:
    PdfDocument();
    ~PdfDocument();
    PdfDocument(const PdfDocument&) = delete;
    PdfDocument& operator=(const PdfDocument&) = delete;

    bool Load(const std::wstring& path, std::wstring& error);
    int PageCount() const;
    bool PageSize(int index, float& width, float& height) const;
    bool PageText(int index, std::vector<PdfChar>& chars);
    bool RenderPage(int index, int targetWidth, std::vector<uint8_t>& png, std::wstring& error);
    bool passwordProtected() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
