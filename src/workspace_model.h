// Lazy folder workspace model. No HWND or drawing belongs in this layer.
#pragma once
#include <string>
#include <vector>
#include <memory>
#include "workspace_scan.h"

struct WorkspaceRow {
    std::wstring name;
    std::wstring path;
    bool isDir = false;
    bool expanded = false;
    bool error = false;
    bool loading = false;
    int depth = 0;
};

class WorkspaceModel {
public:
    WorkspaceModel();
    ~WorkspaceModel();
    WorkspaceModel(const WorkspaceModel&) = delete;
    WorkspaceModel& operator=(const WorkspaceModel&) = delete;

    bool SetRoot(const std::wstring& path, std::wstring* error = nullptr);
    void Clear();
    bool Valid() const { return !root_.empty(); }
    const std::wstring& Root() const { return root_; }
    std::wstring RootName() const;
    std::vector<WorkspaceRow> Rows() const;
    bool Toggle(const std::wstring& path);
    void Refresh();
    bool Reveal(const std::wstring& file);
    // Explicit user action: loads the path chain and expands it. This is the
    // only operation allowed to trigger a synchronous deep path load.
    bool Locate(const std::wstring& file);
    // True when `path` is the root or lies below it.
    bool Contains(const std::wstring& path) const;
    bool ApplyCache();
    bool ContinueIdle();
    bool Scanning() const;
    bool BudgetExceeded() const;
    size_t CachedEntries() const;

private:
    struct Node {
        std::wstring name;
        std::wstring path;
        bool isDir = false;
        bool expanded = false;
        bool loaded = false;
        bool loading = false;
        bool error = false;
        std::vector<std::unique_ptr<Node>> children;
    };

    bool LoadChildren(Node& node);
    void BuildChildren(Node& node, const std::vector<CachedEntry>& entries);
    Node* Find(Node& node, const std::wstring& path);
    void Collect(const Node& node, int depth, std::vector<WorkspaceRow>& out) const;
    bool ApplyNode(Node& node);

    std::wstring root_;
    std::unique_ptr<Node> tree_;
    std::unique_ptr<WorkspaceScanner> scanner_;
    uint64_t scanGeneration_ = 0;
};
