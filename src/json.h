// Minimal JSON DOM and parser used by openmd. No external dependencies.
#pragma once
#include <string>
#include <vector>
#include <utility>

enum class JKind { Null, Bool, Number, String, Array, Object };

struct JNode {
    JKind kind = JKind::Null;
    bool boolean = false;
    double number = 0.0;
    std::wstring raw;                 // original number text
    std::wstring str;                 // decoded string
    std::vector<JNode> items;         // array
    std::vector<std::pair<std::wstring, JNode>> members; // object

    const JNode* Get(const std::wstring& key) const;
    JNode* Get(const std::wstring& key);
    const JNode* At(size_t i) const;
    std::wstring Text() const;        // string/number/bool as display text
    size_t Size() const;
};

bool ParseJson(const std::wstring& text, JNode& out, std::wstring* error = nullptr);
std::wstring PrettyJson(const JNode& node, int indent = 2);
std::wstring JsonEscape(const std::wstring& s);
