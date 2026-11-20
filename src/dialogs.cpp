#include "dialogs.h"
#include "common.h"
#include "i18n.h"
#include "render.h"
#include <commdlg.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr int kInputOk = 1;
constexpr int kInputCancel = 2;

class InputDialog {
public:
    InputDialog(std::wstring title, std::wstring prompt, std::wstring initial, bool password, bool dark)
        : title_(std::move(title)), prompt_(std::move(prompt)), initial_(std::move(initial)),
          password_(password), dark_(dark) {}

    std::wstring Run(HWND owner) {
        owner_ = owner;
        HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"LitheMdInputDialog";
        RegisterClassExW(&wc);
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        UINT dpi = owner ? GetDpiForWindow(owner) : 96;
        if (!dpi) dpi = 96;
        float sc = dpi / 96.0f;
        w_ = (int)std::lround(470 * sc);
        h_ = (int)std::lround(196 * sc);
        RECT rc{ 0, 0, w_, h_ };
        AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi);
        int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
        RECT orc{};
        if (owner) GetWindowRect(owner, &orc);
        else { orc.left = 0; orc.top = 0; orc.right = 1200; orc.bottom = 800; }
        int x = orc.left + ((orc.right - orc.left) - ww) / 2;
        int y = orc.top + ((orc.bottom - orc.top) - wh) / 2;
        hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, L"LitheMdInputDialog", title_.c_str(),
                                style, x, y, ww, wh, owner, nullptr, inst, this);
        if (!hwnd_) return L"";
        if (dark_) {
            BOOL value = TRUE;
            DwmSetWindowAttribute(hwnd_, 20, &value, sizeof(value));
            DwmSetWindowAttribute(hwnd_, 19, &value, sizeof(value));
            ApplyDarkWindowTheme(hwnd_, true);
        }
        CreateChildren(inst, dpi);
        if (owner_) EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        SetFocus(edit_);
        SendMessageW(edit_, EM_SETSEL, 0, -1);
        MSG msg{};
        while (IsWindow(hwnd_) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
                cancelled_ = true;
                DestroyWindow(hwnd_);
                break;
            }
            if (!IsDialogMessageW(hwnd_, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        if (owner_) { EnableWindow(owner_, TRUE); SetForegroundWindow(owner_); }
        return cancelled_ ? L"" : value_;
    }

    bool Accepted() const { return !cancelled_; }

private:
    void CreateChildren(HINSTANCE inst, UINT dpi) {
        float sc = dpi / 96.0f;
        bgBrush_ = CreateSolidBrush(dark_ ? RGB(13, 17, 23) : RGB(255, 255, 255));
        editFont_ = CreateFontW(-(int)std::lround(16 * sc), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        DWORD ss = WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX;
        HWND prompt = CreateWindowExW(0, L"STATIC", prompt_.c_str(), ss,
                                      (int)std::lround(24 * sc), (int)std::lround(22 * sc),
                                      w_ - (int)std::lround(48 * sc), (int)std::lround(42 * sc),
                                      hwnd_, (HMENU)(INT_PTR)101, inst, nullptr);
        if (editFont_) SendMessageW(prompt, WM_SETFONT, (WPARAM)editFont_, TRUE);
        int editY = (int)std::lround(76 * sc);
        int editH = (int)std::lround(30 * sc);
        DWORD es = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
        if (password_) es |= ES_PASSWORD;
        edit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial_.c_str(), es,
                                (int)std::lround(24 * sc), editY, w_ - (int)std::lround(48 * sc), editH,
                                hwnd_, (HMENU)(INT_PTR)100, inst, nullptr);
        if (editFont_) SendMessageW(edit_, WM_SETFONT, (WPARAM)editFont_, TRUE);
        int by = (int)std::lround(128 * sc);
        int bw = (int)std::lround(96 * sc);
        int bh = (int)std::lround(34 * sc);
        std::wstring okText = TranslateUi(L"确定");
        std::wstring cancelText = TranslateUi(L"取消");
        HWND ok = CreateWindowExW(0, L"BUTTON", okText.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                  w_ - bw * 2 - (int)std::lround(36 * sc), by, bw, bh, hwnd_,
                                  (HMENU)(INT_PTR)kInputOk, inst, nullptr);
        HWND cancel = CreateWindowExW(0, L"BUTTON", cancelText.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                      w_ - bw - (int)std::lround(24 * sc), by, bw, bh, hwnd_,
                                      (HMENU)(INT_PTR)kInputCancel, inst, nullptr);
        if (editFont_) {
            SendMessageW(ok, WM_SETFONT, (WPARAM)editFont_, TRUE);
            SendMessageW(cancel, WM_SETFONT, (WPARAM)editFont_, TRUE);
        }
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        InputDialog* self = (InputDialog*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (msg == WM_NCCREATE) {
            auto* cs = (CREATESTRUCTW*)lp;
            self = (InputDialog*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
            self->hwnd_ = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        switch (msg) {
        case WM_COMMAND:
            if (LOWORD(wp) == kInputOk) {
                int len = GetWindowTextLengthW(self->edit_);
                std::wstring value((size_t)len + 1, L'\0');
                GetWindowTextW(self->edit_, &value[0], len + 1);
                value.resize((size_t)len);
                self->value_ = Trim(value);
                self->cancelled_ = false;
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wp) == kInputCancel) {
                self->cancelled_ = true;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CLOSE:
            self->cancelled_ = true;
            DestroyWindow(hwnd);
            return 0;
        case WM_ERASEBKGND: {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect((HDC)wp, &rc, self->bgBrush_ ? self->bgBrush_ : (HBRUSH)(COLOR_WINDOW + 1));
            return 1;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORBTN:
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            SetBkMode(dc, TRANSPARENT);
            if (self->dark_) {
                SetTextColor(dc, RGB(230, 230, 230));
                SetBkColor(dc, RGB(13, 17, 23));
                return (LRESULT)self->bgBrush_;
            }
            SetTextColor(dc, RGB(31, 35, 40));
            SetBkColor(dc, RGB(255, 255, 255));
            return (LRESULT)self->bgBrush_;
        }
        case WM_DESTROY:
            if (self->editFont_) { DeleteObject(self->editFont_); self->editFont_ = nullptr; }
            if (self->bgBrush_) { DeleteObject(self->bgBrush_); self->bgBrush_ = nullptr; }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    HWND owner_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH bgBrush_ = nullptr;
    std::wstring title_;
    std::wstring prompt_;
    std::wstring initial_;
    std::wstring value_;
    bool password_ = false;
    bool dark_ = false;
    bool cancelled_ = true;
    int w_ = 470;
    int h_ = 196;
};

} // namespace

std::wstring ShowTextInput(HWND owner, const std::wstring& title, const std::wstring& prompt,
                           const std::wstring& initial, bool password, bool dark, bool* ok) {
    InputDialog dialog(title, prompt, initial, password, dark);
    std::wstring value = dialog.Run(owner);
    if (ok) *ok = dialog.Accepted();
    return value;
}

std::wstring PickFolder(HWND owner, const std::wstring& title) {
    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IFileOpenDialog, reinterpret_cast<void**>(&dialog));
    if (FAILED(hr) || !dialog) return L"";
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(title.c_str());
    std::wstring result;
    hr = dialog->Show(owner);
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}
