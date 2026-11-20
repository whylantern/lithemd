#include "markdown.h"
#include "latex.h"
#include <algorithm>
#include <cwctype>
#include <map>
#include <set>

namespace {

std::wstring UnescapeUrl(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'%' && i + 2 < s.size()) {
            auto hv = [](wchar_t c) -> int {
                if (c >= L'0' && c <= L'9') return c - L'0';
                if (c >= L'a' && c <= L'f') return c - L'a' + 10;
                if (c >= L'A' && c <= L'F') return c - L'A' + 10;
                return -1;
            };
            int a = hv(s[i + 1]), b = hv(s[i + 2]);
            if (a >= 0 && b >= 0) {
                out += (wchar_t)((a << 4) | b);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

std::wstring HtmlEntity(const std::wstring& name) {
    if (name == L"amp") return L"&";
    if (name == L"lt") return L"<";
    if (name == L"gt") return L">";
    if (name == L"quot") return L"\"";
    if (name == L"apos") return L"'";
    if (name == L"nbsp") return L" ";
    if (name == L"hellip") return L"…";
    if (name == L"mdash") return L"—";
    if (name == L"ndash") return L"–";
    if (name == L"copy") return L"©";
    if (name == L"reg") return L"®";
    if (name == L"trade") return L"™";
    if (name == L"times") return L"×";
    if (name == L"divide") return L"÷";
    if (name == L"rarr") return L"→";
    if (name == L"larr") return L"←";
    if (name == L"harr") return L"↔";
    if (name == L"le") return L"≤";
    if (name == L"ge") return L"≥";
    if (name == L"ne") return L"≠";
    if (name == L"deg") return L"°";
    if (name == L"middot") return L"·";
    return L"&" + name + L";";
}

std::wstring DecodeEntities(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != L'&') { out += s[i]; continue; }
        size_t semi = s.find(L';', i + 1);
        if (semi == std::wstring::npos || semi - i > 12) { out += s[i]; continue; }
        std::wstring name = s.substr(i + 1, semi - i - 1);
        if (!name.empty() && name[0] == L'#') {
            unsigned v = 0;
            bool hex = name.size() > 1 && (name[1] == L'x' || name[1] == L'X');
            size_t start = hex ? 2 : 1;
            for (size_t k = start; k < name.size(); k++) {
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
                i = semi;
                continue;
            }
        } else {
            std::wstring e = HtmlEntity(name);
            if (e != L"&" + name + L";") {
                out += e;
                i = semi;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

bool IsAlnum(wchar_t c) { return iswalnum(c) != 0; }

void AddRun(std::vector<Inline>& out, Inline run) {
    if (run.text.empty() && !run.math) return;
    if (!run.math && !out.empty()) {
        Inline& prev = out.back();
        if (!prev.math && prev.bold == run.bold && prev.italic == run.italic &&
            prev.underline == run.underline && prev.strike == run.strike &&
            prev.code == run.code && prev.link == run.link && prev.url == run.url &&
            prev.role == run.role) {
            prev.text += run.text;
            return;
        }
    }
    out.push_back(std::move(run));
}

std::wstring TrimCodeSpan(const std::wstring& s) {
    if (s.size() >= 2 && s.front() == L' ' && s.back() == L' ' && Trim(s).size() > 0)
        return s.substr(1, s.size() - 2);
    return s;
}

std::wstring StripHtmlTag(const std::wstring& tag) {
    std::wstring t = ToLower(tag);
    if (t == L"<br>" || t == L"<br/>" || t == L"<br />") return L"\n";
    return L"";
}

struct InlineParser {
    const MarkdownContext& ctx;
    const std::map<std::wstring, std::wstring>* refs;

    InlineParser(const MarkdownContext& c, const std::map<std::wstring, std::wstring>* r)
        : ctx(c), refs(r) {}

    std::vector<Inline> Parse(const std::wstring& s, int from, int to,
                              bool bold = false, bool italic = false, bool strike = false,
                              bool link = false, const std::wstring& url = L"") {
        std::vector<Inline> out;
        std::wstring buf;
        auto flush = [&]() {
            if (!buf.empty()) {
                Inline r;
                r.text = std::move(buf);
                buf.clear();
                r.bold = bold; r.italic = italic; r.strike = strike; r.link = link; r.url = url;
                AddRun(out, std::move(r));
            }
        };
        int i = from;
        while (i < to) {
            wchar_t c = s[(size_t)i];
            if (c == L'\\' && i + 1 < to) {
                wchar_t n = s[(size_t)i + 1];
                if (n == L'(' || n == L'[') {
                    wchar_t closeChar = n == L'(' ? L')' : L']';
                    size_t close = s.find(std::wstring(L"\\") + closeChar, (size_t)i + 2);
                    if (close != std::wstring::npos && (int)close < to) {
                        std::wstring tex = s.substr((size_t)i + 2, close - (size_t)i - 2);
                        auto node = ParseMath(tex, closeChar == L']');
                        if (node) {
                            flush();
                            Inline r;
                            r.math = node;
                            AddRun(out, std::move(r));
                            i = (int)close + 2;
                            continue;
                        }
                    }
                }
                if (n == L'\n') { flush(); buf += L"\n"; i += 2; continue; }
                buf += n;
                i += 2;
                continue;
            }
            if (c == L'\n') {
                size_t spaces = 0;
                while (spaces < buf.size() && buf[buf.size() - 1 - spaces] == L' ') spaces++;
                bool hard = spaces >= 2 || (i > 0 && s[(size_t)i - 1] == L'\\');
                if (hard) {
                    while (!buf.empty() && buf.back() == L' ') buf.pop_back();
                    buf += L"\n";
                } else {
                    while (!buf.empty() && buf.back() == L' ') buf.pop_back();
                    buf += L' ';
                }
                i++;
                continue;
            }
            if (c == L'`') {
                int n = 0;
                while (i + n < to && s[(size_t)i + n] == L'`') n++;
                std::wstring fence((size_t)n, L'`');
                size_t close = s.find(fence, (size_t)i + n);
                if (close != std::wstring::npos && (int)close < to) {
                    flush();
                    Inline r;
                    r.text = TrimCodeSpan(s.substr((size_t)i + n, close - (size_t)i - n));
                    r.code = true;
                    r.role = InlineRole::CodeString;
                    AddRun(out, std::move(r));
                    i = (int)close + n;
                    continue;
                }
            }
            if (c == L'!' && i + 1 < to && s[(size_t)i + 1] == L'[') {
                int closeBracket = -1;
                for (int k = i + 2, depth = 1; k < to; k++) {
                    if (s[(size_t)k] == L'[') depth++;
                    else if (s[(size_t)k] == L']') { depth--; if (depth == 0) { closeBracket = k; break; } }
                }
                if (closeBracket > 0 && closeBracket + 1 < to && s[(size_t)closeBracket + 1] == L'(') {
                    int closeParen = -1;
                    for (int k = closeBracket + 2, depth = 1; k < to; k++) {
                        if (s[(size_t)k] == L'(') depth++;
                        else if (s[(size_t)k] == L')') { depth--; if (depth == 0) { closeParen = k; break; } }
                    }
                    if (closeParen > 0) {
                        flush();
                        Inline r;
                        r.text = L"🖼 " + s.substr((size_t)i + 2, (size_t)closeBracket - i - 2);
                        r.role = InlineRole::Muted;
                        r.italic = true;
                        AddRun(out, std::move(r));
                        i = closeParen + 1;
                        continue;
                    }
                }
            }
            if (c == L'[') {
                int closeBracket = -1;
                for (int k = i + 1, depth = 1; k < to; k++) {
                    if (s[(size_t)k] == L'[') depth++;
                    else if (s[(size_t)k] == L']') { depth--; if (depth == 0) { closeBracket = k; break; } }
                }
                if (closeBracket > 0) {
                    std::wstring label = s.substr((size_t)i + 1, (size_t)closeBracket - i - 1);
                    std::wstring target;
                    int next = closeBracket + 1;
                    if (next < to && s[(size_t)next] == L'(') {
                        int closeParen = -1;
                        for (int k = next + 1, depth = 1; k < to; k++) {
                            if (s[(size_t)k] == L'(') depth++;
                            else if (s[(size_t)k] == L')') { depth--; if (depth == 0) { closeParen = k; break; } }
                        }
                        if (closeParen > 0) {
                            std::wstring inside = Trim(s.substr((size_t)next + 1, (size_t)closeParen - next - 1));
                            size_t sp = inside.find_first_of(L" \t");
                            target = sp == std::wstring::npos ? inside : inside.substr(0, sp);
                            if (!target.empty() && (target.front() == L'<' && target.back() == L'>'))
                                target = target.substr(1, target.size() - 2);
                            next = closeParen + 1;
                        }
                    } else if (next < to && s[(size_t)next] == L'[') {
                        int close2 = (int)s.find(L']', (size_t)next + 1);
                        if (close2 > 0 && close2 < to) {
                            std::wstring id = s.substr((size_t)next + 1, (size_t)close2 - next - 1);
                            if (id.empty()) id = label;
                            if (refs) {
                                auto it = refs->find(ToLower(Trim(id)));
                                if (it != refs->end()) target = it->second;
                            }
                            next = close2 + 1;
                        }
                    } else if (refs) {
                        auto it = refs->find(ToLower(Trim(label)));
                        if (it != refs->end()) {
                            target = it->second;
                            next = closeBracket + 1;
                        }
                    }
                    if (!target.empty()) {
                        flush();
                        std::vector<Inline> inner = Parse(s, i + 1, closeBracket, bold, italic, strike,
                                                          true, UnescapeUrl(target));
                        out.insert(out.end(), inner.begin(), inner.end());
                        i = next;
                        continue;
                    }
                }
            }
            if ((c == L'*' || c == L'_' || c == L'~') && i + 1 < to) {
                bool doubleDelim = s[(size_t)i + 1] == c;
                if (c == L'_' && i > 0 && i + 1 < to && IsAlnum(s[(size_t)i - 1]) && IsAlnum(s[(size_t)i + 1])) {
                    buf += c; i++; continue;
                }
                int n = doubleDelim ? 2 : 1;
                if (c == L'~' && !doubleDelim) { buf += c; i++; continue; }
                size_t close = s.find(std::wstring((size_t)n, c), (size_t)i + n);
                if (close != std::wstring::npos && (int)close + n <= to && close > (size_t)i + n) {
                    flush();
                    bool nb = bold || (doubleDelim && c != L'~');
                    bool ni = italic || (!doubleDelim);
                    bool ns = strike || (c == L'~');
                    std::vector<Inline> inner = Parse(s, i + n, (int)close, nb, ni, ns, link, url);
                    out.insert(out.end(), inner.begin(), inner.end());
                    i = (int)close + n;
                    continue;
                }
            }
            if (c == L'$' && i + 1 < to) {
                if (s[(size_t)i + 1] == L'$') {
                    size_t close = s.find(L"$$", (size_t)i + 2);
                    if (close != std::wstring::npos && (int)close < to) {
                        auto node = ParseMath(s.substr((size_t)i + 2, close - (size_t)i - 2), true);
                        if (node) {
                            flush();
                            Inline r;
                            r.math = node;
                            AddRun(out, std::move(r));
                            i = (int)close + 2;
                            continue;
                        }
                    }
                } else {
                    size_t close = s.find(L'$', (size_t)i + 1);
                    if (close != std::wstring::npos && (int)close < to && close > (size_t)i + 1 &&
                        !iswspace(s[close - 1])) {
                        std::wstring tex = s.substr((size_t)i + 1, close - (size_t)i - 1);
                        auto node = ParseMath(tex, false);
                        if (node) {
                            flush();
                            Inline r;
                            r.math = node;
                            AddRun(out, std::move(r));
                            i = (int)close + 1;
                            continue;
                        }
                    }
                }
            }
            if (c == L'<') {
                size_t close = s.find(L'>', (size_t)i + 1);
                if (close != std::wstring::npos && (int)close < to) {
                    std::wstring inside = s.substr((size_t)i + 1, close - (size_t)i - 1);
                    if (StartsWithI(inside, L"http://") || StartsWithI(inside, L"https://") ||
                        StartsWithI(inside, L"mailto:")) {
                        flush();
                        Inline r;
                        r.text = inside;
                        r.link = true;
                        r.url = inside;
                        r.role = InlineRole::Accent;
                        r.underline = true;
                        AddRun(out, std::move(r));
                        i = (int)close + 1;
                        continue;
                    }
                    std::wstring br = StripHtmlTag(s.substr((size_t)i, close - (size_t)i + 1));
                    if (!br.empty()) { flush(); buf += br; }
                    i = (int)close + 1;
                    continue;
                }
            }
            if (c == L'&') {
                size_t semi = s.find(L';', (size_t)i + 1);
                if (semi != std::wstring::npos && semi - (size_t)i <= 12 && (int)semi < to) {
                    std::wstring ent = s.substr((size_t)i, semi - (size_t)i + 1);
                    std::wstring dec = DecodeEntities(ent);
                    if (dec != ent) { buf += dec; i = (int)semi + 1; continue; }
                }
            }
            if ((c == L'h' || c == L'H' || c == L'w' || c == L'W') &&
                (i == 0 || !IsAlnum(s[(size_t)i - 1]))) {
                std::wstring rest = s.substr((size_t)i, (size_t)std::min(to - i, 2048));
                bool web = StartsWithI(rest, L"http://") || StartsWithI(rest, L"https://");
                bool www = StartsWithI(rest, L"www.");
                if (web || www) {
                    size_t end = 0;
                    while (end < rest.size() && !iswspace(rest[end]) && rest[end] != L')' &&
                           rest[end] != L']' && rest[end] != L'<' && rest[end] != L'>' &&
                           rest[end] != L'"' && rest[end] != L'\'') end++;
                    while (end > 0 && (rest[end - 1] == L'.' || rest[end - 1] == L',' ||
                                       rest[end - 1] == L';' || rest[end - 1] == L':' ||
                                       rest[end - 1] == L'!' || rest[end - 1] == L'?')) end--;
                    if (end > 4) {
                        flush();
                        Inline r;
                        r.text = rest.substr(0, end);
                        r.link = true;
                        r.url = www ? L"http://" + r.text : r.text;
                        r.role = InlineRole::Accent;
                        r.underline = true;
                        AddRun(out, std::move(r));
                        i += (int)end;
                        continue;
                    }
                }
            }
            buf += c;
            i++;
        }
        flush();
        return out;
    }
};

bool IsBlank(const std::wstring& s) { return Trim(s).empty(); }

bool IsFence(const std::wstring& line, std::wstring& info, wchar_t& fenceChar, int& fenceLen) {
    std::wstring t = TrimLeft(line);
    if (t.size() < 3) return false;
    wchar_t c = t[0];
    if (c != L'`' && c != L'~') return false;
    int n = 0;
    while (n < (int)t.size() && t[(size_t)n] == c) n++;
    if (n < 3) return false;
    fenceChar = c;
    fenceLen = n;
    info = Trim(t.substr((size_t)n));
    return true;
}

bool IsAtxHeading(const std::wstring& line, int& level, std::wstring& text) {
    size_t i = 0;
    while (i < line.size() && i < 4 && line[i] == L' ') i++;
    if (i > 3 || i >= line.size() || line[i] != L'#') return false;
    int n = 0;
    while (i < line.size() && line[i] == L'#') { n++; i++; }
    if (n > 6) return false;
    if (i < line.size() && line[i] != L' ' && line[i] != L'\t') return false;
    while (i < line.size() && (line[i] == L' ' || line[i] == L'\t')) i++;
    std::wstring t = TrimRight(line.substr(i));
    while (!t.empty() && t.back() == L'#') {
        t.pop_back();
        if (!t.empty() && t.back() == L' ') { t.pop_back(); break; }
    }
    level = n;
    text = TrimRight(t);
    return true;
}

bool IsSetext(const std::wstring& line, int& level) {
    std::wstring t = Trim(line);
    if (t.size() < 2) return false;
    wchar_t c = t[0];
    if (c != L'=' && c != L'-') return false;
    for (wchar_t x : t) if (x != c) return false;
    level = c == L'=' ? 1 : 2;
    return true;
}

bool IsHr(const std::wstring& line) {
    std::wstring t = Trim(line);
    if (t.size() < 3) return false;
    wchar_t c = t[0];
    if (c != L'-' && c != L'*' && c != L'_') return false;
    int n = 0;
    for (wchar_t x : t) {
        if (x == c) n++;
        else if (x != L' ' && x != L'\t') return false;
    }
    return n >= 3;
}

struct ListStart {
    bool ok = false;
    int indent = 0;
    bool ordered = false;
    int number = 0;
    bool task = false;
    bool checked = false;
    std::wstring rest;
};

ListStart ParseListStart(const std::wstring& line) {
    ListStart r;
    size_t i = 0;
    while (i < line.size() && (line[i] == L' ' || line[i] == L'\t')) i++;
    if (i >= line.size()) return r;
    int spaces = (int)i;
    wchar_t c = line[i];
    if (c == L'-' || c == L'+' || c == L'*') {
        if (i + 1 < line.size() && (line[i + 1] == L' ' || line[i + 1] == L'\t')) {
            r.ok = true;
            r.indent = spaces / 2;
            r.rest = TrimLeft(line.substr(i + 1));
        }
    } else if (iswdigit(c)) {
        size_t j = i;
        long n = 0;
        while (j < line.size() && iswdigit(line[j]) && j - i < 9) { n = n * 10 + (line[j] - L'0'); j++; }
        if (j < line.size() && (line[j] == L'.' || line[j] == L')') &&
            j + 1 < line.size() && (line[j + 1] == L' ' || line[j + 1] == L'\t')) {
            r.ok = true;
            r.ordered = true;
            r.number = (int)n;
            r.indent = spaces / 2;
            r.rest = TrimLeft(line.substr(j + 1));
        }
    }
    if (r.ok && r.rest.size() >= 4 && r.rest[0] == L'[' && r.rest[2] == L']' &&
        (r.rest[3] == L' ' || r.rest[3] == L'\t')) {
        if (r.rest[1] == L' ' || r.rest[1] == L'x' || r.rest[1] == L'X') {
            r.task = true;
            r.checked = (r.rest[1] == L'x' || r.rest[1] == L'X');
            r.rest = TrimLeft(r.rest.substr(3));
        }
    }
    return r;
}

bool IsTableSeparator(const std::wstring& line, std::vector<int>& aligns) {
    std::wstring t = Trim(line);
    if (t.find(L'-') == std::wstring::npos) return false;
    std::vector<std::wstring> cells;
    std::wstring cur;
    bool esc = false;
    for (wchar_t c : t) {
        if (esc) { cur += c; esc = false; continue; }
        if (c == L'\\') { esc = true; cur += c; continue; }
        if (c == L'|') { cells.push_back(cur); cur.clear(); }
        else cur += c;
    }
    cells.push_back(cur);
    if (!cells.empty() && Trim(cells.front()).empty()) cells.erase(cells.begin());
    if (!cells.empty() && Trim(cells.back()).empty()) cells.pop_back();
    if (cells.empty()) return false;
    aligns.clear();
    for (auto& cellRaw : cells) {
        std::wstring cell = Trim(cellRaw);
        if (cell.empty()) return false;
        bool left = !cell.empty() && cell.front() == L':';
        bool right = !cell.empty() && cell.back() == L':';
        std::wstring core = cell;
        if (left) core = core.substr(1);
        if (right && !core.empty()) core.pop_back();
        if (core.size() < 3) return false;
        for (wchar_t c : core) if (c != L'-') return false;
        aligns.push_back(left && right ? 0 : (right ? 1 : (left ? -1 : 0)));
    }
    return true;
}

std::vector<std::wstring> SplitTableRow(const std::wstring& line) {
    std::wstring t = Trim(line);
    std::vector<std::wstring> cells;
    std::wstring cur;
    bool esc = false;
    bool math = false;
    bool code = false;
    for (size_t i = 0; i < t.size(); i++) {
        wchar_t c = t[i];
        if (esc) {
            if (c == L'|') cur += L'|';
            else { cur += L'\\'; cur += c; }
            // \(...\) and \[...\] protect any pipes inside the formula.
            if (c == L'(' || c == L'[') math = true;
            else if (c == L')' || c == L']') math = false;
            esc = false;
            continue;
        }
        if (c == L'\\') { esc = true; continue; }
        if (c == L'$') { math = !math; cur += c; continue; }
        if (c == L'`') { code = !code; cur += c; continue; }
        if (c == L'|' && !math && !code) { cells.push_back(Trim(cur)); cur.clear(); }
        else cur += c;
    }
    if (esc) cur += L'\\';
    cells.push_back(Trim(cur));
    if (!cells.empty() && cells.front().empty()) cells.erase(cells.begin());
    if (!cells.empty() && cells.back().empty()) cells.pop_back();
    return cells;
}

std::wstring ParseLinkTarget(const std::wstring& insideRaw) {
    std::wstring inside = Trim(insideRaw);
    if (!inside.empty() && inside.front() == L'<') {
        size_t e = inside.find(L'>');
        if (e != std::wstring::npos) return UnescapeUrl(inside.substr(1, e - 1));
    }
    size_t sp = inside.find_first_of(L" \t");
    std::wstring target = sp == std::wstring::npos ? inside : inside.substr(0, sp);
    return UnescapeUrl(target);
}

std::shared_ptr<ImageRef> MakeImage(const std::wstring& targetRaw, const std::wstring& alt,
                                    const MarkdownContext& ctx) {
    std::wstring target = UnescapeUrl(Trim(targetRaw));
    if (target.empty()) return nullptr;
    auto img = std::make_shared<ImageRef>();
    img->alt = alt;
    if (StartsWithI(target, L"data:")) {
        size_t comma = target.find(L',');
        if (comma == std::wstring::npos) return nullptr;
        std::wstring header = target.substr(5, comma - 5);
        size_t semi = header.find(L';');
        img->mime = semi == std::wstring::npos ? header : header.substr(0, semi);
        std::wstring data = target.substr(comma + 1);
        if (header.find(L"base64") != std::wstring::npos) img->bytes = Base64DecodeBytes(data);
        else {
            std::string u = Utf8FromWide(data);
            img->bytes.assign(u.begin(), u.end());
        }
        img->cacheKey = L"data:" + img->mime + L":" + std::to_wstring(img->bytes.size());
        return img;
    }
    if (StartsWithI(target, L"attachment:") && ctx.attachments) {
        std::wstring key = target.substr(11);
        auto it = ctx.attachments->find(key);
        if (it == ctx.attachments->end()) {
            for (auto& kv : *ctx.attachments)
                if (_wcsicmp(kv.first.c_str(), key.c_str()) == 0) { it = ctx.attachments->find(kv.first); break; }
        }
        if (it != ctx.attachments->end()) {
            img->bytes = it->second;
            img->cacheKey = L"attachment:" + key + L":" + std::to_wstring(img->bytes.size());
            return img;
        }
        return nullptr;
    }
    if (StartsWithI(target, L"file:///")) {
        target = target.substr(8);
        target = ReplaceAll(target, L"/", L"\\");
    }
    if (!IsWebUrl(target) && !(target.size() > 1 && target[1] == L':') &&
        !(target.size() > 0 && (target[0] == L'\\' || target[0] == L'/'))) {
        target = PathJoin(ctx.baseDir, target);
    }
    img->path = target;
    img->cacheKey = target;
    return img;
}

bool StandaloneImage(const std::wstring& line, std::wstring& alt, std::wstring& target) {
    std::wstring t = Trim(line);
    if (t.size() < 5 || t[0] != L'!' || t[1] != L'[') return false;
    int close = -1;
    for (int i = 2, depth = 1; i < (int)t.size(); i++) {
        if (t[(size_t)i] == L'[') depth++;
        else if (t[(size_t)i] == L']') { depth--; if (depth == 0) { close = i; break; } }
    }
    if (close < 0 || close + 1 >= (int)t.size() || t[(size_t)close + 1] != L'(') return false;
    if (t.back() != L')') return false;
    alt = t.substr(2, (size_t)close - 2);
    target = ParseLinkTarget(t.substr((size_t)close + 2, t.size() - (size_t)close - 3));
    return true;
}

std::wstring HeadingAnchor(const std::wstring& text, std::set<std::wstring>& used) {
    std::wstring base = Slugify(text);
    if (base.empty()) base = L"section";
    std::wstring anchor = base;
    int n = 2;
    while (used.count(anchor)) anchor = base + L"-" + std::to_wstring(n++);
    used.insert(anchor);
    return anchor;
}

} // namespace

std::wstring Slugify(const std::wstring& text) {
    std::wstring out;
    for (wchar_t c : text) {
        if (iswalnum(c)) out += (wchar_t)towlower(c);
        else if (c == L' ' || c == L'-' || c == L'_') {
            if (!out.empty() && out.back() != L'-') out += L'-';
        }
    }
    while (!out.empty() && out.back() == L'-') out.pop_back();
    return out;
}

std::vector<Inline> ParseMarkdownInlines(const std::wstring& text, const MarkdownContext& ctx,
                                         const std::map<std::wstring, std::wstring>* refs) {
    InlineParser p(ctx, refs);
    return p.Parse(text, 0, (int)text.size());
}

namespace {

constexpr int kMaxMarkdownBlockDepth = 64;

std::vector<Block> ParseBlocks(const std::vector<std::wstring>& lines, const MarkdownContext& ctx,
                               const std::map<std::wstring, std::wstring>& refs,
                               std::set<std::wstring>& anchors, bool quoted, int depth = 0) {
    std::vector<Block> out;
    auto parseInlines = [&](const std::wstring& t) {
        return ParseMarkdownInlines(t, ctx, &refs);
    };

    int i = 0;
    int n = (int)lines.size();
    while (i < n) {
        std::wstring line = lines[(size_t)i];
        if (IsBlank(line)) { i++; continue; }

        // Fenced code
        {
            std::wstring info;
            wchar_t fenceChar = 0;
            int fenceLen = 0;
            if (IsFence(line, info, fenceChar, fenceLen)) {
                Block b;
                b.kind = BlockKind::Code;
                b.quote = quoted;
                b.lang = Trim(info);
                size_t sp = b.lang.find_first_of(L" \t{");
                if (sp != std::wstring::npos) b.lang = b.lang.substr(0, sp);
                std::wstring code;
                i++;
                while (i < n) {
                    std::wstring t = TrimLeft(lines[(size_t)i]);
                    int cnt = 0;
                    while (cnt < (int)t.size() && t[(size_t)cnt] == fenceChar) cnt++;
                    if (cnt >= fenceLen) { i++; break; }
                    if (!code.empty()) code += L"\n";
                    code += lines[(size_t)i];
                    i++;
                }
                b.text = code;
                b.inlines = HighlightCode(code, b.lang);
                out.push_back(std::move(b));
                continue;
            }
        }

        // Display math: $$ ... $$ or \[ ... \]
        {
            std::wstring t = Trim(line);
            bool dollar = StartsWithI(t, L"$$");
            bool bracket = StartsWithI(t, L"\\[");
            if (dollar || bracket) {
                std::wstring close = dollar ? L"$$" : L"\\]";
                std::wstring body = t.substr(2);
                bool closed = false;
                if (body.size() >= 2 && body.substr(body.size() - 2) == close) {
                    body = body.substr(0, body.size() - 2);
                    closed = true;
                    i++;
                } else {
                    i++;
                    while (i < n) {
                        std::wstring l = lines[(size_t)i];
                        size_t p = l.find(close);
                        if (p != std::wstring::npos) {
                            body += L"\n" + l.substr(0, p);
                            closed = true;
                            i++;
                            break;
                        }
                        body += L"\n" + l;
                        i++;
                    }
                }
                if (closed) {
                    auto node = ParseMath(body, true);
                    if (node) {
                        Block b;
                        b.kind = BlockKind::Math;
                        b.math = node;
                        b.quote = quoted;
                        out.push_back(std::move(b));
                        continue;
                    }
                }
            }
        }

        int level = 0;
        std::wstring headingText;
        if (IsAtxHeading(line, level, headingText)) {
            Block b;
            b.kind = BlockKind::Heading;
            b.level = level;
            b.quote = quoted;
            b.inlines = parseInlines(headingText);
            b.text = headingText;
            b.anchor = HeadingAnchor(headingText, anchors);
            out.push_back(std::move(b));
            i++;
            continue;
        }
        if (i + 1 < n && IsSetext(lines[(size_t)i + 1], level) && !Trim(line).empty()) {
            Block b;
            b.kind = BlockKind::Heading;
            b.level = level;
            b.quote = quoted;
            b.text = Trim(line);
            b.inlines = parseInlines(b.text);
            b.anchor = HeadingAnchor(b.text, anchors);
            out.push_back(std::move(b));
            i += 2;
            continue;
        }
        if (IsHr(line)) {
            Block b;
            b.kind = BlockKind::Hr;
            b.quote = quoted;
            out.push_back(std::move(b));
            i++;
            continue;
        }

        // Block quote
        if (TrimLeft(line).size() > 0 && TrimLeft(line)[0] == L'>') {
            if (depth >= kMaxMarkdownBlockDepth) {
                // Extremely deep quote nesting would otherwise recurse until
                // the stack overflows. Flatten the remaining markers to text.
                std::wstring flat = TrimLeft(line);
                while (!flat.empty() && flat[0] == L'>') {
                    flat.erase(flat.begin());
                    if (!flat.empty() && flat[0] == L' ') flat.erase(flat.begin());
                }
                Block b;
                b.kind = BlockKind::Paragraph;
                b.quote = true;
                b.text = flat;
                b.inlines = parseInlines(flat);
                out.push_back(std::move(b));
                i++;
                continue;
            }
            std::vector<std::wstring> inner;
            while (i < n) {
                std::wstring l = lines[(size_t)i];
                std::wstring tl = TrimLeft(l);
                if (!tl.empty() && tl[0] == L'>') {
                    std::wstring rest = tl.substr(1);
                    if (!rest.empty() && rest[0] == L' ') rest = rest.substr(1);
                    inner.push_back(rest);
                    i++;
                } else if (IsBlank(l)) {
                    if (i + 1 < n) {
                        std::wstring nx = TrimLeft(lines[(size_t)i + 1]);
                        if (!nx.empty() && nx[0] == L'>') { inner.push_back(L""); i++; continue; }
                    }
                    break;
                } else break;
            }
            std::vector<Block> sub = ParseBlocks(inner, ctx, refs, anchors, true, depth + 1);
            for (auto& b : sub) { b.quote = true; out.push_back(std::move(b)); }
            continue;
        }

        // Table
        if (line.find(L'|') != std::wstring::npos && i + 1 < n) {
            std::vector<int> aligns;
            if (IsTableSeparator(lines[(size_t)i + 1], aligns)) {
                Block b;
                b.kind = BlockKind::Table;
                b.quote = quoted;
                b.table.aligns = aligns;
                std::vector<std::wstring> header = SplitTableRow(line);
                std::vector<TableCell> hrow;
                for (size_t c = 0; c < header.size(); c++) {
                    TableCell cell;
                    cell.header = true;
                    cell.align = c < aligns.size() ? aligns[c] : 0;
                    cell.inlines = parseInlines(header[c]);
                    hrow.push_back(std::move(cell));
                }
                b.table.rows.push_back(std::move(hrow));
                i += 2;
                while (i < n && !IsBlank(lines[(size_t)i]) && lines[(size_t)i].find(L'|') != std::wstring::npos) {
                    std::vector<std::wstring> row = SplitTableRow(lines[(size_t)i]);
                    std::vector<TableCell> cells;
                    size_t colCount = b.table.rows[0].size();
                    for (size_t c = 0; c < colCount; c++) {
                        TableCell cell;
                        cell.align = c < aligns.size() ? aligns[c] : 0;
                        if (c < row.size()) cell.inlines = parseInlines(row[c]);
                        cells.push_back(std::move(cell));
                    }
                    b.table.rows.push_back(std::move(cells));
                    i++;
                }
                out.push_back(std::move(b));
                continue;
            }
        }

        // List item
        ListStart ls = ParseListStart(line);
        if (ls.ok) {
            Block b;
            b.kind = BlockKind::ListItem;
            b.quote = quoted;
            b.indent = ls.indent;
            b.ordered = ls.ordered;
            b.number = ls.number;
            b.task = ls.task;
            b.checked = ls.checked;
            std::wstring item = ls.rest;
            i++;
            // simple continuation lines
            while (i < n) {
                std::wstring l = lines[(size_t)i];
                if (IsBlank(l)) break;
                ListStart nx = ParseListStart(l);
                if (nx.ok) break;
                {
                    std::wstring info2;
                    wchar_t fc2 = 0;
                    int fl2 = 0;
                    if (IsAtxHeading(l, level, headingText) || IsFence(l, info2, fc2, fl2)) break;
                }
                if (!l.empty() && l[0] != L' ' && l[0] != L'\t') break;
                item += L"\n" + Trim(l);
                i++;
            }
            b.text = item;
            b.inlines = parseInlines(item);
            out.push_back(std::move(b));
            continue;
        }

        // Standalone image
        {
            std::wstring alt, target;
            if (StandaloneImage(line, alt, target)) {
                auto img = MakeImage(target, alt, ctx);
                if (img) {
                    Block b;
                    b.kind = BlockKind::Image;
                    b.quote = quoted;
                    b.image = img;
                    b.text = alt;
                    out.push_back(std::move(b));
                    i++;
                    continue;
                }
            }
        }

        // Paragraph
        std::wstring para;
        while (i < n) {
            std::wstring l = lines[(size_t)i];
            if (IsBlank(l)) break;
            if (!para.empty()) {
                if (IsAtxHeading(l, level, headingText) || IsHr(l)) break;
                std::wstring info;
                wchar_t fc; int fl;
                if (IsFence(l, info, fc, fl)) break;
                ListStart lx = ParseListStart(l);
                if (lx.ok) break;
                std::wstring tl = TrimLeft(l);
                if (!tl.empty() && tl[0] == L'>') break;
                if (l.find(L'|') != std::wstring::npos && i + 1 < n) {
                    std::vector<int> al;
                    if (IsTableSeparator(lines[(size_t)i + 1], al)) break;
                }
                std::wstring a, t;
                if (StandaloneImage(l, a, t)) break;
            }
            if (!para.empty()) para += L"\n";
            para += l;
            i++;
        }
        Block b;
        b.kind = BlockKind::Paragraph;
        b.quote = quoted;
        b.text = para;
        b.inlines = parseInlines(para);
        out.push_back(std::move(b));
    }
    return out;
}

} // namespace

std::vector<Block> ParseMarkdown(const std::wstring& text, const MarkdownContext& ctx) {
    std::wstring src = NormalizeNewlines(text);
    // Strip UTF-8 BOM-ish leading marker.
    if (!src.empty() && src[0] == 0xFEFF) src.erase(0, 1);
    std::vector<std::wstring> lines = SplitLines(src);

    std::map<std::wstring, std::wstring> refs;
    std::vector<std::wstring> filtered;
    filtered.reserve(lines.size());
    // Extract link reference definitions outside code fences.
    bool inFence = false;
    wchar_t fenceChar = 0;
    int fenceLen = 0;
    for (auto& l : lines) {
        std::wstring info;
        wchar_t fc = 0; int fl = 0;
        if (IsFence(l, info, fc, fl)) {
            inFence = !inFence;
            fenceChar = fc; fenceLen = fl;
            filtered.push_back(l);
            continue;
        }
        if (!inFence) {
            std::wstring t = TrimLeft(l);
            if (t.size() > 3 && t[0] == L'[') {
                size_t close = t.find(L"]:");
                if (close != std::wstring::npos) {
                    std::wstring id = ToLower(Trim(t.substr(1, close - 1)));
                    std::wstring rest = Trim(t.substr(close + 2));
                    std::wstring target = ParseLinkTarget(rest);
                    if (!id.empty() && !target.empty()) {
                        refs[id] = target;
                        continue;
                    }
                }
            }
        }
        filtered.push_back(l);
    }
    std::set<std::wstring> anchors;
    return ParseBlocks(filtered, ctx, refs, anchors, false);
}

// ---------------------------------------------------------------- highlight
namespace {
std::set<std::wstring> KeywordSet(const std::wstring& lang) {
    std::wstring l = ToLower(lang);
    if (l == L"c" || l == L"cpp" || l == L"c++" || l == L"cxx" || l == L"h" || l == L"hpp" || l == L"cc") {
        return { L"auto",L"break",L"case",L"catch",L"char",L"class",L"const",L"constexpr",L"continue",
            L"default",L"delete",L"do",L"double",L"else",L"enum",L"explicit",L"export",L"extern",L"false",
            L"float",L"for",L"friend",L"goto",L"if",L"inline",L"int",L"long",L"mutable",L"namespace",L"new",
            L"noexcept",L"nullptr",L"operator",L"private",L"protected",L"public",L"register",L"return",
            L"short",L"signed",L"sizeof",L"static",L"struct",L"switch",L"template",L"this",L"throw",L"true",
            L"try",L"typedef",L"typename",L"union",L"unsigned",L"using",L"virtual",L"void",L"volatile",
            L"while",L"override",L"final",L"concept",L"requires",L"co_await",L"co_return",L"co_yield" };
    }
    if (l == L"cs" || l == L"csharp" || l == L"c#") {
        return { L"abstract",L"as",L"base",L"bool",L"break",L"byte",L"case",L"catch",L"char",L"checked",
            L"class",L"const",L"continue",L"decimal",L"default",L"delegate",L"do",L"double",L"else",L"enum",
            L"event",L"explicit",L"extern",L"false",L"finally",L"fixed",L"float",L"for",L"foreach",L"goto",
            L"if",L"implicit",L"in",L"int",L"interface",L"internal",L"is",L"lock",L"long",L"namespace",L"new",
            L"null",L"object",L"operator",L"out",L"override",L"params",L"private",L"protected",L"public",
            L"readonly",L"ref",L"return",L"sbyte",L"sealed",L"short",L"sizeof",L"stackalloc",L"static",
            L"string",L"struct",L"switch",L"this",L"throw",L"true",L"try",L"typeof",L"uint",L"ulong",
            L"unchecked",L"unsafe",L"ushort",L"using",L"virtual",L"void",L"volatile",L"while",L"var",
            L"async",L"await",L"record",L"init",L"required" };
    }
    if (l == L"js" || l == L"javascript" || l == L"ts" || l == L"typescript" || l == L"jsx" || l == L"tsx") {
        return { L"await",L"async",L"break",L"case",L"catch",L"class",L"const",L"continue",L"debugger",
            L"default",L"delete",L"do",L"else",L"export",L"extends",L"false",L"finally",L"for",L"function",
            L"if",L"import",L"in",L"instanceof",L"let",L"new",L"null",L"of",L"return",L"static",L"super",
            L"switch",L"this",L"throw",L"true",L"try",L"typeof",L"undefined",L"var",L"void",L"while",L"with",
            L"yield",L"interface",L"type",L"enum",L"implements",L"readonly",L"as",L"namespace",L"declare" };
    }
    if (l == L"py" || l == L"python" || l == L"python3") {
        return { L"and",L"as",L"assert",L"async",L"await",L"break",L"class",L"continue",L"def",L"del",
            L"elif",L"else",L"except",L"False",L"finally",L"for",L"from",L"global",L"if",L"import",L"in",
            L"is",L"lambda",L"None",L"nonlocal",L"not",L"or",L"pass",L"raise",L"return",L"True",L"try",
            L"while",L"with",L"yield",L"match",L"case",L"self" };
    }
    if (l == L"rs" || l == L"rust") {
        return { L"as",L"async",L"await",L"break",L"const",L"continue",L"crate",L"dyn",L"else",L"enum",
            L"extern",L"false",L"fn",L"for",L"if",L"impl",L"in",L"let",L"loop",L"match",L"mod",L"move",
            L"mut",L"pub",L"ref",L"return",L"self",L"Self",L"static",L"struct",L"super",L"trait",L"true",
            L"type",L"unsafe",L"use",L"where",L"while",L"Some",L"None",L"Ok",L"Err" };
    }
    if (l == L"go" || l == L"golang") {
        return { L"break",L"case",L"chan",L"const",L"continue",L"default",L"defer",L"else",L"fallthrough",
            L"for",L"func",L"go",L"goto",L"if",L"import",L"interface",L"map",L"package",L"range",L"return",
            L"select",L"struct",L"switch",L"type",L"var",L"true",L"false",L"nil" };
    }
    if (l == L"java" || l == L"kt" || l == L"kotlin" || l == L"swift") {
        return { L"abstract",L"as",L"break",L"case",L"catch",L"class",L"const",L"continue",L"default",
            L"do",L"else",L"enum",L"extends",L"false",L"final",L"finally",L"for",L"fun",L"if",L"implements",
            L"import",L"in",L"instanceof",L"interface",L"internal",L"is",L"let",L"new",L"null",L"object",
            L"override",L"package",L"private",L"protected",L"public",L"return",L"sealed",L"static",L"super",
            L"switch",L"this",L"throw",L"throws",L"true",L"try",L"var",L"val",L"void",L"when",L"while" };
    }
    if (l == L"sh" || l == L"bash" || l == L"shell" || l == L"zsh" || l == L"ps1" || l == L"powershell") {
        return { L"if",L"then",L"else",L"elif",L"fi",L"for",L"while",L"do",L"done",L"case",L"esac",
            L"function",L"in",L"select",L"until",L"time",L"return",L"exit",L"local",L"export",L"readonly",
            L"declare",L"unset",L"shift",L"source",L"echo",L"set",L"param",L"begin",L"process",L"end",
            L"try",L"catch",L"finally",L"throw",L"foreach",L"filter",L"function" };
    }
    if (l == L"sql") {
        return { L"select",L"from",L"where",L"insert",L"into",L"values",L"update",L"set",L"delete",
            L"create",L"table",L"alter",L"drop",L"index",L"view",L"join",L"inner",L"left",L"right",
            L"outer",L"full",L"on",L"group",L"by",L"order",L"having",L"limit",L"offset",L"union",L"all",
            L"distinct",L"as",L"and",L"or",L"not",L"null",L"is",L"in",L"like",L"between",L"exists",
            L"case",L"when",L"then",L"else",L"end",L"primary",L"key",L"foreign",L"references",L"default",
            L"constraint",L"unique",L"check",L"asc",L"desc",L"with",L"returning" };
    }
    if (l == L"json" || l == L"jsonc" || l == L"json5") {
        return { L"true", L"false", L"null" };
    }
    if (l == L"yaml" || l == L"yml") {
        return { L"true", L"false", L"null", L"yes", L"no", L"on", L"off" };
    }
    return {};
}
std::set<std::wstring> TypeSet(const std::wstring& lang) {
    std::wstring l = ToLower(lang);
    if (l == L"c" || l == L"cpp" || l == L"c++" || l == L"cxx" || l == L"h" || l == L"hpp" || l == L"cs" || l == L"csharp" || l == L"java" || l == L"ts" || l == L"typescript") {
        return { L"void",L"bool",L"char",L"short",L"int",L"long",L"float",L"double",L"size_t",L"string",
            L"String",L"wstring",L"vector",L"map",L"set",L"unique_ptr",L"shared_ptr",L"auto",L"byte",
            L"uint8_t",L"uint16_t",L"uint32_t",L"uint64_t",L"int8_t",L"int16_t",L"int32_t",L"int64_t",
            L"HRESULT",L"BOOL",L"DWORD",L"UINT",L"HWND",L"LPCWSTR",L"std",L"Object",L"Boolean",L"Number",
            L"Promise",L"Array",L"Record",L"Partial",L"Readonly" };
    }
    if (l == L"py" || l == L"python" || l == L"python3") {
        return { L"str",L"int",L"float",L"bool",L"list",L"dict",L"set",L"tuple",L"bytes",L"object",
            L"Exception",L"BaseException",L"Optional",L"List",L"Dict",L"Any",L"Union",L"Callable" };
    }
    return {};
}
bool IsMarkupLang(const std::wstring& lang) {
    std::wstring l = ToLower(lang);
    return l == L"html" || l == L"xml" || l == L"xaml" || l == L"svg" || l == L"xhtml";
}
} // namespace

std::vector<Inline> HighlightCode(const std::wstring& code, const std::wstring& lang) {
    std::vector<Inline> out;
    std::wstring l = ToLower(lang);
    auto keywords = KeywordSet(l);
    auto types = TypeSet(l);
    bool markup = IsMarkupLang(l);
    bool pythonLike = (l == L"py" || l == L"python" || l == L"python3" || l == L"sh" ||
                       l == L"bash" || l == L"shell" || l == L"zsh" || l == L"yaml" || l == L"yml" ||
                       l == L"toml" || l == L"ini" || l == L"ps1" || l == L"powershell");
    bool sqlLike = (l == L"sql");

    auto add = [&](const std::wstring& text, InlineRole role) {
        if (text.empty()) return;
        if (!out.empty() && out.back().role == role) { out.back().text += text; return; }
        Inline r;
        r.text = text;
        r.role = role;
        out.push_back(std::move(r));
    };
    auto isIdentStart = [](wchar_t c) { return iswalpha(c) || c == L'_' || c == L'$'; };
    auto isIdent = [](wchar_t c) { return iswalnum(c) || c == L'_' || c == L'$'; };

    int i = 0;
    int n = (int)code.size();
    while (i < n) {
        wchar_t c = code[(size_t)i];
        if (markup) {
            if (c == L'<') {
                size_t close = code.find(L'>', (size_t)i);
                if (close != std::wstring::npos) {
                    std::wstring tag = code.substr((size_t)i, close - (size_t)i + 1);
                    size_t p = 0;
                    while (p < tag.size()) {
                        if (tag[p] == L'"' || tag[p] == L'\'') {
                            wchar_t q = tag[p];
                            size_t e = tag.find(q, p + 1);
                            if (e == std::wstring::npos) e = tag.size() - 1;
                            add(tag.substr(p, e - p + 1), InlineRole::CodeString);
                            p = e + 1;
                        } else if (iswalpha(tag[p]) || tag[p] == L'/' || tag[p] == L'!' || tag[p] == L'?') {
                            size_t e = p;
                            while (e < tag.size() && (iswalpha(tag[e]) || iswdigit(tag[e]) || tag[e] == L'-' || tag[e] == L':' || tag[e] == L'_')) e++;
                            add(tag.substr(p, e - p), InlineRole::CodeKeyword);
                            p = e;
                        } else {
                            add(std::wstring(1, tag[p]), InlineRole::CodePunct);
                            p++;
                        }
                    }
                    i = (int)close + 1;
                    continue;
                }
            }
            add(std::wstring(1, c), InlineRole::Text);
            i++;
            continue;
        }

        if (c == L'/' && i + 1 < n && code[(size_t)i + 1] == L'/') {
            size_t e = code.find(L'\n', (size_t)i);
            if (e == std::wstring::npos) e = n;
            add(code.substr((size_t)i, e - (size_t)i), InlineRole::CodeComment);
            i = (int)e;
            continue;
        }
        if (c == L'/' && i + 1 < n && code[(size_t)i + 1] == L'*') {
            size_t e = code.find(L"*/", (size_t)i + 2);
            e = (e == std::wstring::npos) ? n : e + 2;
            add(code.substr((size_t)i, e - (size_t)i), InlineRole::CodeComment);
            i = (int)e;
            continue;
        }
        if (c == L'#' && (pythonLike || i == 0 || code[(size_t)i - 1] == L'\n' || iswspace(code[(size_t)i - 1])) && !markup) {
            size_t e = code.find(L'\n', (size_t)i);
            if (e == std::wstring::npos) e = n;
            add(code.substr((size_t)i, e - (size_t)i), InlineRole::CodeComment);
            i = (int)e;
            continue;
        }
        if (sqlLike && c == L'-' && i + 1 < n && code[(size_t)i + 1] == L'-') {
            size_t e = code.find(L'\n', (size_t)i);
            if (e == std::wstring::npos) e = n;
            add(code.substr((size_t)i, e - (size_t)i), InlineRole::CodeComment);
            i = (int)e;
            continue;
        }
        if (c == L'"' || c == L'\'' || c == L'`') {
            wchar_t q = c;
            size_t e = (size_t)i + 1;
            bool raw = (q == L'`');
            while (e < (size_t)n) {
                if (!raw && code[e] == L'\\' && e + 1 < (size_t)n) { e += 2; continue; }
                if (code[e] == q) {
                    size_t qn = 0;
                    while (e + qn < (size_t)n && code[e + qn] == q) qn++;
                    if (qn >= (q == L'"' && i + 2 < n && code[(size_t)i + 1] == L'"') ? 3 : 1) { e += qn; break; }
                }
                if (q != L'`' && code[e] == L'\n') break;
                e++;
            }
            add(code.substr((size_t)i, e - (size_t)i), InlineRole::CodeString);
            i = (int)e;
            continue;
        }
        if (iswdigit(c)) {
            size_t e = (size_t)i;
            while (e < (size_t)n && (iswalnum(code[e]) || code[e] == L'.' || code[e] == L'_' || code[e] == L'x' || code[e] == L'X')) e++;
            add(code.substr((size_t)i, e - (size_t)i), InlineRole::CodeNumber);
            i = (int)e;
            continue;
        }
        if (isIdentStart(c)) {
            size_t e = (size_t)i;
            while (e < (size_t)n && isIdent(code[e])) e++;
            std::wstring w = code.substr((size_t)i, e - (size_t)i);
            if (keywords.count(w)) add(w, InlineRole::CodeKeyword);
            else if (types.count(w) || (!w.empty() && iswupper(w[0]) && w.size() > 1 && w.find(L'_') == std::wstring::npos))
                add(w, InlineRole::CodeType);
            else add(w, InlineRole::Text);
            i = (int)e;
            continue;
        }
        if (iswspace(c)) {
            size_t e = (size_t)i;
            while (e < (size_t)n && iswspace(code[e])) e++;
            add(code.substr((size_t)i, e - (size_t)i), InlineRole::Text);
            i = (int)e;
            continue;
        }
        if (wcschr(L"{}[]()<>+-*/%=!&|^~?:;,.\\", c)) {
            add(std::wstring(1, c), InlineRole::CodePunct);
            i++;
            continue;
        }
        add(std::wstring(1, c), InlineRole::Text);
        i++;
    }
    if (out.empty()) {
        Inline r;
        r.text = code;
        out.push_back(std::move(r));
    }
    return out;
}
