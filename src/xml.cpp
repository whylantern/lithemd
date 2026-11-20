#include "xml.h"
#include <cwctype>

std::wstring XmlNode::LocalName() const {
    size_t p = name.find(L':');
    return p == std::wstring::npos ? name : name.substr(p + 1);
}
bool XmlNode::Is(const std::wstring& local) const { return LocalName() == local; }
const XmlNode* XmlNode::Child(const std::wstring& local) const {
    for (auto& c : children) if (c->Is(local)) return c.get();
    return nullptr;
}
std::vector<const XmlNode*> XmlNode::Children(const std::wstring& local) const {
    std::vector<const XmlNode*> out;
    for (auto& c : children) if (c->Is(local)) out.push_back(c.get());
    return out;
}
std::wstring XmlNode::Attr(const std::wstring& n) const {
    for (auto& a : attrs) if (a.first == n) return a.second;
    return L"";
}
std::wstring XmlNode::AttrLocal(const std::wstring& local) const {
    for (auto& a : attrs) {
        size_t p = a.first.find(L':');
        std::wstring ln = p == std::wstring::npos ? a.first : a.first.substr(p + 1);
        if (ln == local) return a.second;
    }
    return L"";
}
std::wstring XmlNode::AllText() const {
    std::wstring out = text;
    for (auto& c : children) out += c->AllText();
    return out;
}

namespace {

struct XmlParser {
    const std::wstring& s;
    size_t p = 0;
    std::wstring err;

    explicit XmlParser(const std::wstring& t) : s(t) {}
    bool Eof() const { return p >= s.size(); }
    wchar_t Peek(size_t n = 0) const { return p + n < s.size() ? s[p + n] : 0; }
    bool Starts(const wchar_t* t) const {
        size_t n = wcslen(t);
        return s.compare(p, n, t) == 0;
    }
    bool Fail(const wchar_t* msg) {
        if (err.empty()) err = std::wstring(msg) + L"（位置 " + std::to_wstring(p) + L"）";
        return false;
    }
    void SkipWs() { while (!Eof() && iswspace(Peek())) p++; }

    void SkipComment() {
        size_t e = s.find(L"-->", p + 4);
        p = e == std::wstring::npos ? s.size() : e + 3;
    }
    void SkipPI() {
        size_t e = s.find(L"?>", p + 2);
        p = e == std::wstring::npos ? s.size() : e + 2;
    }
    void SkipDoctype() {
        int depth = 0;
        while (!Eof()) {
            wchar_t c = s[p++];
            if (c == L'[') depth++;
            else if (c == L']') depth--;
            else if (c == L'>' && depth <= 0) break;
        }
    }
    void SkipMisc() {
        for (;;) {
            SkipWs();
            if (Starts(L"<!--")) { SkipComment(); continue; }
            if (Starts(L"<?")) { SkipPI(); continue; }
            if (Starts(L"<!DOCTYPE") || Starts(L"<!doctype")) { SkipDoctype(); continue; }
            if (Starts(L"<![CDATA[")) return;
            break;
        }
    }

    std::wstring Decode(const std::wstring& raw) {
        std::wstring out;
        for (size_t i = 0; i < raw.size(); i++) {
            if (raw[i] != L'&') { out += raw[i]; continue; }
            size_t e = raw.find(L';', i + 1);
            if (e == std::wstring::npos || e - i > 12) { out += raw[i]; continue; }
            std::wstring name = raw.substr(i + 1, e - i - 1);
            if (name == L"amp") out += L'&';
            else if (name == L"lt") out += L'<';
            else if (name == L"gt") out += L'>';
            else if (name == L"quot") out += L'"';
            else if (name == L"apos") out += L'\'';
            else if (!name.empty() && name[0] == L'#') {
                unsigned v = 0;
                bool hex = name.size() > 1 && (name[1] == L'x' || name[1] == L'X');
                for (size_t k = hex ? 2 : 1; k < name.size(); k++) {
                    wchar_t c = name[k];
                    int d = (c >= L'0' && c <= L'9') ? c - L'0' :
                            (c >= L'a' && c <= L'f') ? c - L'a' + 10 :
                            (c >= L'A' && c <= L'F') ? c - L'A' + 10 : -1;
                    if (d < 0) { v = 0; break; }
                    v = v * (hex ? 16 : 10) + d;
                }
                if (v > 0 && v <= 0x10FFFF) {
                    if (v <= 0xFFFF) out += (wchar_t)v;
                    else {
                        v -= 0x10000;
                        out += (wchar_t)(0xD800 + (v >> 10));
                        out += (wchar_t)(0xDC00 + (v & 0x3FF));
                    }
                    i = e;
                    continue;
                }
            } else out += raw[i];
        }
        return out;
    }

    std::wstring ParseName() {
        size_t start = p;
        while (!Eof()) {
            wchar_t c = Peek();
            if (iswalnum(c) || c == L'_' || c == L':' || c == L'-' || c == L'.') p++;
            else break;
        }
        return s.substr(start, p - start);
    }

    bool ParseNode(XmlNode& node, int depth) {
        if (depth > 512) return Fail(L"XML 嵌套过深");
        if (Peek() != L'<') return Fail(L"应为元素");
        p++;
        if (Peek() == L'/') return Fail(L"意外的结束标签");
        std::wstring name = ParseName();
        if (name.empty()) return Fail(L"元素名称为空");
        node.name = name;
        for (;;) {
            SkipWs();
            if (Eof()) return Fail(L"标签没有结束");
            if (Peek() == L'>') { p++; break; }
            if (Peek() == L'/' && Peek(1) == L'>') { p += 2; return true; }
            std::wstring an = ParseName();
            if (an.empty()) return Fail(L"属性名称为空");
            SkipWs();
            if (Peek() != L'=') return Fail(L"属性缺少等号");
            p++;
            SkipWs();
            wchar_t q = Peek();
            if (q != L'"' && q != L'\'') return Fail(L"属性值缺少引号");
            p++;
            size_t start = p;
            while (!Eof() && Peek() != q) p++;
            if (Eof()) return Fail(L"属性值没有结束");
            std::wstring val = s.substr(start, p - start);
            p++;
            node.attrs.emplace_back(an, Decode(val));
        }
        // content
        for (;;) {
            if (Eof()) return Fail(L"元素没有结束");
            if (Starts(L"</")) {
                p += 2;
                std::wstring close = ParseName();
                SkipWs();
                if (Peek() != L'>') return Fail(L"结束标签格式错误");
                p++;
                if (close != node.name) return Fail(L"结束标签不匹配");
                return true;
            }
            if (Starts(L"<!--")) { SkipComment(); continue; }
            if (Starts(L"<?")) { SkipPI(); continue; }
            if (Starts(L"<![CDATA[")) {
                p += 9;
                size_t e = s.find(L"]]>", p);
                if (e == std::wstring::npos) return Fail(L"CDATA 没有结束");
                node.text += s.substr(p, e - p);
                p = e + 3;
                continue;
            }
            if (Peek() == L'<') {
                auto child = std::make_unique<XmlNode>();
                if (!ParseNode(*child, depth + 1)) return false;
                node.children.push_back(std::move(child));
                continue;
            }
            size_t start = p;
            while (!Eof() && Peek() != L'<') p++;
            node.text += Decode(s.substr(start, p - start));
        }
    }

    bool Parse(XmlNode& root) {
        if (s.size() >= 1 && s[0] == 0xFEFF) p++;
        SkipMisc();
        if (Peek() != L'<') return Fail(L"缺少 XML 根元素");
        if (!ParseNode(root, 0)) return false;
        SkipMisc();
        if (!Eof()) return Fail(L"根元素之后还有内容");
        return true;
    }
};

} // namespace

bool ParseXml(const std::wstring& text, XmlNode& root, std::wstring* error) {
    XmlParser p(text);
    root = XmlNode{};
    if (!p.Parse(root)) {
        if (error) *error = p.err.empty() ? L"XML 解析失败" : p.err;
        return false;
    }
    return true;
}
