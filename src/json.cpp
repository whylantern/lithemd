#include "json.h"
#include <cwchar>
#include <cmath>
#include <cstdlib>
#include <algorithm>

const JNode* JNode::Get(const std::wstring& key) const {
    for (const auto& m : members) if (m.first == key) return &m.second;
    return nullptr;
}
JNode* JNode::Get(const std::wstring& key) {
    for (auto& m : members) if (m.first == key) return &m.second;
    return nullptr;
}
const JNode* JNode::At(size_t i) const {
    return i < items.size() ? &items[i] : nullptr;
}
size_t JNode::Size() const {
    return kind == JKind::Array ? items.size() : (kind == JKind::Object ? members.size() : 0);
}
std::wstring JNode::Text() const {
    switch (kind) {
    case JKind::String: return str;
    case JKind::Number: return raw;
    case JKind::Bool: return boolean ? L"true" : L"false";
    default: return L"";
    }
}

namespace {

struct Parser {
    const std::wstring& s;
    size_t p = 0;
    std::wstring err;
    int depth = 0;

    explicit Parser(const std::wstring& text) : s(text) {}

    bool Eof() const { return p >= s.size(); }
    wchar_t Peek(size_t ahead = 0) const { return p + ahead < s.size() ? s[p + ahead] : 0; }

    void Skip() {
        for (;;) {
            while (p < s.size() && (s[p] == L' ' || s[p] == L'\t' || s[p] == L'\r' || s[p] == L'\n' || s[p] == 0xFEFF)) p++;
            if (p + 1 < s.size() && s[p] == L'/' && s[p + 1] == L'/') {
                p += 2;
                while (p < s.size() && s[p] != L'\n') p++;
                continue;
            }
            if (p + 1 < s.size() && s[p] == L'/' && s[p + 1] == L'*') {
                p += 2;
                while (p + 1 < s.size() && !(s[p] == L'*' && s[p + 1] == L'/')) p++;
                p = (p + 1 < s.size()) ? p + 2 : s.size();
                continue;
            }
            break;
        }
    }

    bool Fail(const wchar_t* msg) {
        if (err.empty()) {
            size_t line = 1, col = 1;
            for (size_t i = 0; i < p && i < s.size(); i++) {
                if (s[i] == L'\n') { line++; col = 1; } else col++;
            }
            err = std::wstring(msg) + L" (第 " + std::to_wstring(line) + L" 行, 第 " + std::to_wstring(col) + L" 列)";
        }
        return false;
    }

    bool ParseString(std::wstring& out) {
        if (Peek() != L'"') return Fail(L"应为字符串");
        p++;
        while (p < s.size()) {
            wchar_t c = s[p++];
            if (c == L'"') return true;
            if (c == L'\\') {
                if (p >= s.size()) return Fail(L"字符串转义不完整");
                wchar_t e = s[p++];
                switch (e) {
                case L'"': out += L'"'; break;
                case L'\\': out += L'\\'; break;
                case L'/': out += L'/'; break;
                case L'b': out += L'\b'; break;
                case L'f': out += L'\f'; break;
                case L'n': out += L'\n'; break;
                case L'r': out += L'\r'; break;
                case L't': out += L'\t'; break;
                case L'u': {
                    auto hex4 = [&](unsigned& v) -> bool {
                        if (p + 4 > s.size()) return false;
                        v = 0;
                        for (int i = 0; i < 4; i++) {
                            wchar_t h = s[p++];
                            v <<= 4;
                            if (h >= L'0' && h <= L'9') v |= (h - L'0');
                            else if (h >= L'a' && h <= L'f') v |= (h - L'a' + 10);
                            else if (h >= L'A' && h <= L'F') v |= (h - L'A' + 10);
                            else return false;
                        }
                        return true;
                    };
                    unsigned v = 0;
                    if (!hex4(v)) return Fail(L"\\u 转义无效");
                    if (v >= 0xD800 && v <= 0xDBFF) {
                        if (p + 6 <= s.size() && s[p] == L'\\' && s[p + 1] == L'u') {
                            p += 2;
                            unsigned lo = 0;
                            if (!hex4(lo)) return Fail(L"\\u 代理对无效");
                            if (lo >= 0xDC00 && lo <= 0xDFFF)
                                v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                            else v = 0xFFFD;
                        } else v = 0xFFFD;
                    }
                    if (v <= 0xFFFF) out += (wchar_t)v;
                    else {
                        v -= 0x10000;
                        out += (wchar_t)(0xD800 + (v >> 10));
                        out += (wchar_t)(0xDC00 + (v & 0x3FF));
                    }
                    break;
                }
                default: return Fail(L"未知的字符串转义");
                }
            } else {
                if (c < 0x20) return Fail(L"字符串中包含控制字符");
                out += c;
            }
        }
        return Fail(L"字符串没有结束");
    }

    bool ParseNumber(JNode& out) {
        size_t start = p;
        if (Peek() == L'-') p++;
        while (p < s.size() && s[p] >= L'0' && s[p] <= L'9') p++;
        if (Peek() == L'.') {
            p++;
            while (p < s.size() && s[p] >= L'0' && s[p] <= L'9') p++;
        }
        if (Peek() == L'e' || Peek() == L'E') {
            p++;
            if (Peek() == L'+' || Peek() == L'-') p++;
            while (p < s.size() && s[p] >= L'0' && s[p] <= L'9') p++;
        }
        if (p == start || (p == start + 1 && s[start] == L'-')) return Fail(L"数字格式无效");
        out.kind = JKind::Number;
        out.raw = s.substr(start, p - start);
        out.number = wcstod(out.raw.c_str(), nullptr);
        return true;
    }

    bool ParseValue(JNode& out) {
        if (++depth > 512) return Fail(L"JSON 嵌套过深");
        Skip();
        if (Eof()) return Fail(L"内容意外结束");
        wchar_t c = Peek();
        bool ok = false;
        if (c == L'{') {
            p++;
            out.kind = JKind::Object;
            Skip();
            if (Peek() == L'}') { p++; ok = true; }
            else {
                ok = true;
                for (;;) {
                    Skip();
                    std::wstring key;
                    if (!ParseString(key)) { ok = false; break; }
                    Skip();
                    if (Peek() != L':') { ok = Fail(L"对象成员缺少冒号"); break; }
                    p++;
                    JNode v;
                    if (!ParseValue(v)) { ok = false; break; }
                    out.members.emplace_back(std::move(key), std::move(v));
                    Skip();
                    if (Peek() == L',') {
                        p++;
                        Skip();
                        if (Peek() == L'}') { p++; break; } // trailing comma
                        continue;
                    }
                    if (Peek() == L'}') { p++; break; }
                    ok = Fail(L"对象成员之间缺少逗号");
                    break;
                }
            }
        } else if (c == L'[') {
            p++;
            out.kind = JKind::Array;
            Skip();
            if (Peek() == L']') { p++; ok = true; }
            else {
                ok = true;
                for (;;) {
                    JNode v;
                    if (!ParseValue(v)) { ok = false; break; }
                    out.items.push_back(std::move(v));
                    Skip();
                    if (Peek() == L',') {
                        p++;
                        Skip();
                        if (Peek() == L']') { p++; break; } // trailing comma
                        continue;
                    }
                    if (Peek() == L']') { p++; break; }
                    ok = Fail(L"数组元素之间缺少逗号");
                    break;
                }
            }
        } else if (c == L'"') {
            out.kind = JKind::String;
            ok = ParseString(out.str);
        } else if (c == L't' && s.compare(p, 4, L"true") == 0) {
            p += 4; out.kind = JKind::Bool; out.boolean = true; ok = true;
        } else if (c == L'f' && s.compare(p, 5, L"false") == 0) {
            p += 5; out.kind = JKind::Bool; out.boolean = false; ok = true;
        } else if (c == L'n' && s.compare(p, 4, L"null") == 0) {
            p += 4; out.kind = JKind::Null; ok = true;
        } else if (c == L'-' || (c >= L'0' && c <= L'9')) {
            ok = ParseNumber(out);
        } else {
            ok = Fail(L"无法识别的值");
        }
        depth--;
        return ok;
    }
};

void WriteString(std::wstring& out, const std::wstring& s) {
    out += L'"';
    for (wchar_t c : s) {
        switch (c) {
        case L'"': out += L"\\\""; break;
        case L'\\': out += L"\\\\"; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        case L'\t': out += L"\\t"; break;
        case L'\b': out += L"\\b"; break;
        case L'\f': out += L"\\f"; break;
        default:
            if (c < 0x20) {
                wchar_t buf[8];
                swprintf(buf, 8, L"\\u%04x", (unsigned)c);
                out += buf;
            } else out += c;
        }
    }
    out += L'"';
}

void WriteIndent(std::wstring& out, int n) { out.append((size_t)n, L' '); }

void WritePretty(std::wstring& out, const JNode& n, int indent, int level) {
    switch (n.kind) {
    case JKind::Null: out += L"null"; break;
    case JKind::Bool: out += n.boolean ? L"true" : L"false"; break;
    case JKind::Number: out += n.raw.empty() ? std::to_wstring(n.number) : n.raw; break;
    case JKind::String: WriteString(out, n.str); break;
    case JKind::Array:
        if (n.items.empty()) { out += L"[]"; break; }
        out += L"[\n";
        for (size_t i = 0; i < n.items.size(); i++) {
            WriteIndent(out, (level + 1) * indent);
            WritePretty(out, n.items[i], indent, level + 1);
            if (i + 1 < n.items.size()) out += L',';
            out += L'\n';
        }
        WriteIndent(out, level * indent); out += L']';
        break;
    case JKind::Object:
        if (n.members.empty()) { out += L"{}"; break; }
        out += L"{\n";
        for (size_t i = 0; i < n.members.size(); i++) {
            WriteIndent(out, (level + 1) * indent);
            WriteString(out, n.members[i].first);
            out += L": ";
            WritePretty(out, n.members[i].second, indent, level + 1);
            if (i + 1 < n.members.size()) out += L',';
            out += L'\n';
        }
        WriteIndent(out, level * indent); out += L'}';
        break;
    }
}

} // namespace

bool ParseJson(const std::wstring& text, JNode& out, std::wstring* error) {
    Parser ps(text);
    ps.Skip();
    if (ps.Eof()) {
        if (error) *error = L"内容为空";
        return false;
    }
    JNode node;
    bool ok = ps.ParseValue(node);
    if (ok) {
        ps.Skip();
        if (!ps.Eof()) ok = ps.Fail(L"根值之后还有多余内容");
    }
    if (!ok) {
        if (error) *error = ps.err.empty() ? L"JSON 解析失败" : ps.err;
        return false;
    }
    out = std::move(node);
    return true;
}

std::wstring PrettyJson(const JNode& node, int indent) {
    std::wstring out;
    WritePretty(out, node, indent, 0);
    return out;
}

std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    WriteString(out, s);
    return out;
}
