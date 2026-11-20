#pragma once
#include "docmodel.h"
#include "markdown.h"
#include <memory>

std::unique_ptr<Document> LoadDocument(const std::wstring& path, std::wstring& error,
                                       TextEncoding encoding = TextEncoding::Auto,
                                       DocLanguage language = DocLanguage::Auto,
                                       bool repairMojibake = false);
void AppendMarkdownToDocument(Document& doc, const std::wstring& text, const MarkdownContext& ctx);
