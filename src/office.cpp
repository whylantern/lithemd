#include "office.h"
#include "zip.h"
#include "xml.h"
#include "markdown.h"
#include <algorithm>
#include <map>
#include <set>
#include <functional>

namespace {

std::wstring NormPart(const std::wstring& p) {
    std::wstring s = ReplaceAll(p, L"\\", L"/");
    while (!s.empty() && s[0] == L'/') s.erase(0, 1);
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= s.size()) {
        size_t e = s.find(L'/', start);
        if (e == std::wstring::npos) e = s.size();
        std::wstring part = s.substr(start, e - start);
        if (part == L"..") { if (!parts.empty()) parts.pop_back(); }
        else if (!part.empty() && part != L".") parts.push_back(part);
        if (e == s.size()) break;
        start = e + 1;
    }
    std::wstring out;
    for (auto& p2 : parts) { if (!out.empty()) out += L"/"; out += p2; }
    return out;
}

std::wstring ResolvePart(const std::wstring& base, const std::wstring& target) {
    if (target.empty()) return L"";
    if (target[0] == L'/') return NormPart(target);
    return NormPart(base + L"/" + target);
}

std::wstring MimeForExt(const std::wstring& path) {
    std::wstring e = PathExt(path);
    if (e == L".png") return L"image/png";
    if (e == L".jpg" || e == L".jpeg") return L"image/jpeg";
    if (e == L".gif") return L"image/gif";
    if (e == L".bmp") return L"image/bmp";
    if (e == L".webp") return L"image/webp";
    if (e == L".svg") return L"image/svg+xml";
    if (e == L".tif" || e == L".tiff") return L"image/tiff";
    return L"application/octet-stream";
}

void AppendInline(std::vector<Inline>& out, const std::wstring& text, bool bold, bool italic,
                  bool underline, bool strike, bool code, const std::wstring& url = L"") {
    if (text.empty()) return;
    if (!out.empty()) {
        Inline& p = out.back();
        if (p.bold == bold && p.italic == italic && p.underline == underline &&
            p.strike == strike && p.code == code && p.url == url && p.link == !url.empty() && !p.math) {
            p.text += text;
            return;
        }
    }
    Inline r;
    r.text = text;
    r.bold = bold;
    r.italic = italic;
    r.underline = underline;
    r.strike = strike;
    r.code = code;
    if (!url.empty()) {
        r.link = true;
        r.url = url;
        r.underline = true;
        r.role = InlineRole::Accent;
    }
    out.push_back(std::move(r));
}

struct RunStyle {
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;
    bool code = false;
};

struct RelInfo {
    std::wstring target;
    std::wstring type;
    bool external = false;
};

std::wstring XmlDocTitle(const XmlNode* n) {
    if (!n) return L"";
    if (n->Is(L"t")) return n->text;
    std::wstring out;
    for (auto& c : n->children) out += XmlDocTitle(c.get());
    return out;
}

} // namespace

// ================================================================ DOCX
namespace {

class DocxParser {
public:
    struct StyleInfo {
        std::wstring name;
        int level = 0;
        bool quote = false;
        bool code = false;
    };
    struct NumLevel {
        std::wstring fmt = L"decimal";
        int start = 1;
    };

    Document& doc;
    ZipReader zip;
    std::map<std::wstring, RelInfo> relsById;
    std::map<std::wstring, StyleInfo> styles;
    std::map<int, std::map<int, NumLevel>> numbering;
    std::map<int, int> numToAbstract;
    std::map<uint64_t, int> counters;
    std::map<std::wstring, std::shared_ptr<ImageRef>> imageCache;
    std::set<std::wstring> anchors;
    std::wstring error;

    explicit DocxParser(Document& d) : doc(d) {}

    bool Load(const std::vector<uint8_t>& bytes) {
        if (!zip.Open(bytes, &error)) return false;
        ParseStyles();
        ParseNumbering();
        ParseRels();
        std::wstring documentXml;
        if (!zip.ReadText(L"word/document.xml", documentXml, &error)) return false;
        XmlNode root;
        if (!ParseXml(documentXml, root, &error)) return false;
        const XmlNode* body = nullptr;
        if (root.Is(L"document")) body = root.Child(L"body");
        if (!body) {
            for (auto& c : root.children) {
                if (c->Child(L"body")) { body = c->Child(L"body"); break; }
            }
        }
        if (!body) { error = L"document.xml 中缺少 body"; return false; }
        ParseBody(*body);
        // core properties title
        std::wstring core;
        if (zip.ReadText(L"docProps/core.xml", core)) {
            XmlNode croot;
            if (ParseXml(core, croot)) {
                for (auto& c : croot.children) {
                    if (c->Is(L"title") && !Trim(c->text).empty()) { doc.title = Trim(c->text); break; }
                }
            }
        }
        return true;
    }

private:
    void ParseStyles() {
        std::wstring text;
        if (!zip.ReadText(L"word/styles.xml", text)) return;
        XmlNode root;
        if (!ParseXml(text, root)) return;
        for (auto& st : root.children) {
            if (!st->Is(L"style")) continue;
            std::wstring id = st->Attr(L"w:styleId");
            if (id.empty()) id = st->AttrLocal(L"styleId");
            if (id.empty()) continue;
            StyleInfo info;
            if (const XmlNode* n = st->Child(L"name")) {
                info.name = n->Attr(L"w:val");
                if (info.name.empty()) info.name = n->AttrLocal(L"val");
            }
            std::wstring lowId = ToLower(id), lowName = ToLower(info.name);
            auto digits = [](const std::wstring& s) {
                std::wstring d;
                for (wchar_t c : s) if (iswdigit(c)) d += c;
                return d;
            };
            if (lowName.rfind(L"heading", 0) == 0 || lowId.rfind(L"heading", 0) == 0) {
                info.level = _wtoi(digits(lowName).empty() ? digits(lowId).c_str() : digits(lowName).c_str());
                if (info.level < 1 || info.level > 6) info.level = 1;
            } else if (lowName == L"title" || lowId == L"title") info.level = 1;
            else if (lowName == L"subtitle" || lowId == L"subtitle") info.level = 2;
            else if (lowName == L"quote" || lowName == L"intense quote" || lowId == L"quote") info.quote = true;
            else if (lowName == L"code" || lowName.find(L"code") != std::wstring::npos || lowId.find(L"code") != std::wstring::npos) info.code = true;
            if (const XmlNode* ppr = st->Child(L"pPr")) {
                if (const XmlNode* ol = ppr->Child(L"outlineLvl")) {
                    std::wstring v = ol->Attr(L"w:val");
                    if (v.empty()) v = ol->AttrLocal(L"val");
                    int lv = _wtoi(v.c_str());
                    if (lv >= 0 && lv < 6) info.level = lv + 1;
                }
                if (ppr->Child(L"numPr")) {
                    // numbering handled per paragraph
                }
            }
            if (const XmlNode* rpr = st->Child(L"rPr")) {
                if (const XmlNode* rf = rpr->Child(L"rFonts")) {
                    std::wstring font = rf->Attr(L"w:ascii");
                    if (font.empty()) font = rf->Attr(L"w:hAnsi");
                    std::wstring lf = ToLower(font);
                    if (lf.find(L"consolas") != std::wstring::npos || lf.find(L"courier") != std::wstring::npos ||
                        lf.find(L"mono") != std::wstring::npos) info.code = true;
                }
            }
            styles[id] = info;
        }
    }

    void ParseNumbering() {
        std::wstring text;
        if (!zip.ReadText(L"word/numbering.xml", text)) return;
        XmlNode root;
        if (!ParseXml(text, root)) return;
        std::map<int, std::map<int, NumLevel>> abstracts;
        for (auto& n : root.children) {
            if (n->Is(L"abstractNum")) {
                int id = _wtoi(n->Attr(L"w:abstractNumId").c_str());
                for (auto& lvl : n->children) {
                    if (!lvl->Is(L"lvl")) continue;
                    int il = _wtoi(lvl->Attr(L"w:ilvl").c_str());
                    NumLevel nl;
                    if (const XmlNode* f = lvl->Child(L"numFmt")) {
                        nl.fmt = f->Attr(L"w:val");
                        if (nl.fmt.empty()) nl.fmt = f->AttrLocal(L"val");
                    }
                    if (const XmlNode* s = lvl->Child(L"start")) {
                        std::wstring v = s->Attr(L"w:val");
                        if (!v.empty()) nl.start = _wtoi(v.c_str());
                    }
                    abstracts[id][il] = nl;
                }
            }
        }
        for (auto& n : root.children) {
            if (n->Is(L"num")) {
                int numId = _wtoi(n->Attr(L"w:numId").c_str());
                int absId = 0;
                if (const XmlNode* a = n->Child(L"abstractNumId")) {
                    std::wstring v = a->Attr(L"w:val");
                    if (v.empty()) v = a->AttrLocal(L"val");
                    absId = _wtoi(v.c_str());
                }
                numToAbstract[numId] = absId;
            }
        }
        numbering = std::move(abstracts);
    }

    void ParseRels() {
        std::wstring text;
        if (!zip.ReadText(L"word/_rels/document.xml.rels", text)) return;
        XmlNode root;
        if (!ParseXml(text, root)) return;
        for (auto& r : root.children) {
            if (!r->Is(L"Relationship")) continue;
            std::wstring id = r->Attr(L"Id");
            RelInfo info;
            info.target = r->Attr(L"Target");
            info.type = r->Attr(L"Type");
            std::wstring mode = r->Attr(L"TargetMode");
            info.external = (ToLower(mode) == L"external");
            relsById[id] = info;
        }
    }

    const RelInfo* Rel(const std::wstring& id) const {
        auto it = relsById.find(id);
        if (it == relsById.end()) return nullptr;
        return &it->second;
    }

    int HeadingLevelFor(const std::wstring& styleId, int outlineLevel) const {
        if (outlineLevel >= 0 && outlineLevel < 6) return outlineLevel + 1;
        auto it = styles.find(styleId);
        if (it != styles.end() && it->second.level) return it->second.level;
        std::wstring low = ToLower(styleId);
        if (low.rfind(L"heading", 0) == 0) {
            int n = _wtoi(low.c_str() + 7);
            if (n >= 1 && n <= 6) return n;
            return 1;
        }
        return 0;
    }

    std::shared_ptr<ImageRef> LoadImage(const std::wstring& relId) {
        const RelInfo* rel = Rel(relId);
        if (!rel || rel->external) return nullptr;
        std::wstring part = ResolvePart(L"word", rel->target);
        auto it = imageCache.find(part);
        if (it != imageCache.end()) return it->second;
        std::vector<uint8_t> bytes;
        if (!zip.Read(part, bytes) || bytes.empty()) return nullptr;
        auto img = std::make_shared<ImageRef>();
        img->bytes = std::move(bytes);
        img->mime = MimeForExt(part);
        img->cacheKey = part + L":" + std::to_wstring(img->bytes.size());
        imageCache[part] = img;
        return img;
    }

    void ApplyRunStyle(const XmlNode& r, RunStyle& st) {
        const XmlNode* rpr = r.Child(L"rPr");
        if (!rpr) return;
        auto flag = [&](const wchar_t* name) -> bool {
            const XmlNode* n = rpr->Child(name);
            if (!n) return false;
            std::wstring v = ToLower(n->Attr(L"w:val"));
            return !(v == L"0" || v == L"false" || v == L"none");
        };
        if (flag(L"b")) st.bold = true;
        if (flag(L"i")) st.italic = true;
        if (flag(L"u")) st.underline = true;
        if (flag(L"strike") || flag(L"dstrike")) st.strike = true;
        if (const XmlNode* rs = rpr->Child(L"rStyle")) {
            std::wstring id = rs->Attr(L"w:val");
            std::wstring low = ToLower(id);
            if (low.find(L"strong") != std::wstring::npos) st.bold = true;
            if (low.find(L"emphasis") != std::wstring::npos) st.italic = true;
            if (low.find(L"code") != std::wstring::npos) st.code = true;
        }
        if (const XmlNode* rf = rpr->Child(L"rFonts")) {
            std::wstring font = ToLower(rf->Attr(L"w:ascii"));
            if (font.empty()) font = ToLower(rf->Attr(L"w:hAnsi"));
            if (font.find(L"consolas") != std::wstring::npos || font.find(L"courier") != std::wstring::npos ||
                font.find(L"mono") != std::wstring::npos) st.code = true;
        }
    }

    std::wstring LinkTarget(const XmlNode& h) const {
        std::wstring rid = h.Attr(L"r:id");
        if (rid.empty()) rid = h.AttrLocal(L"id");
        const RelInfo* rel = Rel(rid);
        if (rel) return rel->target;
        std::wstring anchor = h.Attr(L"w:anchor");
        if (anchor.empty()) anchor = h.AttrLocal(L"anchor");
        if (!anchor.empty()) return L"#" + anchor;
        return L"";
    }

    void WalkInline(const XmlNode& n, RunStyle st, const std::wstring& link,
                    std::vector<Inline>& out, std::vector<std::shared_ptr<ImageRef>>& images) {
        std::wstring name = n.LocalName();
        if (name == L"del" || name == L"moveFrom" || name == L"instrText" || name == L"rPr" ||
            name == L"pPr" || name == L"commentReference" || name == L"footnoteReference") return;
        if (name == L"r") {
            RunStyle rs = st;
            ApplyRunStyle(n, rs);
            for (auto& c : n.children) WalkInline(*c, rs, link, out, images);
            return;
        }
        if (name == L"t") {
            AppendInline(out, n.text, st.bold, st.italic, st.underline, st.strike, st.code, link);
            return;
        }
        if (name == L"tab") {
            AppendInline(out, L"    ", st.bold, st.italic, st.underline, st.strike, st.code, link);
            return;
        }
        if (name == L"br" || name == L"cr") {
            AppendInline(out, L"\n", st.bold, st.italic, st.underline, st.strike, st.code, link);
            return;
        }
        if (name == L"noBreakHyphen") {
            AppendInline(out, L"-", st.bold, st.italic, st.underline, st.strike, st.code, link);
            return;
        }
        if (name == L"sym") {
            std::wstring ch = n.Attr(L"w:char");
            if (!ch.empty()) {
                long v = wcstol(ch.c_str(), nullptr, 16);
                if (v > 0 && v < 0x10000) AppendInline(out, std::wstring(1, (wchar_t)v), st.bold, st.italic, st.underline, st.strike, st.code, link);
            }
            return;
        }
        if (name == L"hyperlink") {
            std::wstring target = LinkTarget(n);
            for (auto& c : n.children) WalkInline(*c, st, target, out, images);
            return;
        }
        if (name == L"drawing" || name == L"pict" || name == L"object") {
            // find blip / imagedata relationship ids
            std::function<void(const XmlNode&)> find = [&](const XmlNode& x) {
                std::wstring en = x.LocalName();
                if (en == L"blip" || en == L"imagedata") {
                    std::wstring rid = x.Attr(L"r:embed");
                    if (rid.empty()) rid = x.Attr(L"r:id");
                    if (rid.empty()) rid = x.AttrLocal(L"embed");
                    if (rid.empty()) rid = x.AttrLocal(L"id");
                    if (!rid.empty()) {
                        auto img = LoadImage(rid);
                        if (img && images.end() == std::find(images.begin(), images.end(), img))
                            images.push_back(img);
                    }
                } else if (en == L"docPr" || en == L"cNvPr") {
                    std::wstring alt = x.Attr(L"descr");
                    if (alt.empty()) alt = x.Attr(L"title");
                    if (!alt.empty() && !images.empty()) images.back()->alt = alt;
                }
                for (auto& c : x.children) find(*c);
            };
            find(n);
            return;
        }
        if (name == L"AlternateContent") {
            for (auto& c : n.children) {
                if (c->Is(L"Choice")) { for (auto& cc : c->children) WalkInline(*cc, st, link, out, images); return; }
            }
            for (auto& c : n.children) {
                if (c->Is(L"Fallback")) { for (auto& cc : c->children) WalkInline(*cc, st, link, out, images); return; }
            }
            return;
        }
        // containers
        for (auto& c : n.children) WalkInline(*c, st, link, out, images);
    }

    int AlignOf(const XmlNode* pPr) const {
        if (!pPr) return 0;
        if (const XmlNode* jc = pPr->Child(L"jc")) {
            std::wstring v = ToLower(jc->Attr(L"w:val"));
            if (v == L"center") return 1;
            if (v == L"right" || v == L"end") return 2;
        }
        return 0;
    }

    std::wstring ListMarker(int numId, int ilvl, bool& ordered) {
        ordered = false;
        int absId = 0;
        auto nit = numToAbstract.find(numId);
        if (nit != numToAbstract.end()) absId = nit->second;
        NumLevel lvl;
        auto ait = numbering.find(absId);
        if (ait != numbering.end()) {
            auto lit = ait->second.find(ilvl);
            if (lit != ait->second.end()) lvl = lit->second;
        }
        std::wstring fmt = ToLower(lvl.fmt);
        if (fmt == L"bullet" || fmt == L"none") { ordered = false; return L"•"; }
        ordered = true;
        uint64_t key = ((uint64_t)numId << 8) | (uint64_t)ilvl;
        int v;
        auto cit = counters.find(key);
        if (cit == counters.end()) v = lvl.start;
        else v = cit->second + 1;
        counters[key] = v;
        // reset deeper levels
        for (auto it = counters.begin(); it != counters.end();) {
            if ((int)(it->first >> 8) == numId && (int)(it->first & 0xFF) > ilvl) it = counters.erase(it);
            else ++it;
        }
        switch (0) { default: break; }
        if (fmt == L"lowerletter" || fmt == L"upperletter") {
            wchar_t c = (wchar_t)(L'a' + (v - 1) % 26);
            if (fmt == L"upperletter") c = (wchar_t)towupper(c);
            return std::wstring(1, c) + L".";
        }
        if (fmt == L"lowerroman" || fmt == L"upperroman") {
            auto roman = [](int n) {
                const std::pair<int, const wchar_t*> tab[] = {{1000,L"M"},{900,L"CM"},{500,L"D"},{400,L"CD"},
                    {100,L"C"},{90,L"XC"},{50,L"L"},{40,L"XL"},{10,L"X"},{9,L"IX"},{5,L"V"},{4,L"IV"},{1,L"I"}};
                std::wstring r;
                for (auto& p : tab) while (n >= p.first) { r += p.second; n -= p.first; }
                return r;
            };
            std::wstring r = roman(v);
            if (fmt == L"lowerroman") for (auto& c : r) c = (wchar_t)towlower(c);
            return r + L".";
        }
        return std::to_wstring(v) + L".";
    }

    void AddBlock(Block b) {
        doc.blocks.push_back(std::move(b));
    }

    void ParseParagraph(const XmlNode& p, int inheritedIndent = 0, bool inCell = false) {
        const XmlNode* ppr = p.Child(L"pPr");
        std::wstring styleId;
        if (ppr) {
            if (const XmlNode* ps = ppr->Child(L"pStyle")) {
                styleId = ps->Attr(L"w:val");
                if (styleId.empty()) styleId = ps->AttrLocal(L"val");
            }
        }
        int outline = -1;
        if (ppr) {
            if (const XmlNode* ol = ppr->Child(L"outlineLvl")) {
                std::wstring v = ol->Attr(L"w:val");
                if (v.empty()) v = ol->AttrLocal(L"val");
                outline = _wtoi(v.c_str());
            }
        }
        bool quote = false;
        bool codeStyle = false;
        {
            auto it = styles.find(styleId);
            if (it != styles.end()) {
                quote = it->second.quote;
                codeStyle = it->second.code;
            }
        }
        int numId = 0, ilvl = 0;
        if (ppr) {
            if (const XmlNode* np = ppr->Child(L"numPr")) {
                if (const XmlNode* ni = np->Child(L"numId")) {
                    std::wstring v = ni->Attr(L"w:val");
                    if (v.empty()) v = ni->AttrLocal(L"val");
                    numId = _wtoi(v.c_str());
                }
                if (const XmlNode* il = np->Child(L"ilvl")) {
                    std::wstring v = il->Attr(L"w:val");
                    if (v.empty()) v = il->AttrLocal(L"val");
                    ilvl = _wtoi(v.c_str());
                }
            }
        }
        int leftTwips = 0;
        if (ppr) {
            if (const XmlNode* ind = ppr->Child(L"ind")) {
                std::wstring v = ind->Attr(L"w:left");
                if (v.empty()) v = ind->Attr(L"w:start");
                if (!v.empty()) leftTwips = _wtoi(v.c_str());
            }
        }
        RunStyle st;
        st.code = codeStyle;
        std::vector<Inline> inlines;
        std::vector<std::shared_ptr<ImageRef>> images;
        for (auto& c : p.children) WalkInline(*c, st, L"", inlines, images);
        // trim empty inlines
        for (auto& r : inlines) r.text = ReplaceAll(r.text, L"\t", L"    ");
        while (!inlines.empty() && Trim(inlines.back().text).empty() && !inlines.back().math) inlines.pop_back();
        bool hasText = false;
        for (auto& r : inlines) if (!r.text.empty() || r.math) hasText = true;

        int heading = HeadingLevelFor(styleId, outline);
        if (heading > 0) {
            if (hasText) {
                Block b;
                b.kind = BlockKind::Heading;
                b.level = heading;
                b.inlines = std::move(inlines);
                b.align = AlignOf(ppr);
                b.quote = quote;
                for (auto& r : b.inlines) if (r.text.empty() && !r.math) r.text = L"";
                b.text.clear();
                for (auto& r : b.inlines) b.text += r.text;
                b.anchor = Slugify(b.text);
                if (b.anchor.empty()) b.anchor = L"section";
                while (anchors.count(b.anchor)) b.anchor += L"-1";
                anchors.insert(b.anchor);
                int idx = (int)doc.blocks.size();
                doc.outline.push_back({heading, Trim(ReplaceAll(b.text, L"\n", L" ")), idx});
                AddBlock(std::move(b));
                doc.docParagraphs++;
                for (auto& img : images) { Block ib; ib.kind = BlockKind::Image; ib.image = img; ib.text = img->alt; AddBlock(std::move(ib)); }
            }
            return;
        }
        if (numId > 0 && hasText) {
            Block b;
            b.kind = BlockKind::ListItem;
            bool ordered = false;
            std::wstring marker = ListMarker(numId, ilvl, ordered);
            b.ordered = ordered;
            b.number = ordered ? _wtoi(marker.c_str()) : 0;
            b.indent = std::max(inheritedIndent + ilvl, leftTwips / 360);
            b.inlines = std::move(inlines);
            b.align = AlignOf(ppr);
            b.quote = quote;
            b.text = marker;
            int idx = (int)doc.blocks.size();
            AddBlock(std::move(b));
            doc.docParagraphs++;
            for (auto& img : images) { Block ib; ib.kind = BlockKind::Image; ib.image = img; ib.text = img->alt; AddBlock(std::move(ib)); }
            (void)idx;
            return;
        }
        if (hasText) {
            Block b;
            b.kind = BlockKind::Paragraph;
            b.align = AlignOf(ppr);
            b.quote = quote;
            b.inlines = std::move(inlines);
            AddBlock(std::move(b));
            doc.docParagraphs++;
        }
        for (auto& img : images) {
            Block ib;
            ib.kind = BlockKind::Image;
            ib.image = img;
            ib.text = img->alt;
            AddBlock(std::move(ib));
        }
    }

    void CellParagraphs(const XmlNode& tc, std::vector<Inline>& out, int& align) {
        align = 0;
        bool first = true;
        std::function<void(const XmlNode&)> visit = [&](const XmlNode& n) {
            if (n.Is(L"p")) {
                const XmlNode* ppr = n.Child(L"pPr");
                if (first) { align = AlignOf(ppr); first = false; }
                RunStyle st;
                std::vector<Inline> tmp;
                std::vector<std::shared_ptr<ImageRef>> imgs;
                for (auto& c : n.children) WalkInline(*c, st, L"", tmp, imgs);
                if (!out.empty()) {
                    bool onlyEmpty = true;
                    for (auto& r : tmp) if (!Trim(r.text).empty()) onlyEmpty = false;
                    if (!onlyEmpty) AppendInline(out, L"\n", false, false, false, false, false);
                }
                for (auto& r : tmp) {
                    AppendInline(out, r.text, r.bold, r.italic, r.underline, r.strike, r.code, r.url);
                }
                return;
            }
            for (auto& c : n.children) visit(*c);
        };
        visit(tc);
    }

    void ParseTable(const XmlNode& tbl) {
        Block b;
        b.kind = BlockKind::Table;
        int rowIndex = 0;
        for (auto& tr : tbl.children) {
            if (!tr->Is(L"tr")) continue;
            bool header = false;
            if (const XmlNode* trPr = tr->Child(L"trPr")) {
                if (const XmlNode* th = trPr->Child(L"tblHeader")) {
                    std::wstring v = ToLower(th->Attr(L"w:val"));
                    header = !(v == L"0" || v == L"false");
                    if (v.empty()) header = true;
                }
            }
            if (rowIndex == 0) header = true; // most Word tables read better with a header row
            std::vector<TableCell> row;
            for (auto& tc : tr->children) {
                if (!tc->Is(L"tc")) continue;
                TableCell cell;
                cell.header = header;
                int align = 0;
                CellParagraphs(*tc, cell.inlines, align);
                cell.align = align;
                row.push_back(std::move(cell));
            }
            if (!row.empty()) b.table.rows.push_back(std::move(row));
            rowIndex++;
        }
        if (!b.table.rows.empty()) {
            size_t cols = 0;
            for (auto& r : b.table.rows) cols = std::max(cols, r.size());
            for (auto& r : b.table.rows) while (r.size() < cols) r.push_back(TableCell{});
            b.table.aligns.assign(cols, 0);
            AddBlock(std::move(b));
        }
    }

    void ParseBody(const XmlNode& body) {
        for (auto& c : body.children) {
            if (c->Is(L"p")) ParseParagraph(*c);
            else if (c->Is(L"tbl")) ParseTable(*c);
            else if (c->Is(L"sdt")) {
                if (const XmlNode* content = c->Child(L"sdtContent")) ParseBody(*content);
            }
        }
    }
};

} // namespace

bool BuildDocx(Document& doc, const std::vector<uint8_t>& bytes, std::wstring* error) {
    DocxParser p(doc);
    if (!p.Load(bytes)) {
        if (error) *error = p.error.empty() ? L"无法读取 Word 文档" : p.error;
        return false;
    }
    if (doc.blocks.empty()) {
        if (error) *error = L"Word 文档中没有可显示的正文";
        return false;
    }
    doc.kind = DocKind::Docx;
    doc.sourceAllowed = false;
    doc.subtitle = L"Word 文档";
    if (doc.outline.empty()) doc.outline.clear();
    doc.meta = L"段落 " + std::to_wstring(doc.docParagraphs);
    return true;
}

// ================================================================ XLSX
namespace {

class XlsxParser {
public:
    Document& doc;
    ZipReader zip;
    std::wstring error;
    std::map<std::wstring, RelInfo> rels;
    std::vector<std::wstring> shared;
    struct SheetRef { std::wstring name; std::wstring rid; int index; };
    std::vector<SheetRef> sheets;
    int activeTab = 0;
    static constexpr int MaxRows = 3000;
    static constexpr int MaxCols = 120;

    explicit XlsxParser(Document& d) : doc(d) {}

    bool Load(const std::vector<uint8_t>& bytes) {
        if (!zip.Open(bytes, &error)) return false;
        ParseWorkbook();
        ParseShared();
        for (auto& s : sheets) {
            ParseSheet(s);
        }
        if (doc.grid.sheets.empty()) {
            error = L"工作簿中没有工作表";
            return false;
        }
        doc.grid.active = std::min(activeTab, (int)doc.grid.sheets.size() - 1);
        if (doc.grid.active < 0) doc.grid.active = 0;
        return true;
    }

private:
    void ParseWorkbook() {
        std::wstring wb;
        if (!zip.ReadText(L"xl/workbook.xml", wb, &error)) return;
        XmlNode root;
        if (!ParseXml(wb, root, &error)) return;
        if (const XmlNode* views = root.Child(L"bookViews")) {
            if (const XmlNode* v = views->Child(L"workbookView")) {
                std::wstring a = v->Attr(L"activeTab");
                if (!a.empty()) activeTab = _wtoi(a.c_str());
            }
        }
        std::wstring relText;
        if (zip.ReadText(L"xl/_rels/workbook.xml.rels", relText)) {
            XmlNode rroot;
            if (ParseXml(relText, rroot)) {
                for (auto& r : rroot.children) {
                    if (!r->Is(L"Relationship")) continue;
                    RelInfo info;
                    info.target = r->Attr(L"Target");
                    info.type = r->Attr(L"Type");
                    rels[r->Attr(L"Id")] = info;
                }
            }
        }
        const XmlNode* sheetsNode = root.Child(L"sheets");
        if (!sheetsNode) return;
        int idx = 1;
        for (auto& s : sheetsNode->children) {
            if (!s->Is(L"sheet")) continue;
            SheetRef ref;
            ref.name = s->Attr(L"name");
            ref.rid = s->Attr(L"r:id");
            if (ref.rid.empty()) ref.rid = s->AttrLocal(L"id");
            ref.index = idx++;
            if (!ref.name.empty()) sheets.push_back(ref);
        }
    }

    void ParseShared() {
        std::wstring text;
        if (!zip.ReadText(L"xl/sharedStrings.xml", text)) return;
        XmlNode root;
        if (!ParseXml(text, root)) return;
        for (auto& si : root.children) {
            if (!si->Is(L"si")) continue;
            shared.push_back(XmlDocTitle(si.get()));
        }
    }

    std::wstring SheetPath(const SheetRef& s) const {
        auto it = rels.find(s.rid);
        if (it != rels.end()) {
            std::wstring p = ResolvePart(L"xl", it->second.target);
            if (zip.Has(p)) return p;
        }
        std::wstring guess = L"xl/worksheets/sheet" + std::to_wstring(s.index) + L".xml";
        return guess;
    }

    static int ColumnIndex(const std::wstring& ref) {
        int col = 0;
        for (wchar_t c : ref) {
            if (c >= L'A' && c <= L'Z') col = col * 26 + (c - L'A' + 1);
            else if (c >= L'a' && c <= L'z') col = col * 26 + (c - L'a' + 1);
            else break;
        }
        return col > 0 ? col - 1 : -1;
    }
    static int RowIndex(const std::wstring& ref) {
        int row = 0;
        bool digits = false;
        for (wchar_t c : ref) {
            if (iswdigit(c)) { digits = true; row = row * 10 + (c - L'0'); }
            else if (digits) break;
        }
        return digits ? row - 1 : -1;
    }

    std::wstring CellValue(const XmlNode& c) {
        std::wstring t = c.Attr(L"t");
        if (t == L"s") {
            if (const XmlNode* v = c.Child(L"v")) {
                int idx = _wtoi(v->text.c_str());
                if (idx >= 0 && idx < (int)shared.size()) return shared[(size_t)idx];
            }
            return L"";
        }
        if (t == L"inlineStr") {
            if (const XmlNode* is = c.Child(L"is")) return XmlDocTitle(is);
            return L"";
        }
        if (const XmlNode* v = c.Child(L"v")) {
            std::wstring val = v->text;
            if (t == L"b") return val == L"1" ? L"TRUE" : L"FALSE";
            return val;
        }
        if (const XmlNode* f = c.Child(L"f")) {
            std::wstring formula = f->text;
            if (!formula.empty()) return L"=" + formula;
        }
        return L"";
    }

    void ParseSheet(const SheetRef& s) {
        std::wstring path = SheetPath(s);
        std::wstring text;
        if (!zip.ReadText(path, text)) return;
        XmlNode root;
        if (!ParseXml(text, root)) return;
        const XmlNode* data = nullptr;
        if (root.Is(L"worksheet")) data = root.Child(L"sheetData");
        if (!data) {
            for (auto& c : root.children) if (c->Child(L"sheetData")) { data = c->Child(L"sheetData"); break; }
        }
        if (!data) return;
        GridSheet sheet;
        sheet.name = s.name;
        int lastRow = -1;
        int maxCols = 0;
        std::map<int, std::vector<std::wstring>> rowMap;
        for (auto& rowNode : data->children) {
            if (!rowNode->Is(L"row")) continue;
            int r = _wtoi(rowNode->Attr(L"r").c_str()) - 1;
            if (r < 0) r = lastRow + 1;
            lastRow = r;
            if (r >= MaxRows) { doc.grid.truncated = true; break; }
            std::vector<std::wstring>& row = rowMap[r];
            int lastCol = -1;
            for (auto& c : rowNode->children) {
                if (!c->Is(L"c")) continue;
                std::wstring ref = c->Attr(L"r");
                int col = ColumnIndex(ref);
                if (col < 0) col = lastCol + 1;
                lastCol = col;
                if (col >= MaxCols) { doc.grid.truncated = true; continue; }
                std::wstring v = CellValue(*c);
                if ((int)row.size() <= col) row.resize((size_t)col + 1);
                row[(size_t)col] = v;
                maxCols = std::max(maxCols, col + 1);
            }
        }
        if (rowMap.empty()) return;
        int last = rowMap.rbegin()->first;
        sheet.rows.resize((size_t)last + 1);
        for (auto& kv : rowMap) sheet.rows[(size_t)kv.first] = std::move(kv.second);
        sheet.maxCols = maxCols;
        if ((int)sheet.rows.size() > MaxRows) sheet.rows.resize(MaxRows);
        doc.csvRows += (int)sheet.rows.size();
        doc.grid.sheets.push_back(std::move(sheet));
    }
};

} // namespace

bool BuildXlsx(Document& doc, const std::vector<uint8_t>& bytes, std::wstring* error) {
    XlsxParser p(doc);
    if (!p.Load(bytes)) {
        if (error) *error = p.error.empty() ? L"无法读取 Excel 工作簿" : p.error;
        return false;
    }
    doc.kind = DocKind::Xlsx;
    doc.grid.fromExcel = true;
    doc.sourceAllowed = false;
    doc.subtitle = L"Excel 工作簿 · " + std::to_wstring(doc.grid.sheets.size()) + L" 个工作表";
    if (doc.grid.truncated) doc.grid.note = L"数据较大，已截断为最多 3000 行 × 120 列";
    return true;
}

// ================================================================ CSV
bool BuildCsv(Document& doc, const std::wstring& text, std::wstring* error) {
    std::wstring src = NormalizeNewlines(text);
    if (src.empty()) { if (error) *error = L"CSV 内容为空"; return false; }
    // pick delimiter
    auto countSep = [&](wchar_t sep) {
        int count = 0;
        bool quoted = false;
        int lines = 0;
        for (size_t i = 0; i < src.size() && lines < 20; i++) {
            wchar_t c = src[i];
            if (c == L'"') quoted = !quoted;
            else if (c == L'\n' && !quoted) lines++;
            else if (c == sep && !quoted) count++;
        }
        return count;
    };
    wchar_t sep = L',';
    int best = countSep(L',');
    int tabs = countSep(L'\t');
    int semis = countSep(L';');
    if (tabs > best) { sep = L'\t'; best = tabs; }
    if (semis > best) { sep = L';'; }
    std::vector<std::vector<std::wstring>> rows;
    std::vector<std::wstring> row;
    std::wstring cell;
    bool quoted = false;
    bool truncated = false;
    const int MaxRowsCsv = 3000, MaxColsCsv = 120;
    for (size_t i = 0; i < src.size(); i++) {
        wchar_t c = src[i];
        if (quoted) {
            if (c == L'"') {
                if (i + 1 < src.size() && src[i + 1] == L'"') { cell += L'"'; i++; }
                else quoted = false;
            } else cell += c;
        } else {
            if (c == L'"' && cell.empty()) quoted = true;
            else if (c == sep) {
                row.push_back(cell); cell.clear();
                if ((int)row.size() >= MaxColsCsv) {
                    truncated = true;
                    // consume until newline
                    while (i < src.size() && src[i] != L'\n') i++;
                }
            } else if (c == L'\n') {
                row.push_back(cell); cell.clear();
                rows.push_back(std::move(row)); row.clear();
                if ((int)rows.size() >= MaxRowsCsv) { truncated = true; break; }
            } else cell += c;
        }
    }
    if (!cell.empty() || !row.empty()) {
        row.push_back(cell);
        rows.push_back(std::move(row));
    }
    if (rows.empty()) { if (error) *error = L"CSV 中没有数据"; return false; }
    GridSheet sheet;
    sheet.name = L"CSV";
    sheet.rows = std::move(rows);
    size_t cols = 0;
    for (auto& r : sheet.rows) cols = std::max(cols, r.size());
    sheet.maxCols = (int)cols;
    doc.grid.sheets.push_back(std::move(sheet));
    doc.grid.active = 0;
    doc.grid.truncated = truncated || (int)doc.grid.sheets[0].rows.size() >= MaxRowsCsv || cols >= MaxColsCsv;
    if (doc.grid.truncated) doc.grid.note = L"数据较大，已截断为最多 3000 行 × 120 列";
    doc.kind = DocKind::Csv;
    doc.csvRows = (int)doc.grid.sheets[0].rows.size();
    doc.subtitle = L"CSV 表格 · " + std::to_wstring(doc.csvRows) + L" 行";
    return true;
}
