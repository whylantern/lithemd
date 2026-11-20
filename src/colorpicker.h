#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include "common.h"
#include "view.h"

// Small custom color editor. It exposes RGB(A) sliders with 0-255 edit
// fields, a hex field, and two persistent custom swatches. The callback is
// responsible for saving the chosen slot into Settings and refreshing views.
class ColorPickerWindow {
public:
    using ApplyFn = std::function<void(int slot, Color color)>;

    ColorPickerWindow(HWND owner, Settings* settings, int slot, Color initial, ApplyFn onApply);
    ~ColorPickerWindow();

    bool Create();
    HWND hwnd() const { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void Paint();
    void LayoutEdits();
    void SyncEdits();
    void SetChannel(int channel, int value);
    void ParseHexEdit();
    void Apply();
    void Close();
    Color Current() const;

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Settings* settings_ = nullptr;
    int slot_ = 0;
    int r_ = 0, g_ = 0, b_ = 0, a_ = 255;
    bool updating_ = false;
    int drag_ = -1;
    ApplyFn onApply_;
    HWND edits_[5]{}; // R,G,B,A,hex
    std::unique_ptr<class Canvas> canvas_;

    DRect previewRect_;
    DRect slotRects_[2];
    DRect sliderRects_[4];
    DRect applyRect_;
    DRect cancelRect_;
};
