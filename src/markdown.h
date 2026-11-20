// Markdown parser and small syntax highlighter.
#pragma once
#include "docmodel.h"
#include <map>

struct MarkdownContext {
    std::wstring baseDir;
    const std::map<std::wstring, std::vector<uint8_t>>* attachments = nullptr;
};

std::vector<Block> ParseMarkdown(const std::wstring& text, const MarkdownContext& ctx);
std::vector<Inline> ParseMarkdownInlines(const std::wstring& text, const MarkdownContext& ctx,
                                         const std::map<std::wstring, std::wstring>* refs = nullptr);
std::vector<Inline> HighlightCode(const std::wstring& code, const std::wstring& lang);
std::wstring Slugify(const std::wstring& text);
