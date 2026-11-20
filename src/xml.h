// Tiny XML DOM used for OOXML (docx/xlsx). Namespaces are preserved in names,
// but lookups compare local names, which is all the readers need.
#pragma once
#include <string>
#include <vector>
#include <memory>

struct XmlNode {
    std::wstring name;
    std::vector<std::pair<std::wstring, std::wstring>> attrs;
    std::vector<std::unique_ptr<XmlNode>> children;
    std::wstring text;

    std::wstring LocalName() const;
    bool Is(const std::wstring& local) const;
    const XmlNode* Child(const std::wstring& local) const;
    std::vector<const XmlNode*> Children(const std::wstring& local) const;
    std::wstring Attr(const std::wstring& name) const;
    std::wstring AttrLocal(const std::wstring& local) const;
    std::wstring AllText() const;
};

bool ParseXml(const std::wstring& text, XmlNode& root, std::wstring* error = nullptr);
