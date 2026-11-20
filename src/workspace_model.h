// Lazy folder workspace model. No HWND or drawing belongs in this layer.
#pragma once
#include <string>
#include <vector>
#include <memory>

struct WorkspaceRow {
    std::wstring name;
    std::wstring path;
    bool isDir = false;
    bool expanded = false;
    bool error = false;
    int depth = 0;
};

class WorkspaceModel {
public:
    bool SetRoot(const std::wstring& path, std::wstring* error = nullptr);
    void Clear();
    bool Valid() const { return !root_.empty(); }
    const std::wstring& Root() const { return root_; }
    std::wstring RootName() const;
    std::vector<WorkspaceRow> Rows() const;
    bool Toggle(const std::wstring& path);
    void Refresh();
    bool Reveal(const std::wstring& file);
    // True when `path` is the root or lies below it.
    bool Contains(const std::wstring& path) const;

private:
    struct Node {
        std::wstring name;
        std::wstring path;
        bool isDir = false;
        bool expanded = false;
        bool loaded = false;
        bool error = false;
        std::vector<std::unique_ptr<Node>> children;
    };

    bool LoadChildren(Node& node);
    Node* Find(Node& node, const std::wstring& path);
    void Collect(const Node& node, int depth, std::vector<WorkspaceRow>& out) const;

    std::wstring root_;
    std::unique_ptr<Node> tree_;
};
