// Minimal read-only OLE Compound File Binary (CFB) reader used by .doc/.xls.
#pragma once
#include <string>
#include <vector>
#include <cstdint>

class CfbFile {
public:
    bool Open(const std::vector<uint8_t>& bytes, std::wstring* error = nullptr);
    bool ReadStream(const std::wstring& name, std::vector<uint8_t>& out) const;
    bool HasStream(const std::wstring& name) const;
    std::vector<std::wstring> Names() const;

private:
    struct Entry {
        std::wstring name;
        uint8_t type = 0;
        uint32_t start = 0;
        uint64_t size = 0;
    };
    bool ReadChain(uint32_t start, const std::vector<uint32_t>& fat, std::vector<uint8_t>& out,
                   uint64_t maxBytes = 512ull * 1024 * 1024) const;
    bool ReadMiniChain(uint32_t start, std::vector<uint8_t>& out) const;

    std::vector<uint8_t> data_;
    uint32_t sectorSize_ = 512;
    uint32_t miniSectorSize_ = 64;
    uint32_t miniCutoff_ = 4096;
    std::vector<uint32_t> fat_;
    std::vector<uint32_t> miniFat_;
    std::vector<Entry> dir_;
    std::vector<uint8_t> miniStream_;
};
