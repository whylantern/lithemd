#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>
#include "common.h"
#include "docmodel.h"
#include "render.h"
#include "view.h"

class DocView;

// Bitmap export dialog: preview on the left, output resolution and actions on
// the right. Rendering is done off-screen through Canvas::BeginBitmap, so the
// exported image contains the document and ink, but no toolbars or handles.
class ExportWindow {
public:
    ExportWindow(DocView* view, Settings* settings);
    ~ExportWindow();

    bool Create(HWND owner);
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void Paint();
    void LayoutEdit();
    void SchedulePreview();
    void UpdatePreview();
    void Export();
    void Close();

    DocView* view_ = nullptr;
    Settings* settings_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND widthEdit_ = nullptr;
    std::unique_ptr<Canvas> canvas_;
    std::vector<uint8_t> previewPng_;
    std::shared_ptr<ImageRef> previewImage_;
    int viewW_ = 0;
    int viewH_ = 0;
    int outWidth_ = 0;
    int outHeight_ = 0;
    int previewSeq_ = 0;
    bool updating_ = false;
    bool hasPreview_ = false;
    bool previewTooLarge_ = false;
    bool formatPdf_ = false;
    bool a4Pages_ = true;
    int dpi_ = 150;
    UINT_PTR timer_ = 0;
    std::wstring notice_;

    DRect previewRect_;
    DRect presetRects_[4];
    DRect formatPngRect_;
    DRect formatPdfRect_;
    DRect layoutA4Rect_;
    DRect layoutLongRect_;
    DRect dpi150Rect_;
    DRect dpi300Rect_;
    DRect exportRect_;
    DRect cancelRect_;
};
