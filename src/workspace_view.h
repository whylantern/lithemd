// Left-side workspace tree view (Win32/Canvas UI layer).
#pragma once
#include "common.h"
#include "view.h"
#include "workspace_model.h"
#include <functional>
#include <string>

class WorkspaceView {
public:
    static void Register(HINSTANCE inst);
    bool Create(HWND parent, int id);
    HWND hwnd() const { return hwnd_; }
    void ApplyTheme(const std::wstring& theme);
    bool OpenRoot(const std::wstring& path, std::wstring* error = nullptr);
    void Refresh();
    void SetActivePath(const std::wstring& path, bool reveal);
    void SetCallbacks(std::function<void(const std::wstring&)> openFile,
                      std::function<void()> closeView);
    WorkspaceModel& Model() { return model_; }
    bool HasRoot() const { return model_.Valid(); }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);
    void Paint();
    void UpdateScroll();
    DRect RowRect(int index) const;
    int HitRow(float x, float y) const;
    float ContentHeight() const;

    HWND hwnd_ = nullptr;
    WorkspaceModel model_;
    std::wstring theme_ = L"blue";
    std::wstring activePath_;
    std::vector<WorkspaceRow> rows_;
    std::function<void(const std::wstring&)> openFile_;
    std::function<void()> closeView_;
    int hover_ = -1;
    float scroll_ = 0;
    float contentH_ = 0;
    float viewH_ = 0;
    bool dark_ = false;
};
