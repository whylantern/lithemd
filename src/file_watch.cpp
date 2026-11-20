#include "file_watch.h"
#include "common.h"
#include <memory>
#include <vector>

namespace {

struct WatchContext {
    HWND notify = nullptr;
    UINT message = 0;
    std::wstring directory;
    HANDLE stop = nullptr;
};

DWORD WINAPI WatchThread(LPVOID param) {
    std::unique_ptr<WatchContext> ctx((WatchContext*)param);
    HANDLE dir = CreateFileW(ctx->directory.c_str(), FILE_LIST_DIRECTORY,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (dir == INVALID_HANDLE_VALUE) return 0;
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) { CloseHandle(dir); return 0; }
    std::vector<uint8_t> buffer(64 * 1024);
    OVERLAPPED ov{};
    ov.hEvent = event;
    for (;;) {
        ResetEvent(event);
        DWORD returned = 0;
        BOOL ok = ReadDirectoryChangesW(dir, buffer.data(), (DWORD)buffer.size(), FALSE,
                                        FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE |
                                        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_CREATION,
                                        &returned, &ov, nullptr);
        if (!ok && GetLastError() != ERROR_IO_PENDING) break;
        HANDLE waits[2] = { ctx->stop, event };
        DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) {
            CancelIoEx(dir, &ov);
            break;
        }
        if (wait != WAIT_OBJECT_0 + 1) break;
        DWORD transferred = 0;
        if (!GetOverlappedResult(dir, &ov, &transferred, FALSE)) break;
        if (IsWindow(ctx->notify)) PostMessageW(ctx->notify, ctx->message, 0, 0);
        // Coalesce editor save bursts (write temp -> rename -> touch).
        if (WaitForSingleObject(ctx->stop, 180) == WAIT_OBJECT_0) break;
    }
    CloseHandle(event);
    CloseHandle(dir);
    return 0;
}

} // namespace

FileWatcher::~FileWatcher() {
    Stop();
}

bool FileWatcher::Start(HWND notify, UINT message, const std::wstring& directory) {
    if (thread_ && _wcsicmp(directory_.c_str(), directory.c_str()) == 0) {
        notify_ = notify;
        message_ = message;
        return true;
    }
    Stop();
    if (directory.empty()) return false;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop) return false;
    auto* ctx = new WatchContext{ notify, message, directory, stop };
    HANDLE thread = CreateThread(nullptr, 0, WatchThread, ctx, 0, nullptr);
    if (!thread) {
        delete ctx;
        CloseHandle(stop);
        return false;
    }
    thread_ = thread;
    stop_ = stop;
    notify_ = notify;
    message_ = message;
    directory_ = directory;
    return true;
}

void FileWatcher::Stop() {
    if (stop_) SetEvent(stop_);
    if (thread_) {
        WaitForSingleObject(thread_, 3000);
        CloseHandle(thread_);
    }
    if (stop_) CloseHandle(stop_);
    thread_ = nullptr;
    stop_ = nullptr;
    notify_ = nullptr;
    message_ = 0;
    directory_.clear();
}
