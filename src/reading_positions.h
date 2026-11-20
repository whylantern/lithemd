// Per-document reading positions, persisted as JSON next to config.json.
#pragma once
#include <cstdint>
#include <map>
#include <string>

class ReadingPositions {
public:
    static ReadingPositions Load(const std::wstring& filePath);

    bool Save(const std::wstring& filePath) const;
    // Returns a 0..1 scroll fraction, or -1 when no saved position exists.
    double Get(const std::wstring& documentPath) const;
    void Set(const std::wstring& documentPath, double fraction);
    void Remove(const std::wstring& documentPath);
    void Clear() { entries_.clear(); }
    size_t Size() const { return entries_.size(); }

    static constexpr size_t MaxEntries = 500;

private:
    struct Entry {
        double fraction = 0.0;
        uint64_t updated = 0;
    };
    std::map<std::wstring, Entry> entries_;
};
