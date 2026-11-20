#include "ole.h"
#include "common.h"
#include <cstring>
#include <set>

namespace {
uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
constexpr uint32_t kEndOfChain = 0xFFFFFFFE;
constexpr uint32_t kFreeSect = 0xFFFFFFFF;
} // namespace

bool CfbFile::Open(const std::vector<uint8_t>& bytes, std::wstring* error) {
    if (bytes.size() < 512) { if (error) *error = L"OLE 文件过小"; return false; }
    static const uint8_t sig[8] = { 0xD0,0xCF,0x11,0xE0,0xA1,0xB1,0x1A,0xE1 };
    if (memcmp(bytes.data(), sig, 8) != 0) { if (error) *error = L"不是 OLE 复合文档"; return false; }
    data_ = bytes;
    uint16_t sectorShift = U16(data_.data() + 0x1E);
    uint16_t miniShift = U16(data_.data() + 0x20);
    if (sectorShift < 7 || sectorShift > 14) sectorShift = 9;
    if (miniShift < 4 || miniShift > 10) miniShift = 6;
    sectorSize_ = 1u << sectorShift;
    miniSectorSize_ = 1u << miniShift;
    miniCutoff_ = U32(data_.data() + 0x38);
    if (miniCutoff_ == 0) miniCutoff_ = 4096;

    uint32_t numFat = U32(data_.data() + 0x2C);
    uint32_t firstDir = U32(data_.data() + 0x30);
    uint32_t firstMiniFat = U32(data_.data() + 0x3C);
    uint32_t numMiniFat = U32(data_.data() + 0x40);
    uint32_t firstDifat = U32(data_.data() + 0x44);
    uint32_t numDifat = U32(data_.data() + 0x48);

    std::vector<uint32_t> fatSectors;
    for (int i = 0; i < 109; i++) {
        uint32_t s = U32(data_.data() + 0x4C + i * 4);
        if (s != kFreeSect && s != kEndOfChain) fatSectors.push_back(s);
    }
    uint32_t difatSector = firstDifat;
    uint32_t guard = 0;
    while (difatSector != kEndOfChain && difatSector != kFreeSect && guard++ < numDifat + 8) {
        uint64_t off = (uint64_t)(difatSector + 1) * sectorSize_;
        if (off + sectorSize_ > data_.size()) break;
        const uint8_t* p = data_.data() + off;
        uint32_t count = sectorSize_ / 4 - 1;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t s = U32(p + i * 4);
            if (s != kFreeSect && s != kEndOfChain) fatSectors.push_back(s);
        }
        difatSector = U32(p + count * 4);
    }
    if (fatSectors.size() > numFat + 16) fatSectors.resize(numFat + 16);
    for (uint32_t s : fatSectors) {
        uint64_t off = (uint64_t)(s + 1) * sectorSize_;
        if (off + sectorSize_ > data_.size()) continue;
        const uint8_t* p = data_.data() + off;
        for (uint32_t i = 0; i < sectorSize_ / 4; i++) fat_.push_back(U32(p + i * 4));
    }
    if (fat_.empty()) { if (error) *error = L"OLE FAT 为空"; return false; }

    // Directory
    std::vector<uint8_t> dirData;
    if (!ReadChain(firstDir, fat_, dirData)) { if (error) *error = L"无法读取 OLE 目录"; return false; }
    for (size_t off = 0; off + 128 <= dirData.size(); off += 128) {
        const uint8_t* p = dirData.data() + off;
        uint8_t type = p[0x42];
        if (type != 1 && type != 2 && type != 5) continue;
        uint16_t nameLen = U16(p + 0x40);
        if (nameLen > 64) nameLen = 64;
        std::wstring name;
        for (uint16_t i = 0; i + 1 < nameLen; i += 2) {
            wchar_t c = (wchar_t)U16(p + i);
            if (c == 0) break;
            name += c;
        }
        Entry e;
        e.name = name;
        e.type = type;
        e.start = U32(p + 0x74);
        e.size = U32(p + 0x78);
        uint32_t hi = U32(p + 0x7C);
        if (hi) e.size |= ((uint64_t)hi << 32);
        dir_.push_back(std::move(e));
    }
    if (dir_.empty()) { if (error) *error = L"OLE 目录为空"; return false; }

    // Mini stream (root entry)
    const Entry* root = nullptr;
    for (auto& e : dir_) if (e.type == 5) { root = &e; break; }
    if (root) {
        std::vector<uint8_t> mini;
        if (ReadChain(root->start, fat_, mini, root->size + sectorSize_)) {
            if (root->size && mini.size() > root->size) mini.resize((size_t)root->size);
            miniStream_ = std::move(mini);
        }
    }
    if (numMiniFat && firstMiniFat != kEndOfChain) {
        std::vector<uint8_t> mf;
        if (ReadChain(firstMiniFat, fat_, mf)) {
            for (size_t i = 0; i + 4 <= mf.size(); i += 4) miniFat_.push_back(U32(mf.data() + i));
        }
    }
    return true;
}

bool CfbFile::ReadChain(uint32_t start, const std::vector<uint32_t>& fat, std::vector<uint8_t>& out,
                        uint64_t maxBytes) const {
    out.clear();
    uint32_t s = start;
    std::set<uint32_t> seen;
    while (s != kEndOfChain && s != kFreeSect && s < fat.size()) {
        if (!seen.insert(s).second) return false;
        uint64_t off = (uint64_t)(s + 1) * sectorSize_;
        if (off + sectorSize_ > data_.size()) return false;
        size_t take = (size_t)std::min<uint64_t>(sectorSize_, maxBytes > out.size() ? maxBytes - out.size() : 0);
        if (take == 0) break;
        out.insert(out.end(), data_.begin() + (ptrdiff_t)off, data_.begin() + (ptrdiff_t)(off + take));
        s = fat[s];
        if (out.size() >= maxBytes) break;
    }
    return !out.empty();
}

bool CfbFile::ReadMiniChain(uint32_t start, std::vector<uint8_t>& out) const {
    out.clear();
    uint32_t s = start;
    std::set<uint32_t> seen;
    while (s != kEndOfChain && s != kFreeSect && s < miniFat_.size()) {
        if (!seen.insert(s).second) return false;
        uint64_t off = (uint64_t)s * miniSectorSize_;
        if (off + miniSectorSize_ > miniStream_.size()) return false;
        out.insert(out.end(), miniStream_.begin() + (ptrdiff_t)off,
                   miniStream_.begin() + (ptrdiff_t)(off + miniSectorSize_));
        s = miniFat_[s];
    }
    return !out.empty();
}

bool CfbFile::ReadStream(const std::wstring& name, std::vector<uint8_t>& out) const {
    const Entry* e = nullptr;
    for (auto& x : dir_) if (x.type == 2 && _wcsicmp(x.name.c_str(), name.c_str()) == 0) { e = &x; break; }
    if (!e) return false;
    if (e->size == 0) { out.clear(); return true; }
    if (e->size < miniCutoff_) {
        if (!ReadMiniChain(e->start, out)) return false;
    } else {
        if (!ReadChain(e->start, fat_, out, e->size + sectorSize_)) return false;
    }
    if (out.size() > e->size) out.resize((size_t)e->size);
    return true;
}

bool CfbFile::HasStream(const std::wstring& name) const {
    for (auto& x : dir_) if (x.type == 2 && _wcsicmp(x.name.c_str(), name.c_str()) == 0) return true;
    return false;
}

std::vector<std::wstring> CfbFile::Names() const {
    std::vector<std::wstring> out;
    for (auto& e : dir_) if (e.type == 2) out.push_back(e.name);
    return out;
}
