// Document model shared by the parsers and the renderer.
#pragma once
#include "common.h"
#include "json.h"
#include <memory>
#include <vector>
#include <string>

struct MathNode;

enum class BlockKind {
    Heading, Paragraph, ListItem, Quote, Code, Table, Image, Hr, Math,
    Session, ChatHeader, Reasoning, Tool, CellHeader, OutputText, OutputImage, Banner
};

enum class InlineRole {
    Text, Muted, Accent, Danger, CodeKeyword, CodeString, CodeNumber,
    CodeComment, CodeType, CodePunct, Success
};

struct Inline {
    std::wstring text;
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strike = false;
    bool code = false;
    bool link = false;
    bool highlight = false;
    std::wstring url;
    InlineRole role = InlineRole::Text;
    std::shared_ptr<MathNode> math;
};

struct ImageRef {
    std::wstring path;             // local path (resolved later against baseDir)
    std::wstring mime;
    std::vector<uint8_t> bytes;    // embedded data (wins over path)
    std::wstring alt;
    std::wstring cacheKey;
};

struct TableCell {
    std::vector<Inline> inlines;
    int align = 0;                 // -1 left, 0 center, 1 right
    bool header = false;
};

struct TableData {
    std::vector<std::vector<TableCell>> rows;
    std::vector<int> aligns;
};

struct Block {
    BlockKind kind = BlockKind::Paragraph;
    std::vector<Inline> inlines;
    std::wstring text;             // code / plain output
    std::wstring lang;
    std::wstring badge;            // "In [3] · python"
    std::wstring label;            // small label above code/output
    std::wstring anchor;
    int level = 0;                 // headings
    int align = 0;                 // 0 left, 1 center, 2 right
    int indent = 0;                // list nesting, in 22px steps
    int number = 0;                // ordered list number
    bool ordered = false;
    bool task = false;
    bool checked = false;
    bool quote = false;
    bool output = false;
    bool error = false;
    bool expanded = false;
    bool collapsible = false;
    std::wstring summary;          // reasoning/tool collapsed summary
    TableData table;
    std::shared_ptr<ImageRef> image;
    std::shared_ptr<MathNode> math;
    int group = -1;                // chat message group id
    int chatRole = 0;              // 0 none, 1 user, 2 assistant, 3 system
    bool chatHeader = false;
    int sourceLine = 0;            // optional source mapping
};

struct Heading {
    int level = 0;
    std::wstring text;
    int block = -1;
};

struct GridSheet {
    std::wstring name;
    std::vector<std::vector<std::wstring>> rows;
    int maxCols = 0;
};

struct GridModel {
    std::vector<GridSheet> sheets;
    int active = 0;
    bool truncated = false;
    std::wstring note;
    bool fromExcel = false;
};

struct Document {
    DocKind kind = DocKind::None;
    std::wstring path;
    std::wstring title;
    std::wstring subtitle;
    std::wstring meta;
    std::wstring text;             // original/decode text for source mode
    std::wstring encoding;
    TextEncoding encodingId = TextEncoding::Auto;
    DocLanguage language = DocLanguage::Auto;
    int encodingConfidence = 100;
    bool encodingRepaired = false;
    bool encodingUncertain = false;
    std::wstring error;
    std::vector<Block> blocks;
    std::vector<Heading> outline;
    JNode json;
    GridModel grid;
    std::shared_ptr<ImageRef> image;
    int imageW = 0;
    int imageH = 0;
    uint64_t size = 0;
    int lines = 0;
    int jsonNodes = 0;
    int csvRows = 0;
    int docParagraphs = 0;
    bool truncated = false;
    bool sourceDefault = false;
    bool sourceAllowed = true;
    double renderMs = 0.0;
    int pdfPages = 0;

    bool Empty() const { return kind == DocKind::None; }
};

std::wstring BlockPlainText(const Block& b);
std::wstring BlocksPlainText(const std::vector<Block>& blocks);
