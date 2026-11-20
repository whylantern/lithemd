// ZIP browser / extraction window. UI layer on top of ArchiveReader.
#pragma once
#include "common.h"
#include "view.h"
#include "archive_reader.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

class ArchiveWindow {
public:
    ArchiveWindow(Settings* settings, std::wstring path,
                  std::function<void(const std::wstring&, const std::wstring&)> previewFile);
    ~ArchiveWindow();

    bool Create(HWND owner);
    HWND hwnd() const { return hwnd_; }
    void Activate();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);
    void Paint();
    void LayoutButtons();
    void UpdateScroll();
    int HitRow(float x, float y) const;
    DRect RowRect(int index) const;
    void PreviewSelected();
    void ExtractSelected();
    void ExtractAll();
    bool EnsurePassword();
    bool WritePreview(const ArchiveEntry& entry, std::wstring& tempPath, std::wstring& error);
    void StartExtraction(const std::wstring& dest, bool overwrite);
    void FinishExtraction(bool ok, const std::wstring& error);
    std::wstring SelectedName() const;

    struct Button {
        int id = 0;
        std::wstring text;
        DRect rect;
        bool enabled = true;
        bool primary = false;
    };

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Settings* settings_ = nullptr;
    std::shared_ptr<ArchiveReader> reader_;
    std::wstring path_;
    std::wstring password_;
    std::wstring notice_;
    std::function<void(const std::wstring&, const std::wstring&)> previewFile_;
    std::vector<Button> buttons_;
    std::vector<int> visibleRows_;
    int selected_ = -1;
    int hover_ = -1;
    int hoverButton_ = 0;
    float scroll_ = 0;
    float contentH_ = 0;
    float viewH_ = 0;
    bool extracting_ = false;
    bool selfDelete_ = false;
    int progress_ = 0;
    int progressTotal_ = 0;
    uint64_t extractStart_ = 0;
};
