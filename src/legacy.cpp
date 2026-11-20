// Readers for legacy Word (.doc) and Excel (.xls) binary formats.
#include "office.h"
#include "ole.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <map>

namespace {

uint16_t U16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
double F64(const uint8_t* p) {
    double d = 0;
    memcpy(&d, p, 8);
    return d;
}
wchar_t AnsiByte(uint8_t b) {
    static const wchar_t map[32] = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
        0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
        0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
        0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
    };
    if (b >= 0x80 && b < 0xA0) return map[b - 0x80];
    return (wchar_t)b;
}
std::wstring AnsiString(const uint8_t* p, size_t n) {
    std::wstring s;
    s.reserve(n);
    for (size_t i = 0; i < n; i++) s += AnsiByte(p[i]);
    return s;
}
std::wstring NumText(double d) {
    if (!std::isfinite(d)) return L"";
    wchar_t buf[64];
    swprintf(buf, 64, L"%.15g", d);
    return buf;
}

void AddParagraphBlocks(Document& doc, const std::wstring& text) {
    std::wstring t = NormalizeNewlines(text);
    std::wstring cur;
    auto flush = [&] {
        std::wstring line = Trim(cur);
        cur.clear();
        if (line.empty()) return;
        line = ReplaceAll(line, L"\t", L"    ");
        Block b;
        b.kind = BlockKind::Paragraph;
        Inline in;
        in.text = line;
        b.inlines.push_back(std::move(in));
        doc.blocks.push_back(std::move(b));
        doc.docParagraphs++;
    };
    for (wchar_t c : t) {
        if (c == L'\n') flush();
        else cur += c;
    }
    flush();
    doc.text = BlockPlainText(Block{});
    doc.text.clear();
    for (auto& b : doc.blocks) {
        if (!doc.text.empty()) doc.text += L"\n";
        doc.text += BlockPlainText(b);
    }
    doc.lines = CountLines(doc.text);
}

// ================================================================ DOC
bool ReadDocText(const std::vector<uint8_t>& word, const std::vector<uint8_t>& table, std::wstring& out) {
    if (word.size() < 0x200) return false;
    uint16_t ident = U16(word.data());
    uint16_t nFib = U16(word.data() + 2);
    if (ident != 0xA5EC) return false;
    uint16_t flags = U16(word.data() + 0x0A);
    bool whichTable = (flags & 0x0200) != 0;
    (void)whichTable;
    if (nFib < 0x00C1) {
        // Word 6/95: raw text range, best effort.
        if (word.size() < 0x20) return false;
        uint32_t fcMin = U32(word.data() + 0x18);
        uint32_t fcMac = U32(word.data() + 0x1C);
        if (fcMac <= fcMin || fcMac > word.size()) return false;
        out = AnsiString(word.data() + fcMin, fcMac - fcMin);
        return true;
    }
    if (word.size() < 0x1AA || table.size() < 16) return false;
    uint32_t fcClx = U32(word.data() + 0x01A2);
    uint32_t lcbClx = U32(word.data() + 0x01A6);
    if (fcClx == 0 || lcbClx == 0 || (uint64_t)fcClx + lcbClx > table.size()) return false;
    const uint8_t* clx = table.data() + fcClx;
    size_t pos = 0;
    while (pos < lcbClx) {
        uint8_t kind = clx[pos++];
        if (kind == 1) {
            if (pos + 2 > lcbClx) return false;
            uint16_t cb = U16(clx + pos);
            pos += 2 + cb;
            continue;
        }
        if (kind == 2) {
            if (pos + 4 > lcbClx) return false;
            uint32_t lcb = U32(clx + pos);
            pos += 4;
            if (pos + lcb > lcbClx || lcb < 4) return false;
            const uint8_t* plc = clx + pos;
            uint32_t n = (lcb - 4) / 12;
            if (n == 0 || 12ull * n + 4 > lcb) return false;
            const uint8_t* cps = plc;
            const uint8_t* pcds = plc + (n + 1) * 4;
            std::wstring text;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t cp0 = U32(cps + i * 4);
                uint32_t cp1 = U32(cps + (i + 1) * 4);
                if (cp1 < cp0 || cp1 - cp0 > 10000000) continue;
                if (text.size() > 32ull * 1024 * 1024) break;
                uint32_t cch = cp1 - cp0;
                const uint8_t* pcd = pcds + i * 8;
                uint32_t fc = U32(pcd + 2);
                bool compressed = (fc & 0x40000000) != 0;
                fc &= 0x3FFFFFFF;
                if (compressed) {
                    fc /= 2;
                    if ((uint64_t)fc + cch > word.size()) continue;
                    text += AnsiString(word.data() + fc, cch);
                } else {
                    if ((uint64_t)fc + (uint64_t)cch * 2 > word.size()) continue;
                    for (uint32_t k = 0; k < cch; k++)
                        text += (wchar_t)U16(word.data() + fc + k * 2);
                }
            }
            // Strip field instructions and normalise Word control characters.
            std::wstring clean;
            int field = 0;
            bool result = true;
            for (wchar_t c : text) {
                if (c == 0x13) { field++; result = false; continue; }
                if (c == 0x14) { result = true; continue; }
                if (c == 0x15) { if (field > 0) field--; result = true; continue; }
                if (field > 0 && !result) continue;
                switch (c) {
                case L'\r': clean += L'\n'; break;
                case 0x0B: case 0x0C: clean += L'\n'; break;
                case 0x07: clean += L'\t'; break;
                case 0x1E: clean += L'-'; break;
                case 0x1F: break;
                case 0x01: case 0x08: break;
                default:
                    if (c == L'\t' || c >= 0x20) clean += c;
                    break;
                }
            }
            out = std::move(clean);
            return true;
        }
        break;
    }
    return false;
}

} // namespace

bool BuildLegacyDoc(Document& doc, const std::vector<uint8_t>& bytes, std::wstring* error) {
    CfbFile cfb;
    std::wstring err;
    if (!cfb.Open(bytes, &err)) {
        if (error) *error = L"无法读取 Word 97 复合文档：" + err;
        return false;
    }
    std::vector<uint8_t> word, table;
    if (!cfb.ReadStream(L"WordDocument", word)) {
        if (error) *error = L"Word 文件中缺少 WordDocument 流";
        return false;
    }
    bool hasTable = cfb.ReadStream(L"1Table", table) || cfb.ReadStream(L"0Table", table);
    if (!hasTable) {
        if (error) *error = L"Word 文件中缺少 Table 流";
        return false;
    }
    std::wstring text;
    if (!ReadDocText(word, table, text)) {
        if (error) *error = L"无法从 Word 97 二进制格式中提取文本";
        return false;
    }
    doc.blocks.clear();
    Block banner;
    banner.kind = BlockKind::Banner;
    banner.label = L"兼容显示";
    banner.text = L"这是旧版 Word 文档，openmd 会尽量保留正文内容；复杂版式可能无法完全还原。";
    doc.blocks.push_back(std::move(banner));
    AddParagraphBlocks(doc, text);
    if (doc.docParagraphs == 0) {
        if (error) *error = L"Word 文档中没有可显示的正文";
        return false;
    }
    doc.kind = DocKind::Doc;
    doc.sourceAllowed = false;
    doc.encoding.clear();
    doc.subtitle = L"Word 97 文档";
    doc.meta = L"段落 " + std::to_wstring(doc.docParagraphs);
    return true;
}

// ================================================================ XLS
namespace {

struct BiffRecord {
    uint16_t id = 0;
    std::vector<uint8_t> data;
};

bool ReadRecord(const std::vector<uint8_t>& s, size_t& pos, BiffRecord& rec) {
    if (pos + 4 > s.size()) return false;
    rec.id = U16(s.data() + pos);
    uint16_t len = U16(s.data() + pos + 2);
    pos += 4;
    if (pos + len > s.size()) return false;
    rec.data.assign(s.begin() + (ptrdiff_t)pos, s.begin() + (ptrdiff_t)(pos + len));
    pos += len;
    return true;
}

bool ReadXLString(const uint8_t* p, size_t n, std::wstring& out) {
    if (n < 3) return false;
    uint16_t cch = U16(p);
    uint8_t flags = p[2];
    size_t pos = 3;
    uint16_t runs = 0;
    uint32_t ext = 0;
    if (flags & 0x08) {
        if (pos + 2 > n) return false;
        runs = U16(p + pos);
        pos += 2;
    }
    if (flags & 0x04) {
        if (pos + 4 > n) return false;
        ext = U32(p + pos);
        pos += 4;
    }
    bool wide = (flags & 0x01) != 0;
    size_t need = (size_t)cch * (wide ? 2 : 1);
    if (pos + need > n) return false;
    out.clear();
    if (wide) {
        for (uint16_t i = 0; i < cch; i++) out += (wchar_t)U16(p + pos + i * 2);
    } else {
        out = AnsiString(p + pos, cch);
    }
    pos += need + (size_t)runs * 4 + ext;
    return pos <= n;
}

struct SstReader {
    std::vector<std::vector<uint8_t>> chunks;
    size_t ci = 0, off = 0;

    bool AtEnd() const { return ci >= chunks.size(); }
    bool ReadByte(uint8_t& b) {
        while (ci < chunks.size() && off >= chunks[ci].size()) { ci++; off = 0; }
        if (ci >= chunks.size()) return false;
        b = chunks[ci][off++];
        return true;
    }
    bool ReadBytes(size_t n, std::vector<uint8_t>* out = nullptr) {
        while (n > 0) {
            while (ci < chunks.size() && off >= chunks[ci].size()) { ci++; off = 0; }
            if (ci >= chunks.size()) return false;
            size_t take = std::min(n, chunks[ci].size() - off);
            if (out) out->insert(out->end(), chunks[ci].begin() + (ptrdiff_t)off,
                                 chunks[ci].begin() + (ptrdiff_t)(off + take));
            off += take;
            n -= take;
        }
        return true;
    }
    bool ReadU16(uint16_t& v) {
        uint8_t a = 0, b = 0;
        if (!ReadByte(a) || !ReadByte(b)) return false;
        v = (uint16_t)(a | (b << 8));
        return true;
    }
    bool ReadU32(uint32_t& v) {
        std::vector<uint8_t> b;
        if (!ReadBytes(4, &b)) return false;
        v = U32(b.data());
        return true;
    }
    bool ReadChars(uint16_t cch, bool wide, std::wstring& out) {
        out.clear();
        uint32_t remaining = cch;
        bool w = wide;
        while (remaining > 0) {
            while (ci < chunks.size() && off >= chunks[ci].size()) { ci++; off = 0; }
            if (ci >= chunks.size()) return false;
            size_t availBytes = chunks[ci].size() - off;
            size_t width = w ? 2 : 1;
            size_t can = std::min<size_t>(remaining, availBytes / width);
            if (can > 0) {
                const uint8_t* p = chunks[ci].data() + off;
                if (w)
                    for (size_t i = 0; i < can; i++) out += (wchar_t)U16(p + i * 2);
                else
                    out += AnsiString(p, can);
                off += can * width;
                remaining -= (uint32_t)can;
            }
            if (remaining == 0) break;
            // A string split across a CONTINUE record starts with a new option byte.
            ci++;
            off = 0;
            if (ci >= chunks.size()) return false;
            if (chunks[ci].empty()) return false;
            w = (chunks[ci][0] & 0x01) != 0;
            off = 1;
        }
        return true;
    }
};

struct SheetRef {
    std::wstring name;
    uint32_t offset = 0;
};

std::vector<std::wstring> ParseSst(const std::vector<std::vector<uint8_t>>& chunks) {
    std::vector<std::wstring> out;
    if (chunks.empty() || chunks[0].size() < 8) return out;
    uint32_t unique = U32(chunks[0].data() + 4);
    if (unique > 1000000) unique = 1000000;
    SstReader r;
    r.chunks = chunks;
    r.off = 8;
    for (uint32_t i = 0; i < unique; i++) {
        uint16_t cch = 0;
        uint8_t flags = 0;
        if (!r.ReadU16(cch)) break;
        if (!r.ReadByte(flags)) break;
        uint16_t runs = 0;
        uint32_t ext = 0;
        if ((flags & 0x08) && !r.ReadU16(runs)) break;
        if ((flags & 0x04)) {
            std::vector<uint8_t> buf;
            if (!r.ReadBytes(4, &buf)) break;
            ext = U32(buf.data());
        }
        std::wstring s;
        if (!r.ReadChars(cch, (flags & 0x01) != 0, s)) break;
        if (runs) { if (!r.ReadBytes((size_t)runs * 4)) break; }
        if (ext) { if (!r.ReadBytes(ext)) break; }
        out.push_back(std::move(s));
    }
    return out;
}

std::wstring RKText(uint32_t v) {
    double d = 0;
    if (v & 0x02) {
        int32_t iv = (int32_t)v >> 2;
        d = (double)iv;
    } else {
        uint64_t bits = (uint64_t)(v & 0xFFFFFFFC) << 32;
        memcpy(&d, &bits, 8);
    }
    if (v & 0x01) d /= 100.0;
    return NumText(d);
}

void EnsureCell(std::map<int, std::map<int, std::wstring>>& cells, int r, int c, std::wstring v) {
    if (r < 0 || c < 0 || r >= 3000 || c >= 120) return;
    cells[r][c] = std::move(v);
}

void ParseXlsSheet(const std::vector<uint8_t>& stream, uint32_t offset, const std::wstring& name,
                   const std::vector<std::wstring>& shared, Document& doc) {
    if (offset >= stream.size()) return;
    std::map<int, std::map<int, std::wstring>> cells;
    size_t pos = offset;
    BiffRecord rec;
    int pendingRow = -1, pendingCol = -1;
    int guard = 0;
    while (pos < stream.size() && guard++ < 2000000) {
        if (!ReadRecord(stream, pos, rec)) break;
        if (rec.id == 0x000A) break; // EOF
        const uint8_t* d = rec.data.data();
        size_t n = rec.data.size();
        switch (rec.id) {
        case 0x00FD: // LABELSST
            if (n >= 10) {
                int r = U16(d), c = U16(d + 2);
                uint32_t idx = U32(d + 6);
                if (idx < shared.size()) EnsureCell(cells, r, c, shared[idx]);
            }
            break;
        case 0x0204: { // LABEL
            if (n >= 8) {
                int r = U16(d), c = U16(d + 2);
                std::wstring s;
                if (ReadXLString(d + 6, n - 6, s)) EnsureCell(cells, r, c, std::move(s));
            }
            break;
        }
        case 0x0203: // NUMBER
            if (n >= 14) EnsureCell(cells, U16(d), U16(d + 2), NumText(F64(d + 6)));
            break;
        case 0x027E: // RK
            if (n >= 10) EnsureCell(cells, U16(d), U16(d + 2), RKText(U32(d + 6)));
            break;
        case 0x00BD: { // MULRK
            if (n >= 6) {
                int r = U16(d);
                int first = U16(d + 2);
                int count = (int)((n - 6) / 6);
                for (int k = 0; k < count; k++) {
                    uint32_t rk = U32(d + 4 + k * 6 + 2);
                    EnsureCell(cells, r, first + k, RKText(rk));
                }
            }
            break;
        }
        case 0x0006: // FORMULA
            if (n >= 14) {
                int r = U16(d), c = U16(d + 2);
                uint64_t raw = 0;
                memcpy(&raw, d + 6, 8);
                double val = F64(d + 6);
                if (std::isfinite(val) && !(raw & 0xFFFF000000000000ull)) {
                    EnsureCell(cells, r, c, NumText(val));
                } else {
                    pendingRow = r;
                    pendingCol = c;
                }
            }
            break;
        case 0x0207: { // STRING (formula result)
            if (pendingRow >= 0 && n >= 3) {
                std::wstring s;
                if (ReadXLString(d, n, s)) EnsureCell(cells, pendingRow, pendingCol, std::move(s));
                pendingRow = pendingCol = -1;
            }
            break;
        }
        case 0x0205: // BOOLERR
            if (n >= 8) {
                int r = U16(d), c = U16(d + 2);
                if (d[7] == 0) EnsureCell(cells, r, c, d[6] ? L"TRUE" : L"FALSE");
                else EnsureCell(cells, r, c, L"#ERROR");
            }
            break;
        default:
            break;
        }
    }
    if (cells.empty()) return;
    GridSheet sheet;
    sheet.name = name.empty() ? L"Sheet" : name;
    int maxR = 0, maxC = 0;
    for (auto& r : cells) {
        maxR = std::max(maxR, r.first);
        for (auto& c : r.second) maxC = std::max(maxC, c.first);
    }
    sheet.rows.resize((size_t)maxR + 1);
    for (auto& r : cells) {
        auto& row = sheet.rows[(size_t)r.first];
        row.resize((size_t)maxC + 1);
        for (auto& c : r.second) row[(size_t)c.first] = c.second;
    }
    sheet.maxCols = maxC + 1;
    doc.csvRows += (int)sheet.rows.size();
    doc.grid.sheets.push_back(std::move(sheet));
}

} // namespace

bool BuildLegacyXls(Document& doc, const std::vector<uint8_t>& bytes, std::wstring* error) {
    CfbFile cfb;
    std::wstring err;
    if (!cfb.Open(bytes, &err)) {
        if (error) *error = L"无法读取 Excel 97 复合文档：" + err;
        return false;
    }
    std::vector<uint8_t> stream;
    if (!cfb.ReadStream(L"Workbook", stream) && !cfb.ReadStream(L"Book", stream)) {
        if (error) *error = L"Excel 文件中缺少 Workbook 流";
        return false;
    }
    std::vector<SheetRef> sheets;
    std::vector<std::vector<uint8_t>> sstChunks;
    bool inSst = false;
    size_t pos = 0;
    BiffRecord rec;
    while (ReadRecord(stream, pos, rec)) {
        if (rec.id == 0x0085 && rec.data.size() >= 8) { // BOUNDSHEET
            const uint8_t* d = rec.data.data();
            SheetRef s;
            s.offset = U32(d);
            uint8_t cch = d[6];
            uint8_t flags = d[7];
            size_t need = (size_t)cch * ((flags & 0x01) ? 2 : 1);
            if (8 + need <= rec.data.size()) {
                if (flags & 0x01)
                    for (uint8_t i = 0; i < cch; i++) s.name += (wchar_t)U16(d + 8 + i * 2);
                else
                    s.name = AnsiString(d + 8, cch);
            }
            sheets.push_back(std::move(s));
        } else if (rec.id == 0x00FC) { // SST
            sstChunks.clear();
            sstChunks.push_back(rec.data);
            inSst = true;
        } else if (rec.id == 0x003C && inSst) { // CONTINUE
            sstChunks.push_back(rec.data);
        } else {
            inSst = false;
        }
        if (rec.id == 0x000A && !sheets.empty()) {
            // Keep scanning: SST is normally before the sheets; boundsheets too.
        }
    }
    std::vector<std::wstring> shared = ParseSst(sstChunks);
    if (sheets.empty()) {
        if (error) *error = L"Excel 工作簿中没有工作表";
        return false;
    }
    for (size_t i = 0; i < sheets.size(); i++)
        ParseXlsSheet(stream, sheets[i].offset, sheets[i].name, shared, doc);
    if (doc.grid.sheets.empty()) {
        if (error) *error = L"Excel 工作簿中没有可显示的数据";
        return false;
    }
    doc.kind = DocKind::Xls;
    doc.grid.fromExcel = true;
    doc.grid.active = 0;
    doc.sourceAllowed = false;
    doc.encoding.clear();
    doc.subtitle = L"Excel 97 工作簿 · " + std::to_wstring(doc.grid.sheets.size()) + L" 个工作表";
    doc.meta = L"以表格形式显示单元格内容";
    return true;
}
