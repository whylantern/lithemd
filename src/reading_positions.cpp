#include "reading_positions.h"
#include "common.h"
#include "json.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

std::wstring KeyFor(const std::wstring& path) {
    return ToLower(AbsolutePath(path));
}

} // namespace

ReadingPositions ReadingPositions::Load(const std::wstring& filePath) {
    ReadingPositions result;
    std::vector<uint8_t> bytes;
    if (!FileExists(filePath) || !ReadFileBytes(filePath, bytes)) return result;
    std::wstring encoding;
    std::wstring text = DecodeBytes(bytes, encoding);
    JNode root;
    if (!ParseJson(text, root) || root.kind != JKind::Object) return result;
    const JNode* positions = root.Get(L"positions");
    if (!positions || positions->kind != JKind::Object) return result;
    for (auto& member : positions->members) {
        std::wstring key = KeyFor(member.first);
        if (key.empty()) continue;
        double fraction = -1;
        uint64_t updated = 0;
        if (member.second.kind == JKind::Object) {
            const JNode* f = member.second.Get(L"fraction");
            const JNode* t = member.second.Get(L"updated");
            if (f && f->kind == JKind::Number) fraction = f->number;
            if (t && t->kind == JKind::Number && t->number > 0)
                updated = (uint64_t)t->number;
        } else if (member.second.kind == JKind::Number) {
            fraction = member.second.number;
        }
        if (fraction < 0 || fraction > 1) continue;
        result.entries_[key] = Entry{ fraction, updated };
        if (result.entries_.size() >= 2000) break;
    }
    return result;
}

bool ReadingPositions::Save(const std::wstring& filePath) const {
    std::wstring out = L"{\n  \"version\": 1,\n  \"positions\": {";
    if (entries_.empty()) {
        out += L"}\n}\n";
    } else {
        out += L"\n";
        size_t index = 0;
        for (auto& entry : entries_) {
            out += L"    " + JsonEscape(entry.first) + L": { \"fraction\": ";
            wchar_t fraction[32];
            swprintf(fraction, 32, L"%.6f", entry.second.fraction);
            out += fraction;
            out += L", \"updated\": " + std::to_wstring(entry.second.updated) + L" }";
            if (++index < entries_.size()) out += L",";
            out += L"\n";
        }
        out += L"  }\n}\n";
    }
    EnsureDir(PathDir(filePath));
    return WriteTextFile(filePath, out);
}

double ReadingPositions::Get(const std::wstring& documentPath) const {
    auto it = entries_.find(KeyFor(documentPath));
    if (it == entries_.end()) return -1.0;
    return it->second.fraction;
}

void ReadingPositions::Set(const std::wstring& documentPath, double fraction) {
    std::wstring key = KeyFor(documentPath);
    if (key.empty() || !std::isfinite(fraction)) return;
    fraction = std::clamp(fraction, 0.0, 1.0);
    if (fraction <= 0.002) {
        entries_.erase(key);
        return;
    }
    entries_[key] = Entry{ fraction, UnixNowMs() };
    while (entries_.size() > MaxEntries) {
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
            if (it->second.updated < oldest->second.updated) oldest = it;
        entries_.erase(oldest);
    }
}

void ReadingPositions::Remove(const std::wstring& documentPath) {
    entries_.erase(KeyFor(documentPath));
}
