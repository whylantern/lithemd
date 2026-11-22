#pragma once
#include <windows.h>
#include <functional>
#include <memory>
#include <string>
#include "common.h"
#include "view.h"

// Pen color window: up to 12 saved presets on the left, HSV palette and
// transparency on the right, plus exact RGB/hex input. Presets can be pinned
// to the five main-toolbar slots.
class ColorPickerWindow {
public:
    using ChangedFn = std::function<void()>;
    using AppliedFn = std::function<void(int)>;

    ColorPickerWindow(HWND owner, Settings* settings, int presetIndex, Color initial,
                      ChangedFn onChanged, AppliedFn onApplied = {});
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
    void SetFromHsv();
    void UpdateHsvFromRgb();
    void SelectPreset(int index);
    void NewPreset();
    void ResetPresets();
    void ReplacePreset(int index);
    void ToggleToolbarPreset(int index);
    void Apply();
    void Close();
    Color Current() const;
    bool ToolbarContains(int index) const;
    int ToolbarCount() const;

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Settings* settings_ = nullptr;
    int presetIndex_ = 0;
    bool creatingNew_ = false;
    int hue_ = 0;          // 0-359
    float sat_ = 1.0f;     // 0-1
    float val_ = 1.0f;     // 0-1
    int r_ = 0, g_ = 0, b_ = 0, a_ = 255;
    bool updating_ = false;
    int drag_ = -1;        // 0 saturation/value, 1 hue, 2 alpha
    ChangedFn onChanged_;
    AppliedFn onApplied_;
    HWND edits_[5]{};      // R,G,B,A,hex
    std::unique_ptr<class Canvas> canvas_;

    DRect previewRect_;
    DRect svRect_;
    DRect hueRect_;
    DRect alphaRect_;
    DRect presetRects_[12];
    DRect pinRects_[12];
    DRect newRect_;
    DRect resetRect_;
    DRect applyRect_;
    DRect cancelRect_;
};
