#include "workspace_model.h"
#include "common.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>

namespace {

constexpr size_t kMaxChildren = 4000;

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

bool IsInside(const std::wstring& root, const std::wstring& path) {
    if (root.empty() || path.empty()) return false;
    if (path.size() <= root.size()) return SamePath(root, path);
    if (_wcsnicmp(root.c_str(), path.c_str(), root.size()) != 0) return false;
    wchar_t sep = path[root.size()];
    if (sep != L'\\' && sep != L'/') return false;
    // Reject `root\..` style escapes.
    std::wstring rest = path.substr(root.size());
    std::wstring part;
    for (wchar_t c : rest) {
        if (c == L'\\' || c == L'/') {
            if (part == L"..") return false;
            part.clear();
        } else {
            part += c;
        }
    }
    return part != L"..";
}

struct FindData {
    std::wstring name;
    std::wstring path;
    bool isDir = false;
};

} // namespace

bool WorkspaceModel::SetRoot(const std::wstring& path, std::wstring* error) {
    Clear();
    std::wstring full = AbsolutePath(path);
    if (!DirExists(full)) {
        if (error) *error = L"文件夹不存在或无法访问。";
        return false;
    }
    auto node = std::make_unique<Node>();
    node->name = PathName(full);
    if (node->name.empty()) node->name = full;
    node->path = full;
    node->isDir = true;
    node->expanded = true;
    root_ = full;
    tree_ = std::move(node);
    if (!LoadChildren(*tree_)) {
        if (error) *error = L"无法读取文件夹内容。";
        return false;
    }
    return true;
}

void WorkspaceModel::Clear() {
    root_.clear();
    tree_.reset();
}

std::wstring WorkspaceModel::RootName() const {
    if (!tree_) return L"";
    return tree_->name;
}

bool WorkspaceModel::Contains(const std::wstring& path) const {
    return IsInside(root_, AbsolutePath(path));
}

bool WorkspaceModel::LoadChildren(Node& node) {
    node.loaded = true;
    node.children.clear();
    node.error = false;
    std::wstring pattern = PathJoin(node.path, L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                   FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        node.error = true;
        return false;
    }
    std::vector<FindData> dirs, files;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        FindData item;
        item.name = name;
        item.path = PathJoin(node.path, name);
        item.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        // Reparse points can loop or point outside the workspace; show them as
        // files rather than recursing into them.
        if (item.isDir && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            item.isDir = false;
        (item.isDir ? dirs : files).push_back(std::move(item));
        if (dirs.size() + files.size() >= kMaxChildren) break;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    auto byName = [](const FindData& a, const FindData& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    for (auto& d : dirs) {
        auto child = std::make_unique<Node>();
        child->name = d.name;
        child->path = d.path;
        child->isDir = true;
        node.children.push_back(std::move(child));
    }
    for (auto& f : files) {
        auto child = std::make_unique<Node>();
        child->name = f.name;
        child->path = f.path;
        node.children.push_back(std::move(child));
    }
    return true;
}

WorkspaceModel::Node* WorkspaceModel::Find(Node& node, const std::wstring& path) {
    if (SamePath(node.path, path)) return &node;
    if (!IsInside(node.path, path)) return nullptr;
    for (auto& child : node.children) {
        if (!child->isDir) continue;
        if (SamePath(child->path, path)) return child.get();
        if (IsInside(child->path, path)) {
            if (!child->loaded) LoadChildren(*child);
            return Find(*child, path);
        }
    }
    return nullptr;
}

bool WorkspaceModel::Toggle(const std::wstring& path) {
    if (!tree_) return false;
    Node* node = Find(*tree_, AbsolutePath(path));
    if (!node || !node->isDir) return false;
    if (!node->expanded && !node->loaded) LoadChildren(*node);
    node->expanded = !node->expanded;
    return true;
}

void WorkspaceModel::Refresh() {
    if (!root_.empty()) SetRoot(root_);
}

bool WorkspaceModel::Reveal(const std::wstring& file) {
    if (!tree_) return false;
    std::wstring full = AbsolutePath(file);
    if (!IsInside(root_, full)) return false;
    Node* node = &*tree_;
    size_t pos = root_.size();
    while (pos < full.size()) {
        while (pos < full.size() && (full[pos] == L'\\' || full[pos] == L'/')) pos++;
        if (pos >= full.size()) break;
        size_t next = full.find_first_of(L"\\/", pos);
        if (next == std::wstring::npos) next = full.size();
        std::wstring part = full.substr(pos, next - pos);
        if (part.empty()) break;
        if (node->isDir && !node->loaded) LoadChildren(*node);
        Node* found = nullptr;
        for (auto& child : node->children) {
            if (_wcsicmp(child->name.c_str(), part.c_str()) == 0) { found = child.get(); break; }
        }
        if (!found) return false;
        if (found->isDir) found->expanded = true;
        node = found;
        pos = next;
    }
    return true;
}

void WorkspaceModel::Collect(const Node& node, int depth, std::vector<WorkspaceRow>& out) const {
    for (auto& child : node.children) {
        WorkspaceRow row;
        row.name = child->name;
        row.path = child->path;
        row.isDir = child->isDir;
        row.expanded = child->expanded;
        row.error = child->error;
        row.depth = depth;
        out.push_back(row);
        if (child->isDir && child->expanded) Collect(*child, depth + 1, out);
    }
}

std::vector<WorkspaceRow> WorkspaceModel::Rows() const {
    std::vector<WorkspaceRow> out;
    if (tree_) Collect(*tree_, 0, out);
    return out;
}
