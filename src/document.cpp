#include "document.h"
#include "file_policy.h"
#include "json.h"
#include "office.h"
#include "latex.h"
#include "render.h"
#include <chrono>
#include <algorithm>
#include <cwctype>
#include <exception>
#include <map>

std::wstring BlockPlainText(const Block& b) {
    switch (b.kind) {
    case BlockKind::Code:
    case BlockKind::OutputText:
        return b.text;
    case BlockKind::Table: {
        std::wstring out;
        for (auto& row : b.table.rows) {
            for (size_t c = 0; c < row.size(); c++) {
                if (c) out += L"\t";
                for (auto& r : row[c].inlines) out += r.text;
            }
            out += L"\n";
        }
        return out;
    }
    case BlockKind::Image:
    case BlockKind::OutputImage:
        return b.text;
    case BlockKind::Math:
        return b.math ? MathPlainText(*b.math) : L"";
    default: {
        std::wstring out;
        if (b.kind == BlockKind::ListItem) {
            if (b.task) out += b.checked ? L"[x] " : L"[ ] ";
            else if (b.ordered) out += std::to_wstring(b.number) + L". ";
            else out += L"• ";
        }
        for (auto& r : b.inlines) {
            if (r.math) out += MathPlainText(*r.math);
            else out += r.text;
        }
        return out;
    }
    }
}

std::wstring BlocksPlainText(const std::vector<Block>& blocks) {
    std::wstring out;
    for (auto& b : blocks) {
        std::wstring t = BlockPlainText(b);
        if (t.empty()) continue;
        if (!out.empty()) out += L"\n";
        out += t;
    }
    return out;
}

namespace {

std::wstring TextOfNode(const JNode* n) {
    if (!n) return L"";
    if (n->kind == JKind::String) return n->str;
    if (n->kind == JKind::Number) return n->raw;
    if (n->kind == JKind::Bool) return n->boolean ? L"true" : L"false";
    if (n->kind == JKind::Array) {
        std::wstring out;
        for (auto& item : n->items) out += TextOfNode(&item);
        return out;
    }
    return L"";
}

const JNode* GetNode(const JNode* n, const std::wstring& key) {
    return n ? n->Get(key) : nullptr;
}
std::wstring GetStr(const JNode* n, const std::wstring& key) {
    return TextOfNode(GetNode(n, key));
}
long long GetLong(const JNode* n, const std::wstring& key) {
    const JNode* v = GetNode(n, key);
    if (!v || v->kind != JKind::Number) return 0;
    return (long long)v->number;
}

std::wstring StripAnsi(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == 0x1B && i + 1 < s.size() && s[i + 1] == L'[') {
            i += 2;
            while (i < s.size() && !(s[i] >= L'A' && s[i] <= L'Z') && !(s[i] >= L'a' && s[i] <= L'z')) i++;
            continue;
        }
        if (s[i] == 0x1B && i + 1 < s.size() && s[i + 1] == L']') {
            i += 2;
            while (i < s.size() && s[i] != 7 && s[i] != 0x1B) i++;
            continue;
        }
        out += s[i];
    }
    return out;
}

std::wstring HtmlToText(const std::wstring& html) {
    std::wstring s = html;
    // remove script/style blocks
    for (const wchar_t* tag : { L"script", L"style" }) {
        for (;;) {
            std::wstring open = std::wstring(L"<") + tag;
            size_t a = ToLower(s).find(open);
            if (a == std::wstring::npos) break;
            size_t close = ToLower(s).find(std::wstring(L"</") + tag, a);
            if (close == std::wstring::npos) { s.erase(a); break; }
            size_t end = s.find(L'>', close);
            if (end == std::wstring::npos) { s.erase(a); break; }
            s.erase(a, end - a + 1);
        }
    }
    std::wstring out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'<') {
            size_t e = s.find(L'>', i);
            if (e == std::wstring::npos) break;
            std::wstring t = ToLower(s.substr(i, e - i + 1));
            if (t.rfind(L"<br", 0) == 0 || t.rfind(L"</p", 0) == 0 || t.rfind(L"</div", 0) == 0 ||
                t.rfind(L"</tr", 0) == 0 || t.rfind(L"</li", 0) == 0)
                out += L'\n';
            else if (t.rfind(L"<td", 0) == 0)
                out += L' ';
            i = e;
            continue;
        }
        out += s[i];
    }
    out = ReplaceAll(out, L"&lt;", L"<");
    out = ReplaceAll(out, L"&gt;", L">");
    out = ReplaceAll(out, L"&amp;", L"&");
    out = ReplaceAll(out, L"&quot;", L"\"");
    out = ReplaceAll(out, L"&nbsp;", L" ");
    // collapse excessive blank lines
    while (out.find(L"\n\n\n") != std::wstring::npos) out = ReplaceAll(out, L"\n\n\n", L"\n\n");
    return Trim(out);
}

std::wstring FormatUnixMs(long long ms) {
    if (ms <= 0) return L"";
    ULARGE_INTEGER u;
    u.QuadPart = (uint64_t)ms * 10000ull + 116444736000000000ull;
    FILETIME ft{u.LowPart, u.HighPart}, local;
    FileTimeToLocalFileTime(&ft, &local);
    SYSTEMTIME st;
    FileTimeToSystemTime(&local, &st);
    wchar_t buf[64];
    swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

void AddOutlineFromBlocks(Document& doc, size_t start) {
    for (size_t i = start; i < doc.blocks.size(); i++) {
        Block& b = doc.blocks[i];
        if (b.kind != BlockKind::Heading) continue;
        std::wstring text = BlocksPlainText({ b });
        text = Trim(ReplaceAll(text, L"\n", L" "));
        if (text.empty()) continue;
        doc.outline.push_back({ b.level, text, (int)i });
    }
}

void AddMarkdownBlocks(Document& doc, const std::wstring& text, const MarkdownContext& ctx) {
    size_t start = doc.blocks.size();
    std::vector<Block> blocks = ParseMarkdown(text, ctx);
    for (auto& b : blocks) doc.blocks.push_back(std::move(b));
    AddOutlineFromBlocks(doc, start);
}

void ParseNotebook(Document& doc, const std::wstring& text, const JNode& root, std::wstring& error) {
    if (root.kind != JKind::Object) { error = L"不是有效的 Jupyter Notebook"; return; }
    const JNode* cells = root.Get(L"cells");
    if (!cells || cells->kind != JKind::Array) { error = L"notebook 中缺少 cells 数组"; return; }
    const JNode* meta = root.Get(L"metadata");
    std::wstring kernel = GetStr(GetNode(meta, L"kernelspec"), L"display_name");
    std::wstring language = GetStr(GetNode(meta, L"kernelspec"), L"language");
    if (language.empty()) language = GetStr(GetNode(meta, L"language_info"), L"name");
    if (language.empty()) language = L"python";
    std::wstring langLower = ToLower(language);

    int mdCount = 0, codeCount = 0, outCount = 0;
    Block banner;
    banner.kind = BlockKind::Session;
    banner.label = L"Jupyter Notebook";
    banner.text = kernel.empty() ? L"Notebook" : kernel;
    banner.badge = L"内核 " + language + L" · " + std::to_wstring(cells->items.size()) + L" 个单元";
    size_t bannerIndex = doc.blocks.size();
    doc.blocks.push_back(std::move(banner));

    MarkdownContext ctx;
    ctx.baseDir = PathDir(doc.path);
    for (auto& cn : cells->items) {
        if (cn.kind != JKind::Object) continue;
        std::wstring type = GetStr(&cn, L"cell_type");
        std::wstring source = TextOfNode(cn.Get(L"source"));
        std::map<std::wstring, std::vector<uint8_t>> attachments;
        const JNode* att = cn.Get(L"attachments");
        if (att && att->kind == JKind::Object) {
            for (auto& m : att->members) {
                if (m.second.kind != JKind::Object) continue;
                for (auto& mm : m.second.members) {
                    std::wstring b64 = TextOfNode(&mm.second);
                    if (b64.empty()) continue;
                    std::vector<uint8_t> bytes = Base64DecodeBytes(b64);
                    if (!bytes.empty()) {
                        attachments[m.first] = std::move(bytes);
                        break;
                    }
                }
            }
        }
        ctx.attachments = &attachments;

        if (type == L"markdown") {
            mdCount++;
            AddMarkdownBlocks(doc, source, ctx);
        } else if (type == L"code") {
            codeCount++;
            Block b;
            b.kind = BlockKind::Code;
            b.text = source;
            b.lang = langLower;
            int exec = -1;
            const JNode* ec = cn.Get(L"execution_count");
            if (ec && ec->kind == JKind::Number) exec = (int)ec->number;
            b.badge = exec >= 0 ? L"In [" + std::to_wstring(exec) + L"] · " + langLower : L"In [ ] · " + langLower;
            b.inlines = HighlightCode(source, langLower);
            doc.blocks.push_back(std::move(b));

            const JNode* outputs = cn.Get(L"outputs");
            if (outputs && outputs->kind == JKind::Array) {
                for (auto& on : outputs->items) {
                    if (on.kind != JKind::Object) continue;
                    std::wstring kind = GetStr(&on, L"output_type");
                    if (kind == L"stream") {
                        std::wstring t = StripAnsi(TextOfNode(on.Get(L"text")));
                        if (!Trim(t).empty()) {
                            Block ob;
                            ob.kind = BlockKind::OutputText;
                            ob.label = L"输出";
                            ob.text = t;
                            ob.output = true;
                            doc.blocks.push_back(std::move(ob));
                            outCount++;
                        }
                    } else if (kind == L"error") {
                        std::wstring name = GetStr(&on, L"ename");
                        std::wstring value = GetStr(&on, L"evalue");
                        std::wstring tb = StripAnsi(TextOfNode(on.Get(L"traceback")));
                        std::wstring all;
                        if (!name.empty()) all += name;
                        if (!value.empty()) { if (!all.empty()) all += L": "; all += value; }
                        if (!tb.empty()) { if (!all.empty()) all += L"\n"; all += tb; }
                        if (!Trim(all).empty()) {
                            Block ob;
                            ob.kind = BlockKind::OutputText;
                            ob.label = L"错误";
                            ob.text = all;
                            ob.output = true;
                            ob.error = true;
                            doc.blocks.push_back(std::move(ob));
                            outCount++;
                        }
                    } else if (kind == L"execute_result" || kind == L"display_data") {
                        const JNode* data = on.Get(L"data");
                        if (!data || data->kind != JKind::Object) continue;
                        std::vector<uint8_t> image;
                        std::wstring mime;
                        for (const wchar_t* key : { L"image/png", L"image/jpeg", L"image/gif", L"image/bmp" }) {
                            std::wstring b64 = TextOfNode(data->Get(key));
                            if (!b64.empty()) {
                                image = Base64DecodeBytes(b64);
                                mime = key;
                                if (!image.empty()) break;
                            }
                        }
                        if (!image.empty()) {
                            Block ob;
                            ob.kind = BlockKind::OutputImage;
                            ob.label = L"输出图片";
                            auto img = std::make_shared<ImageRef>();
                            img->bytes = std::move(image);
                            img->mime = mime;
                            img->cacheKey = L"nbpix:" + std::to_wstring(doc.blocks.size()) + L":" + std::to_wstring(img->bytes.size());
                            ob.image = img;
                            doc.blocks.push_back(std::move(ob));
                            outCount++;
                            continue;
                        }
                        std::wstring latex = TextOfNode(data->Get(L"text/latex"));
                        if (!latex.empty()) {
                            std::wstring tex = Trim(latex);
                            if (tex.size() >= 4 && tex.substr(0, 2) == L"$$" && tex.substr(tex.size() - 2) == L"$$")
                                tex = tex.substr(2, tex.size() - 4);
                            else if (tex.size() >= 4 && tex.substr(0, 2) == L"\\[" && tex.substr(tex.size() - 2) == L"\\]")
                                tex = tex.substr(2, tex.size() - 4);
                            auto node = ParseMath(tex, true);
                            if (node) {
                                Block ob;
                                ob.kind = BlockKind::Math;
                                ob.label = L"输出公式";
                                ob.math = node;
                                ob.output = true;
                                doc.blocks.push_back(std::move(ob));
                                outCount++;
                            }
                            continue;
                        }
                        std::wstring plain = TextOfNode(data->Get(L"text/plain"));
                        if (plain.empty()) plain = HtmlToText(TextOfNode(data->Get(L"text/html")));
                        if (plain.empty()) plain = TextOfNode(data->Get(L"application/json"));
                        if (!Trim(plain).empty()) {
                            Block ob;
                            ob.kind = BlockKind::OutputText;
                            ob.label = L"输出";
                            ob.text = StripAnsi(plain);
                            ob.output = true;
                            doc.blocks.push_back(std::move(ob));
                            outCount++;
                        }
                    }
                }
            }
        } else {
            // raw cell (and unknown types)
            if (!Trim(source).empty()) {
                Block b;
                b.kind = BlockKind::Code;
                b.text = source;
                b.lang = L"text";
                b.badge = L"Raw";
                b.inlines = HighlightCode(source, L"");
                doc.blocks.push_back(std::move(b));
            }
        }
        ctx.attachments = nullptr;
    }
    // Update banner badge with final counts
    if (bannerIndex < doc.blocks.size()) {
        doc.blocks[bannerIndex].badge = L"内核 " + language + L" · " + std::to_wstring(mdCount) +
            L" 个 Markdown / " + std::to_wstring(codeCount) + L" 个代码 / " + std::to_wstring(outCount) + L" 个输出";
    }
    doc.title = kernel.empty() ? PathName(doc.path) : kernel;
    doc.subtitle = L"Jupyter Notebook · " + language;
    doc.meta = std::to_wstring(mdCount) + L" md · " + std::to_wstring(codeCount) + L" code";
    doc.sourceDefault = text.size() > 8ull * 1024 * 1024;
}

void SetChatBlocks(Document& doc, std::vector<Block> blocks, int group, int role) {
    for (auto& b : blocks) {
        b.group = group;
        b.chatRole = role;
        doc.blocks.push_back(std::move(b));
    }
}

void ParseChat(Document& doc, const std::wstring& text, const JNode& root, std::wstring& error) {
    const JNode* info = root.Get(L"info");
    const JNode* msgs = root.Get(L"messages");
    if (!info || info->kind != JKind::Object || !msgs || msgs->kind != JKind::Array) {
        error = L"不是有效的 OpenCode 会话记录";
        return;
    }
    std::wstring title = GetStr(info, L"title");
    if (title.empty()) title = L"对话记录";
    std::wstring agent = GetStr(info, L"agent");
    std::wstring model = GetStr(info->Get(L"model"), L"id");
    std::wstring provider = GetStr(info->Get(L"model"), L"providerID");
    long long created = GetLong(info->Get(L"time"), L"created");
    long long tin = GetLong(info->Get(L"tokens"), L"input");
    long long tout = GetLong(info->Get(L"tokens"), L"output");
    long long treason = GetLong(info->Get(L"tokens"), L"reasoning");

    Block header;
    header.kind = BlockKind::Session;
    header.label = L"OpenCode 会话";
    header.text = title;
    std::wstring meta;
    if (!agent.empty()) meta += agent;
    if (!model.empty()) { if (!meta.empty()) meta += L" · "; meta += provider.empty() ? model : provider + L"/" + model; }
    meta += L" · " + std::to_wstring(msgs->items.size()) + L" 条消息";
    if (tin || tout) meta += L" · tokens " + std::to_wstring(tin) + L" ↓ / " + std::to_wstring(tout) + L" ↑";
    if (treason) meta += L" · 思考 " + std::to_wstring(treason);
    header.badge = meta;
    if (created) header.label += L" · " + FormatUnixMs(created);
    doc.blocks.push_back(std::move(header));

    MarkdownContext ctx;
    ctx.baseDir = PathDir(doc.path);
    int group = 0;
    int userCount = 0, assistantCount = 0;
    for (auto& mn : msgs->items) {
        if (mn.kind != JKind::Object) continue;
        std::wstring type = GetStr(&mn, L"type");
        if (type == L"user") {
            group++;
            userCount++;
            Block h;
            h.kind = BlockKind::ChatHeader;
            h.chatRole = 1;
            h.group = group;
            h.label = L"你";
            long long t = GetLong(mn.Get(L"time"), L"created");
            std::wstring stamp = FormatUnixMs(t);
            const JNode* metaNode = mn.Get(L"metadata");
            std::wstring display = GetStr(metaNode, L"displayText");
            std::wstring agentName = GetStr(metaNode, L"agent");
            if (!agentName.empty()) h.label += L" · " + agentName;
            if (!stamp.empty()) h.badge = stamp;
            doc.blocks.push_back(std::move(h));
            std::wstring body = display;
            if (body.empty()) body = TextOfNode(mn.Get(L"text"));
            // quoted selections are shown above the message
            const JNode* comments = GetNode(metaNode, L"comments");
            if (comments && comments->kind == JKind::Array) {
                for (auto& cn2 : comments->items) {
                    if (GetStr(&cn2, L"type") != L"quote") continue;
                    std::wstring q = TextOfNode(cn2.Get(L"text"));
                    if (Trim(q).empty()) continue;
                    Block qb;
                    qb.kind = BlockKind::Quote;
                    qb.chatRole = 1;
                    qb.group = group;
                    Inline quoteInline;
                    quoteInline.text = q;
                    qb.inlines.push_back(std::move(quoteInline));
                    doc.blocks.push_back(std::move(qb));
                }
            }
            SetChatBlocks(doc, ParseMarkdown(body, ctx), group, 1);
        } else if (type == L"assistant") {
            group++;
            assistantCount++;
            Block h;
            h.kind = BlockKind::ChatHeader;
            h.chatRole = 2;
            h.group = group;
            h.label = L"Assistant";
            std::wstring aModel = GetStr(mn.Get(L"model"), L"id");
            if (!aModel.empty()) h.label += L" · " + aModel;
            std::wstring aAgent = GetStr(&mn, L"agent");
            if (!aAgent.empty()) h.badge = aAgent;
            long long t = GetLong(mn.Get(L"time"), L"created");
            if (t) h.badge = (h.badge.empty() ? L"" : h.badge + L" · ") + FormatUnixMs(t);
            doc.blocks.push_back(std::move(h));

            const JNode* content = mn.Get(L"content");
            if (!content || content->kind != JKind::Array) continue;
            for (auto& pn : content->items) {
                if (pn.kind != JKind::Object) continue;
                std::wstring pt = GetStr(&pn, L"type");
                if (pt == L"text") {
                    std::wstring body = TextOfNode(pn.Get(L"text"));
                    if (!Trim(body).empty())
                        SetChatBlocks(doc, ParseMarkdown(body, ctx), group, 2);
                } else if (pt == L"reasoning") {
                    std::wstring body = StripAnsi(TextOfNode(pn.Get(L"text")));
                    if (Trim(body).empty()) continue;
                    Block rb;
                    rb.kind = BlockKind::Reasoning;
                    rb.chatRole = 2;
                    rb.group = group;
                    rb.collapsible = true;
                    rb.text = body;
                    std::wstring first = Trim(SplitLines(body).empty() ? L"" : SplitLines(body)[0]);
                    rb.summary = L"思考过程" + (first.empty() ? L"" : L" · " + Shorten(first, 90));
                    doc.blocks.push_back(std::move(rb));
                } else if (pt == L"tool") {
                    std::wstring tool = GetStr(&pn, L"tool");
                    if (tool.empty()) tool = GetStr(&pn, L"name");
                    const JNode* state = pn.Get(L"state");
                    std::wstring status = GetStr(state, L"status");
                    std::wstring input = GetStr(state, L"input");
                    std::wstring output = GetStr(state, L"output");
                    std::wstring all;
                    if (!input.empty()) all += L"输入:\n" + input;
                    if (!output.empty()) { if (!all.empty()) all += L"\n\n"; all += L"输出:\n" + output; }
                    Block tb;
                    tb.kind = BlockKind::Tool;
                    tb.chatRole = 2;
                    tb.group = group;
                    tb.collapsible = !Trim(all).empty();
                    tb.label = L"工具 · " + (tool.empty() ? L"tool" : tool) + (status.empty() ? L"" : L" · " + status);
                    tb.text = all;
                    std::wstring first = Trim(SplitLines(all).empty() ? L"" : SplitLines(all)[0]);
                    tb.summary = tb.label + (first.empty() ? L"" : L" · " + Shorten(first, 80));
                    doc.blocks.push_back(std::move(tb));
                }
            }
        }
    }
    doc.title = title;
    doc.subtitle = L"OpenCode 对话记录 · " + std::to_wstring(userCount) + L" 条提问 / " +
                   std::to_wstring(assistantCount) + L" 条回复";
    doc.meta = meta;
    doc.sourceDefault = text.size() > 8ull * 1024 * 1024;
}

int CountJsonNodes(const JNode& n) {
    int count = 1;
    for (auto& i : n.items) count += CountJsonNodes(i);
    for (auto& m : n.members) count += CountJsonNodes(m.second);
    return count;
}

} // namespace

void AppendMarkdownToDocument(Document& doc, const std::wstring& text, const MarkdownContext& ctx) {
    AddMarkdownBlocks(doc, text, ctx);
}

static std::unique_ptr<Document> LoadDocumentImpl(const std::wstring& pathIn, std::wstring& error,
                                                 TextEncoding encoding, DocLanguage language,
                                                 bool repairMojibake) {
    error.clear();
    auto startTime = std::chrono::steady_clock::now();
    std::wstring path = AbsolutePath(pathIn);
    if (IsBlockedBinaryFile(path)) {
        error = L"不支持打开该文件类型：" + PathExt(path) + L"。";
        return nullptr;
    }
    if (!FileExists(path)) {
        error = L"文件不存在或无法访问：\n" + path;
        return nullptr;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad);
    uint64_t size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (size > 512ull * 1024 * 1024) {
        error = L"文件超过 512 MB，LitheMD 已停止加载";
        return nullptr;
    }

    auto doc = std::make_unique<Document>();
    doc->path = path;
    doc->size = size;
    doc->title = PathName(path);
    DocKind extKind = DetectKind(path, L"");
    std::wstring ext = PathExt(path);

    if (extKind == DocKind::Image) {
        auto img = std::make_shared<ImageRef>();
        img->path = path;
        img->cacheKey = path;
        float iw = 0, ih = 0;
        if (!ImagePixelSize(*img, iw, ih) || iw < 1 || ih < 1) {
            error = L"无法解码这张图片，或当前系统不支持该图片格式";
            return nullptr;
        }
        doc->kind = DocKind::Image;
        doc->image = img;
        doc->imageW = (int)iw;
        doc->imageH = (int)ih;
        doc->encoding.clear();
        doc->sourceAllowed = false;
        doc->subtitle = L"图片";
        doc->meta = std::to_wstring((int)iw) + L" × " + std::to_wstring((int)ih);
        doc->renderMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime).count();
        return doc;
    }

    // Binary formats are read as bytes and parsed directly.
    if (extKind == DocKind::Docx || extKind == DocKind::Doc ||
        extKind == DocKind::Xlsx || extKind == DocKind::Xls || extKind == DocKind::Pdf) {
        std::vector<uint8_t> bytes;
        std::wstring readErr;
        if (!ReadFileBytes(path, bytes, &readErr)) { error = readErr; return nullptr; }
        if (extKind == DocKind::Pdf) {
            if (bytes.size() < 4 || memcmp(bytes.data(), "%PDF", 4) != 0) {
                error = L"不是有效的 PDF 文件";
                return nullptr;
            }
            doc->kind = DocKind::Pdf;
            doc->encoding.clear();
            doc->sourceAllowed = false;
            doc->subtitle = L"PDF 文档";
            doc->meta = L"按页显示";
            doc->renderMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime).count();
            return doc;
        }
        bool ok = false;
        if (extKind == DocKind::Docx) ok = BuildDocx(*doc, bytes, &error);
        else if (extKind == DocKind::Xlsx) ok = BuildXlsx(*doc, bytes, &error);
        else if (extKind == DocKind::Doc) ok = BuildLegacyDoc(*doc, bytes, &error);
        else if (extKind == DocKind::Xls) ok = BuildLegacyXls(*doc, bytes, &error);
        if (!ok) {
            if (error.empty()) error = L"无法解析该 Office 文档";
            return nullptr;
        }
        doc->encoding.clear();
        doc->size = size;
        doc->title = doc->title.empty() ? PathName(path) : doc->title;
        doc->renderMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime).count();
        return doc;
    }

    std::vector<uint8_t> bytes;
    std::wstring readErr;
    if (!ReadFileBytes(path, bytes, &readErr)) { error = readErr; return nullptr; }
    DecodedText decoded = DecodeText(bytes, encoding, language, repairMojibake);
    if (!decoded.ok) {
        error = decoded.error.empty() ? L"无法解码该文件" : decoded.error;
        return nullptr;
    }
    doc->text = NormalizeNewlines(decoded.text);
    doc->encoding = decoded.encodingName;
    doc->encodingId = decoded.encoding;
    doc->language = decoded.language;
    doc->encodingConfidence = decoded.confidence;
    doc->encodingRepaired = decoded.repaired;
    doc->encodingUncertain = decoded.uncertain;
    doc->lines = CountLines(doc->text);

    DocKind kind = DetectKind(path, doc->text);
    doc->kind = kind;
    doc->sourceAllowed = CanShowSource(kind);

    if (kind == DocKind::Markdown) {
        MarkdownContext ctx;
        ctx.baseDir = PathDir(path);
        AddMarkdownBlocks(*doc, doc->text, ctx);
        doc->subtitle = L"Markdown";
        doc->sourceDefault = doc->text.size() > 1800ull * 1024;
        doc->meta = std::to_wstring(doc->blocks.size()) + L" 个块";
    } else if (kind == DocKind::Json) {
        JNode root;
        std::wstring jerr;
        if (!ParseJson(doc->text, root, &jerr)) {
            error = L"JSON 解析失败：" + jerr;
            return nullptr;
        }
        doc->json = std::move(root);
        doc->jsonNodes = CountJsonNodes(doc->json);
        doc->subtitle = L"JSON 结构";
        doc->meta = std::to_wstring(doc->jsonNodes) + L" 个节点";
    } else if (kind == DocKind::Notebook) {
        JNode root;
        std::wstring jerr;
        if (!ParseJson(doc->text, root, &jerr)) {
            error = L"Notebook 解析失败：" + jerr;
            return nullptr;
        }
        ParseNotebook(*doc, doc->text, root, error);
        if (!error.empty()) return nullptr;
    } else if (kind == DocKind::Chat) {
        JNode root;
        std::wstring jerr;
        if (!ParseJson(doc->text, root, &jerr)) {
            error = L"会话记录解析失败：" + jerr;
            return nullptr;
        }
        ParseChat(*doc, doc->text, root, error);
        if (!error.empty()) return nullptr;
    } else if (kind == DocKind::Csv) {
        if (!BuildCsv(*doc, doc->text, &error)) return nullptr;
        doc->sourceAllowed = true;
    } else {
        doc->kind = DocKind::Text;
        doc->subtitle = L"纯文本";
        doc->sourceDefault = true;
        doc->sourceAllowed = true;
        doc->meta = std::to_wstring(doc->lines) + L" 行";
    }
    doc->renderMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startTime).count();
    return doc;
}

std::unique_ptr<Document> LoadDocument(const std::wstring& pathIn, std::wstring& error,
                                       TextEncoding encoding, DocLanguage language,
                                       bool repairMojibake) {
    try {
        return LoadDocumentImpl(pathIn, error, encoding, language, repairMojibake);
    } catch (const std::bad_alloc&) {
        error = L"文件过大或格式异常，内存不足，已停止解析";
    } catch (const std::exception& e) {
        error = L"文件解析失败：" + WideFromUtf8(e.what());
    } catch (...) {
        error = L"文件解析失败，格式可能已损坏";
    }
    return nullptr;
}
