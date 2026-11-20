// Small native dialogs shared by the main window, workspace and archive UI.
#pragma once
#include <windows.h>
#include <string>

// Single-line text input. Returns an empty string when cancelled. The caller
// must check `ok` to distinguish an empty result from cancellation.
std::wstring ShowTextInput(HWND owner, const std::wstring& title, const std::wstring& prompt,
                           const std::wstring& initial, bool password, bool dark, bool* ok = nullptr);

// Folder picker backed by IFileOpenDialog. Empty string means cancelled.
std::wstring PickFolder(HWND owner, const std::wstring& title);
