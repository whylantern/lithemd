#include "workspace_scan.h"
#include "common.h"
#include <algorithm>
#include <windows.h>

void WorkspaceScanCache::Reset(uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    generation_ = generation;
    dirs_.clear();
    entries_ = 0;
    budgetExceeded_ = false;
}

bool WorkspaceScanCache::Get(uint64_t generation, const std::wstring& path,
                             std::vector<CachedEntry>& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation != generation_) return false;
    auto it = dirs_.find(ToLower(path));
    if (it == dirs_.end()) return false;
    out = it->second;
    return true;
}

bool WorkspaceScanCache::Put(uint64_t generation, const std::wstring& path,
                             std::vector<CachedEntry> entries) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generation != generation_) return false;
    std::wstring key = ToLower(path);
    auto old = dirs_.find(key);
    if (old != dirs_.end()) entries_ -= old->second.size();
    entries_ += entries.size();
    dirs_[key] = std::move(entries);
    if (entries_ > MaxEntries) budgetExceeded_ = true;
    return true;
}

bool WorkspaceScanCache::Has(uint64_t generation, const std::wstring& path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation == generation_ && dirs_.find(ToLower(path)) != dirs_.end();
}

std::vector<std::wstring> WorkspaceScanCache::NextUnscanned(uint64_t generation,
                                                            size_t maxCount) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::wstring> out;
    if (generation != generation_ || budgetExceeded_ || maxCount == 0) return out;
    for (auto& dir : dirs_) {
        for (auto& entry : dir.second) {
            if (!entry.isDir) continue;
            std::wstring path = PathJoin(dir.first, entry.name);
            if (dirs_.find(ToLower(path)) != dirs_.end()) continue;
            if (std::find(out.begin(), out.end(), path) == out.end())
                out.push_back(std::move(path));
            if (out.size() >= maxCount) return out;
        }
    }
    return out;
}

size_t WorkspaceScanCache::EntryCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

bool WorkspaceScanCache::BudgetExceeded() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return budgetExceeded_;
}

uint64_t WorkspaceScanCache::Generation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return generation_;
}

WorkspaceScanner::WorkspaceScanner() {
    worker_ = std::thread(&WorkspaceScanner::Worker, this);
}

WorkspaceScanner::~WorkspaceScanner() {
    Stop();
}

void WorkspaceScanner::Begin(uint64_t generation, const std::wstring& root, int depth) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        generation_.store(generation);
        cache_.Reset(generation);
        queue_.clear();
        Job job;
        job.generation = generation;
        job.path = root;
        job.depth = depth;
        job.userPriority = false;
        queue_.push_back(std::move(job));
    }
    busy_.store(true);
    queueCv_.notify_one();
}

void WorkspaceScanner::Request(uint64_t generation, const std::wstring& path, int depth,
                               bool userPriority) {
    if (generation != generation_.load()) return;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (generation != generation_.load()) return;
        if (cache_.Has(generation, path) && !userPriority) return;
        Job job;
        job.generation = generation;
        job.path = path;
        job.depth = depth;
        job.userPriority = userPriority;
        if (userPriority) queue_.push_front(std::move(job));
        else queue_.push_back(std::move(job));
    }
    busy_.store(true);
    queueCv_.notify_one();
}

bool WorkspaceScanner::ContinueIdle(uint64_t generation) {
    if (generation != generation_.load() || busy_.load()) return false;
    std::vector<std::wstring> pending = cache_.NextUnscanned(generation, 32);
    if (pending.empty()) return false;
    for (auto& path : pending) {
        Job job;
        job.generation = generation;
        job.path = std::move(path);
        job.depth = 3;
        job.userPriority = false;
        Enqueue(std::move(job));
    }
    return true;
}

void WorkspaceScanner::Cancel() {
    generation_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        queue_.clear();
        busy_.store(false);
    }
    cache_.Reset(generation_.load());
}

void WorkspaceScanner::Stop() {
    stop_.store(true);
    queueCv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void WorkspaceScanner::Wake() {
    busy_.store(true);
    queueCv_.notify_one();
}

void WorkspaceScanner::Worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [&] { return stop_.load() || !queue_.empty(); });
            if (stop_.load()) break;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        if (job.generation != generation_.load()) continue;
        ScanDirectory(job);
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (queue_.empty()) busy_.store(false);
        }
    }
    busy_.store(false);
}

bool WorkspaceScanner::ScanDirectory(const Job& job) {
    if (job.generation != generation_.load() || stop_.load()) return false;
    std::wstring pattern = PathJoin(job.path, L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                   FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    std::vector<CachedEntry> entries;
    if (find != INVALID_HANDLE_VALUE) {
        size_t checked = 0;
        do {
            if ((++checked & 255) == 0 &&
                (stop_.load() || job.generation != generation_.load()))
                break;
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            CachedEntry entry;
            entry.name = std::move(name);
            entry.isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (entry.isDir && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                entry.isDir = false;
            entries.push_back(std::move(entry));
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    if (stop_.load() || job.generation != generation_.load()) return false;
    cache_.Put(job.generation, job.path, std::move(entries));
    if (job.depth <= 1 || cache_.BudgetExceeded() && !job.userPriority) return true;
    // Enqueue child directories one level at a time. The cache is complete for
    // this directory, so the UI can display it while deeper levels load.
    std::vector<CachedEntry> children;
    if (!cache_.Get(job.generation, job.path, children)) return true;
    for (auto& child : children) {
        if (!child.isDir) continue;
        std::wstring childPath = PathJoin(job.path, child.name);
        if (cache_.Has(job.generation, childPath)) continue;
        Job next;
        next.generation = job.generation;
        next.path = std::move(childPath);
        next.depth = job.depth - 1;
        next.userPriority = job.userPriority;
        Enqueue(std::move(next));
    }
    return true;
}

void WorkspaceScanner::Enqueue(Job job) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (job.generation != generation_.load()) return;
        queue_.push_back(std::move(job));
    }
    busy_.store(true);
    queueCv_.notify_one();
}
