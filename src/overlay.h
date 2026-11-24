#pragma once
#include <windows.h>
#include <functional>
#include <memory>

struct Settings;

// Desktop full-screen ink overlay: one transparent layered surface per
// monitor plus a topmost floating toolbar. All annotations are temporary and
// are discarded when the overlay exits.
class FullscreenInkOverlay {
public:
    struct Callbacks {
        std::function<void()> exit;
        std::function<void()> settings;
        // Opens the shared color picker for a preset index.
        std::function<void(int)> palette;
        // Called after the picker changed the shared palette so the main
        // window can refresh views and persist settings.
        std::function<void()> paletteChanged;
    };

    FullscreenInkOverlay(HWND owner, Settings* settings, Callbacks callbacks);
    ~FullscreenInkOverlay();

    bool Show();
    void Hide();
    bool visible() const;

    HWND toolbarHwnd() const;
    // Re-read colors, sizes and toolbar placement from Settings.
    void ApplySettings();
    // Assign a shared palette preset to the active full-screen color slot.
    void UsePalettePreset(int presetIndex);
    // Suppress global Esc/Ctrl+Z while a settings or color window is open.
    void SetHotkeysEnabled(bool enabled);
    // Switch tools programmatically (used by tests and the toolbar).
    void SetTool(int tool);

    // Opaque implementation type. Public only so the translation unit can
    // define its static window procs and hook state.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};