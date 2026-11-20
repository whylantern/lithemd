// Directory change watcher (ReadDirectoryChangesW) with a Win32 message sink.
#pragma once
#include <windows.h>
#include <string>

class FileWatcher {
public:
    FileWatcher() = default;
    ~FileWatcher();
    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    bool Start(HWND notify, UINT message, const std::wstring& directory);
    void Stop();
    bool Active() const { return thread_ != nullptr; }
    const std::wstring& Directory() const { return directory_; }

private:
    HWND notify_ = nullptr;
    UINT message_ = 0;
    std::wstring directory_;
    HANDLE thread_ = nullptr;
    HANDLE stop_ = nullptr;
};
