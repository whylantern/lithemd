// ZIP archive reader with ZipCrypto and WinZip AES decryption.
// This is Core-layer code: no HWND, no drawing, no UI text policy.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ArchiveEntry {
    std::wstring name;
    bool isDir = false;
    bool encrypted = false;
    bool aes = false;
    uint16_t method = 0;       // effective method after AES extra field
    uint16_t flags = 0;
    uint32_t crc = 0;
    uint64_t size = 0;
    uint64_t compressed = 0;
    uint64_t localOffset = 0;
    uint64_t dosTime = 0;
    uint32_t index = 0;
};

struct ArchiveLimits {
    uint64_t maxEntrySize = 512ull * 1024 * 1024;
    uint64_t maxTotalSize = 4ull * 1024 * 1024 * 1024;
    size_t maxEntries = 100000;
    // Reject implausible compression ratios before inflating.
    uint64_t maxRatio = 20000;
};

class ArchiveReader {
public:
    bool Open(const std::wstring& path, std::wstring* error = nullptr);
    bool OpenBytes(std::vector<uint8_t> bytes, std::wstring* error = nullptr);
    void Close();
    bool IsOpen() const { return !bytes_.empty(); }
    const std::vector<ArchiveEntry>& Entries() const { return entries_; }
    bool HasEncrypted() const;
    uint64_t TotalSize() const;
    const std::wstring& Path() const { return path_; }

    // Reads one entry. `password` is only required for encrypted entries.
    bool Read(size_t index, const std::wstring& password, std::vector<uint8_t>& out,
              std::wstring* error = nullptr) const;

    // Extracts all entries below destDir. Unsafe paths and oversized payloads
    // are rejected. progress(index, name) may return false to cancel.
    bool ExtractAll(const std::wstring& destDir, const std::wstring& password,
                    const std::function<bool(size_t, const std::wstring&)>& progress,
                    bool overwrite, std::wstring* error = nullptr) const;

    static bool IsSafeEntryName(const std::wstring& name);

private:
    struct EntryExtra {
        uint32_t aesStrength = 0;
        uint16_t aesMethod = 0;
        bool hasAes = false;
    };

    std::vector<uint8_t> bytes_;
    std::vector<ArchiveEntry> entries_;
    std::vector<EntryExtra> extras_;
    std::wstring path_;
};
