// ZIP reader for OOXML wrappers, backed by the public-domain miniz library.
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>

class ZipReader {
public:
    ZipReader();
    ~ZipReader();
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    bool Open(std::vector<uint8_t> bytes, std::wstring* error = nullptr);
    bool Has(const std::wstring& name) const;
    bool Read(const std::wstring& name, std::vector<uint8_t>& out) const;
    bool ReadText(const std::wstring& name, std::wstring& out, std::wstring* error = nullptr) const;
    std::vector<std::wstring> Names() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
