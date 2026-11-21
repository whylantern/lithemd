// Background directory cache and scanner for the workspace tree.
// Core layer: no HWND, no drawing, no UI text policy.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct CachedEntry {
    std::wstring name;
    bool isDir = false;
};

class WorkspaceScanCache {
public:
    void Reset(uint64_t generation);
    bool Get(uint64_t generation, const std::wstring& path, std::vector<CachedEntry>& out) const;
    bool Put(uint64_t generation, const std::wstring& path, std::vector<CachedEntry> entries);
    bool Has(uint64_t generation, const std::wstring& path) const;
    std::vector<std::wstring> NextUnscanned(uint64_t generation, size_t maxCount) const;
    size_t EntryCount() const;
    bool BudgetExceeded() const;
    uint64_t Generation() const;

private:
    mutable std::mutex mutex_;
    uint64_t generation_ = 0;
    std::map<std::wstring, std::vector<CachedEntry>> dirs_;
    size_t entries_ = 0;
    bool budgetExceeded_ = false;
    static constexpr size_t MaxEntries = 200000;
};

class WorkspaceScanner {
public:
    WorkspaceScanner();
    ~WorkspaceScanner();
    WorkspaceScanner(const WorkspaceScanner&) = delete;
    WorkspaceScanner& operator=(const WorkspaceScanner&) = delete;

    void Begin(uint64_t generation, const std::wstring& root, int depth);
    void Request(uint64_t generation, const std::wstring& path, int depth, bool userPriority);
    bool ContinueIdle(uint64_t generation);
    void Cancel();
    void Stop();
    bool Busy() const { return busy_.load(); }
    WorkspaceScanCache& Cache() { return cache_; }
    const WorkspaceScanCache& Cache() const { return cache_; }
    uint64_t Generation() const { return generation_.load(); }

private:
    struct Job {
        uint64_t generation = 0;
        std::wstring path;
        int depth = 0;
        bool userPriority = false;
    };

    void Worker();
    void Enqueue(Job job);
    bool ScanDirectory(const Job& job);
    void Wake();

    WorkspaceScanCache cache_;
    std::atomic<uint64_t> generation_{ 0 };
    std::atomic<bool> busy_{ false };
    std::atomic<bool> stop_{ false };
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<Job> queue_;
    std::thread worker_;
};
