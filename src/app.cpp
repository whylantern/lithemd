#include "app.h"
#include "view.h"
#include "workspace_view.h"
#include "archive_window.h"
#include "render.h"
#include "document.h"
#include "export.h"
#include "colorpicker.h"
#include "i18n.h"
#include "dialogs.h"
#include "path_util.h"
#include "file_watch.h"
#include "file_policy.h"
#include "reading_positions.h"
#include <commdlg.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <set>

namespace {

enum : int {
    CMD_OPEN = 1001,
    CMD_OPEN_MENU,
    CMD_REFRESH_WORKSPACE,
    CMD_LOCATE,
    CMD_RELOAD,
    CMD_PREVIEW,
    CMD_SOURCE,
    CMD_TOC,
    CMD_THEME,
    CMD_SETTINGS,
    CMD_ZOOM_IN,
    CMD_ZOOM_OUT,
    CMD_ZOOM_RESET,
    CMD_JSON_EXPAND,
    CMD_JSON_COLLAPSE,
    CMD_CLOSE_TAB,
    CMD_INK,
    CMD_UNDO_INK,
    CMD_ENCODING,
    CMD_REDO_INK,
    CMD_COPY,
    CMD_SCROLL_UP,
    CMD_SCROLL_DOWN,
    CMD_SCROLL_LEFT,
    CMD_SCROLL_RIGHT,
    CMD_PAGE_UP,
    CMD_PAGE_DOWN,
    CMD_SCROLL_HOME,
    CMD_SCROLL_END,
    CMD_SAVE_NOTES,
    CMD_PASTE_INK,
    CMD_DELETE_INK,
    CMD_GROUP_INK,
    CMD_UNGROUP_INK,
    CMD_HIGHLIGHT_PDF,
    CMD_EXPORT,
    CMD_OPEN_FOLDER,
    CMD_PATH,
    CMD_WORKSPACE,
    CMD_SPLIT,
    CMD_PRESENT,
    CMD_ENC_AUTO = 1200,
    CMD_ENC_UTF8,
    CMD_ENC_UTF16LE,
    CMD_ENC_UTF16BE,
    CMD_ENC_UTF32LE,
    CMD_ENC_UTF32BE,
    CMD_ENC_GB18030,
    CMD_ENC_GBK,
    CMD_ENC_BIG5,
    CMD_ENC_SHIFTJIS,
    CMD_ENC_EUCKR,
    CMD_ENC_WIN1252,
    CMD_ENC_WIN1251,
    CMD_ENC_WIN1250,
    CMD_ENC_LATIN1,
    CMD_ENC_ANSI,
    CMD_ENC_REDETECT,
    CMD_ENC_REPAIR,
    CMD_LANG_AUTO = 1300,
    CMD_LANG_ZH_CN,
    CMD_LANG_ZH_TW,
    CMD_LANG_JA,
    CMD_LANG_KO,
    CMD_LANG_WESTERN,
    CMD_APP_OPEN_SETTINGS = 1100
};

constexpr int kHeaderH = 58;
constexpr int kTabH = 34;
constexpr int kStatusH = 30;
constexpr int kOutlineW = 256;
constexpr UINT kFileChangedMsg = WM_APP + 31;

float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

bool IsTextInputFocus(HWND hwnd) {
    if (!hwnd) return false;
    wchar_t cls[64]{};
    if (!GetClassNameW(hwnd, cls, 64)) return false;
    return _wcsicmp(cls, L"Edit") == 0 || _wcsicmp(cls, L"RichEdit20W") == 0 ||
           _wcsicmp(cls, L"RichEdit50W") == 0;
}

struct ShortcutCommandDef {
    const wchar_t* id;
    const wchar_t* label;
    int cmd;
    bool navigation;
    std::vector<ShortcutKey> defaults;
};

const std::vector<ShortcutCommandDef>& ShortcutCommands() {
    static const std::vector<ShortcutCommandDef> defs = {
        { L"open", L"打开文件", CMD_OPEN, false, { { ShortcutCtrl, 'O' } } },
        { L"openfolder", L"打开文件夹", CMD_OPEN_FOLDER, false, { { ShortcutCtrl | ShortcutShift, 'O' } } },
        { L"path", L"输入路径", CMD_PATH, false, { { ShortcutCtrl, 'L' } } },
        { L"workspace", L"工作区", CMD_WORKSPACE, false, { { ShortcutCtrl | ShortcutShift, 'E' } } },
        { L"split", L"分屏", CMD_SPLIT, false, { { ShortcutCtrl, '\\' } } },
        { L"present", L"演示模式", CMD_PRESENT, false, { { 0, VK_F11 } } },
        { L"reload", L"刷新", CMD_RELOAD, false, { { 0, VK_F5 }, { ShortcutCtrl, 'R' } } },
        { L"closetab", L"关闭标签", CMD_CLOSE_TAB, false, { { ShortcutCtrl, 'W' } } },
        { L"undo", L"撤销画笔", CMD_UNDO_INK, false, { { ShortcutCtrl, 'Z' } } },
        { L"redo", L"重做画笔", CMD_REDO_INK, false, { { ShortcutCtrl, 'Y' } } },
        { L"preview", L"预览", CMD_PREVIEW, false, { { ShortcutCtrl, '1' } } },
        { L"source", L"源码", CMD_SOURCE, false, { { ShortcutCtrl, '2' } } },
        { L"outline", L"大纲", CMD_TOC, false, { { ShortcutCtrl, 'B' } } },
        { L"ink", L"画笔模式", CMD_INK, false, { { ShortcutCtrl | ShortcutShift, 'A' } } },
        { L"theme", L"切换主题", CMD_THEME, false, { { ShortcutCtrl | ShortcutShift, 'D' } } },
        { L"settings", L"设置", CMD_SETTINGS, false, { { ShortcutCtrl, VK_OEM_COMMA } } },
        { L"zoomin", L"放大", CMD_ZOOM_IN, false, { { ShortcutCtrl, VK_OEM_PLUS } } },
        { L"zoomout", L"缩小", CMD_ZOOM_OUT, false, { { ShortcutCtrl, VK_OEM_MINUS } } },
        { L"zoomreset", L"重置缩放", CMD_ZOOM_RESET, false, { { ShortcutCtrl, '0' } } },
        { L"save", L"保存笔记", CMD_SAVE_NOTES, false, { { ShortcutCtrl, 'S' } } },
        { L"inkpaste", L"粘贴笔迹", CMD_PASTE_INK, false, { { ShortcutCtrl, 'V' } } },
        { L"inkgroup", L"组合笔迹", CMD_GROUP_INK, false, { { ShortcutCtrl, 'G' } } },
        { L"inkungroup", L"取消组合", CMD_UNGROUP_INK, false, { { ShortcutCtrl | ShortcutShift, 'G' } } },
        { L"inkdelete", L"删除笔迹", CMD_DELETE_INK, false, { { 0, VK_DELETE } } },
        { L"highlightpdf", L"荧光笔", CMD_HIGHLIGHT_PDF, false, { { ShortcutCtrl | ShortcutShift, 'H' } } },
        { L"export", L"导出图片", CMD_EXPORT, false, { { ShortcutCtrl | ShortcutShift, 'E' } } },
        { L"copy", L"复制", CMD_COPY, true, { { ShortcutCtrl, 'C' } } },
        { L"scrollup", L"向上滚动", CMD_SCROLL_UP, true, { { 0, VK_UP } } },
        { L"scrolldown", L"向下滚动", CMD_SCROLL_DOWN, true, { { 0, VK_DOWN } } },
        { L"scrollleft", L"向左滚动", CMD_SCROLL_LEFT, true, { { 0, VK_LEFT } } },
        { L"scrollright", L"向右滚动", CMD_SCROLL_RIGHT, true, { { 0, VK_RIGHT } } },
        { L"pageup", L"上一页", CMD_PAGE_UP, true, { { 0, VK_PRIOR } } },
        { L"pagedown", L"下一页", CMD_PAGE_DOWN, true, { { 0, VK_NEXT } } },
        { L"home", L"回到顶部", CMD_SCROLL_HOME, true, { { 0, VK_HOME } } },
        { L"end", L"到底部", CMD_SCROLL_END, true, { { 0, VK_END } } },
    };
    return defs;
}

std::vector<ShortcutKey> EffectiveShortcuts(const Settings& settings, const ShortcutCommandDef& def) {
    auto it = settings.shortcuts.find(def.id);
    if (it != settings.shortcuts.end()) return it->second;
    return def.defaults;
}

struct HeaderBtn {
    int cmd = 0;
    DRect rect;
    std::wstring text;
    std::wstring glyph;
    bool iconTop = false;
    bool active = false;
    bool enabled = true;
    bool dropdown = false;
    int priority = 50; // lower number = keep longer when the window is narrow
};

std::wstring FitText(Canvas& c, const std::wstring& text, float size, bool bold, float maxWidth) {
    if (maxWidth <= 8) return L"";
    if (c.MeasureText(text, size, bold, false, 100000.0f, false) <= maxWidth) return text;
    std::wstring t = text;
    while (!t.empty()) {
        t.pop_back();
        std::wstring cand = t + L"…";
        if (c.MeasureText(cand, size, bold, false, 100000.0f, false) <= maxWidth) return cand;
    }
    return L"…";
}

std::wstring BackgroundConfigDirectory(const Settings& settings) {
    std::wstring cfg = settings.configPath.empty() ? Settings::ResolveConfigPath() : settings.configPath;
    return PathDir(cfg);
}

std::wstring NewBackgroundId() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    uint64_t stamp = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    static uint64_t sequence = 0;
    sequence++;
    uint64_t value = stamp ^ ((uint64_t)GetCurrentProcessId() << 32) ^
                     (sequence * 0x9E3779B97F4A7C15ull);
    wchar_t text[32];
    swprintf(text, 32, L"%016llX", (unsigned long long)value);
    return text;
}

std::wstring BackgroundFileNameStem(const std::wstring& path) {
    std::wstring name = PathName(path);
    size_t dot = name.find_last_of(L'.');
    std::wstring stem = dot == std::wstring::npos ? name : name.substr(0, dot);
    for (auto& c : stem) {
        if (c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' ||
            c == L'/' || c == L'\\' || c == L'|' || c == L'?' || c == L'*')
            c = L'_';
    }
    while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.')) stem.pop_back();
    if (stem.size() > 48) stem.resize(48);
    while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.')) stem.pop_back();
    if (stem.empty()) stem = L"background";
    return stem;
}

std::wstring BackgroundFileExtension(const std::wstring& path) {
    std::wstring ext = PathExt(path);
    bool valid = ext.size() >= 2 && ext.size() <= 12;
    for (size_t i = 1; valid && i < ext.size(); i++)
        if (!((ext[i] >= L'a' && ext[i] <= L'z') || (ext[i] >= L'0' && ext[i] <= L'9')))
            valid = false;
    return valid ? ext : L".img";
}

bool IsManagedBackground(const Settings& settings, const std::wstring& path, const std::wstring& id) {
    if (path.empty() || id.empty()) return false;
    std::wstring dir = ToLower(BackgroundConfigDirectory(settings));
    if (dir.empty() || ToLower(PathDir(path)) != dir) return false;
    return ToLower(PathName(path)).find(ToLower(id)) != std::wstring::npos;
}

void DeleteManagedBackground(const Settings& settings, const std::wstring& path, const std::wstring& id) {
    if (IsManagedBackground(settings, path, id)) DeleteFileW(path.c_str());
}

bool CopyBackgroundToConfig(Settings& settings, const std::wstring& source, std::wstring& error) {
    error.clear();
    if (!FileExists(source)) {
        error = L"背景图片文件不存在。";
        return false;
    }
    if (!settings.backgroundImage.empty() && !settings.backgroundImageId.empty() &&
        _wcsicmp(source.c_str(), settings.backgroundImage.c_str()) == 0)
        return true;
    std::wstring dir = BackgroundConfigDirectory(settings);
    if (dir.empty() || !EnsureDir(dir)) {
        error = L"无法创建背景图片目录。";
        return false;
    }
    std::wstring stem = BackgroundFileNameStem(source);
    std::wstring ext = BackgroundFileExtension(source);
    for (int attempt = 0; attempt < 8; attempt++) {
        std::wstring id = NewBackgroundId();
        std::wstring target = PathJoin(dir, stem + L"-" + id + ext);
        if (FileExists(target)) continue;
        std::wstring part = target + L".part";
        DeleteFileW(part.c_str());
        if (!CopyFileW(source.c_str(), part.c_str(), FALSE)) {
            error = L"无法复制背景图片到配置目录。请检查配置目录写入权限。";
            return false;
        }
        if (!MoveFileExW(part.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(part.c_str());
            error = L"无法保存背景图片副本。";
            return false;
        }
        settings.backgroundImage = target;
        settings.backgroundImageId = id;
        return true;
    }
    error = L"无法生成唯一的背景图片 ID。";
    return false;
}

void SetDarkTitle(HWND hwnd, bool dark) {
    BOOL value = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20, &value, sizeof(value));
    DwmSetWindowAttribute(hwnd, 19, &value, sizeof(value));
    ApplyDarkWindowTheme(hwnd, dark);
}

std::wstring SourceTextForError(const std::wstring& path) {
    if (IsBlockedBinaryFile(path)) return L"";
    DocKind k = DetectKind(path, L"");
    if (IsBinaryKind(k)) return L"";
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(path, bytes)) return L"";
    std::wstring enc;
    return NormalizeNewlines(DecodeBytes(bytes, enc));
}

bool SetRegString(HKEY root, const std::wstring& sub, const std::wstring& name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(key, name.empty() ? nullptr : name.c_str(), 0, REG_SZ,
                            (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

bool SetRegEmpty(HKEY root, const std::wstring& sub, const std::wstring& name) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    LONG r = RegSetValueExW(key, name.c_str(), 0, REG_NONE, nullptr, 0);
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

std::vector<std::wstring> AssociationExtensions() {
    return { L".md", L".markdown", L".mdown", L".mkd", L".mdx", L".json", L".jsonc", L".ipynb",
             L".csv", L".tsv", L".txt", L".log", L".yml", L".yaml", L".xml", L".ini", L".toml",
             L".docx", L".doc", L".xlsx", L".xls", L".pdf", L".zip" };
}

bool DeleteRegValue(const std::wstring& sub, const std::wstring& name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    LONG r = RegDeleteValueW(key, name.empty() ? nullptr : name.c_str());
    RegCloseKey(key);
    return r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND;
}

bool RegisterAssociations(const std::wstring& exe, const std::vector<std::wstring>& exts) {
    const std::wstring progId = L"LitheMD.Document";
    const std::wstring appKey = L"Software\\Classes\\Applications\\lithemd.exe";
    std::wstring quoted = L"\"" + exe + L"\" \"%1\"";
    bool ok = true;
    ok &= SetRegString(HKEY_CURRENT_USER, L"Software\\Classes\\" + progId, L"", L"LitheMD 文档");
    ok &= SetRegString(HKEY_CURRENT_USER, L"Software\\Classes\\" + progId, L"FriendlyTypeName", L"LitheMD 文档");
    ok &= SetRegString(HKEY_CURRENT_USER, L"Software\\Classes\\" + progId + L"\\DefaultIcon", L"", exe + L",0");
    ok &= SetRegString(HKEY_CURRENT_USER, L"Software\\Classes\\" + progId + L"\\shell\\open\\command", L"", quoted);
    // Keep existing file associations working after the rename: the old
    // ProgID now launches lithemd.exe too.
    ok &= SetRegString(HKEY_CURRENT_USER, L"Software\\Classes\\openmd.Document", L"FriendlyTypeName", L"LitheMD 文档");
    ok &= SetRegString(HKEY_CURRENT_USER, L"Software\\Classes\\openmd.Document\\DefaultIcon", L"", exe + L",0");
    ok &= SetRegString(HKEY_CURRENT_USER, L"Software\\Classes\\openmd.Document\\shell\\open\\command", L"", quoted);
    DeleteRegValue(L"Software\\RegisteredApplications", L"openmd");
    ok &= SetRegString(HKEY_CURRENT_USER, appKey, L"FriendlyAppName", L"LitheMD");
    ok &= SetRegString(HKEY_CURRENT_USER, appKey + L"\\DefaultIcon", L"", exe + L",0");
    ok &= SetRegString(HKEY_CURRENT_USER, appKey + L"\\shell\\open\\command", L"", quoted);
    // Registering the executable itself makes Windows list LitheMD under
    // "Open with" even before any file-type selection is made.
    ok &= SetRegString(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\lithemd.exe",
                       L"", exe);
    ok &= SetRegString(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\lithemd.exe",
                       L"Path", PathDir(exe));
    ok &= SetRegString(HKEY_CURRENT_USER, L"Software\\LitheMD\\Capabilities", L"ApplicationName", L"LitheMD");
    ok &= SetRegString(HKEY_CURRENT_USER, L"Software\\LitheMD\\Capabilities", L"ApplicationDescription",
                       L"轻量本地文档阅读器（Markdown / JSON / Office / PDF / 图片）");
    ok &= SetRegString(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", L"LitheMD",
                       L"Software\\LitheMD\\Capabilities");
    for (auto& ext : exts) {
        SetRegEmpty(HKEY_CURRENT_USER, L"Software\\Classes\\" + ext + L"\\OpenWithProgids", progId);
        SetRegEmpty(HKEY_CURRENT_USER, appKey + L"\\SupportedTypes", ext);
        HKEY cap = nullptr;
        std::wstring capKey = L"Software\\LitheMD\\Capabilities\\FileAssociations";
        if (RegCreateKeyExW(HKEY_CURRENT_USER, capKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &cap, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(cap, ext.c_str(), 0, REG_SZ, (const BYTE*)progId.c_str(),
                           (DWORD)((progId.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(cap);
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

bool UnregisterAssociations(const std::vector<std::wstring>& exts) {
    const std::wstring progId = L"LitheMD.Document";
    const std::wstring appKey = L"Software\\Classes\\Applications\\lithemd.exe";
    const std::wstring oldProgId = L"openmd.Document";
    const std::wstring oldAppKey = L"Software\\Classes\\Applications\\openmd.exe";
    for (auto& ext : exts) {
        DeleteRegValue(L"Software\\Classes\\" + ext + L"\\OpenWithProgids", progId);
        DeleteRegValue(L"Software\\Classes\\" + ext + L"\\OpenWithProgids", oldProgId);
        DeleteRegValue(appKey + L"\\SupportedTypes", ext);
        DeleteRegValue(oldAppKey + L"\\SupportedTypes", ext);
        DeleteRegValue(L"Software\\LitheMD\\Capabilities\\FileAssociations", ext);
        DeleteRegValue(L"Software\\openmd\\Capabilities\\FileAssociations", ext);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

bool AssociationIsDefault(const std::wstring& ext) {
    std::wstring key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\" + ext + L"\\UserChoice";
    auto matches = [](const wchar_t* value) {
        return _wcsicmp(value, L"LitheMD.Document") == 0 || _wcsicmp(value, L"openmd.Document") == 0;
    };
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS) {
        wchar_t buf[512]{};
        DWORD type = 0, cb = sizeof(buf);
        LONG r = RegQueryValueExW(h, L"ProgId", nullptr, &type, (BYTE*)buf, &cb);
        RegCloseKey(h);
        if (r == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && matches(buf))
            return true;
    }
    key = L"Software\\Classes\\" + ext;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &h) == ERROR_SUCCESS) {
        wchar_t buf[512]{};
        DWORD type = 0, cb = sizeof(buf);
        LONG r = RegQueryValueExW(h, nullptr, nullptr, &type, (BYTE*)buf, &cb);
        RegCloseKey(h);
        if (r == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && matches(buf))
            return true;
    }
    return false;
}

std::wstring AssociationProbeFile(const std::wstring& ext) {
    std::wstring name = L"lithemd-association" + ext;
    std::wstring path = TempPath(name);
    if (!FileExists(path)) {
        std::string empty;
        std::vector<uint8_t> data;
        WriteFileBytes(path, data, nullptr);
    }
    return path;
}

bool LaunchAssociationUI() {
    IApplicationAssociationRegistrationUI* ui = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ApplicationAssociationRegistrationUI, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IApplicationAssociationRegistrationUI,
                                  reinterpret_cast<void**>(&ui));
    if (FAILED(hr) || !ui) return false;
    hr = ui->LaunchAdvancedAssociationUI(L"LitheMD");
    ui->Release();
    return SUCCEEDED(hr);
}

std::vector<std::wstring> SelectedExtensions(const std::set<std::wstring>& selected) {
    std::vector<std::wstring> out;
    for (auto& ext : AssociationExtensions())
        if (selected.count(ext)) out.push_back(ext);
    return out;
}

// ================================================================ Shortcuts
class ShortcutWindow {
public:
    ShortcutWindow(Settings* settings, std::function<void()> changed, std::function<void()> closed)
        : settings_(settings), changed_(std::move(changed)), closed_(std::move(closed)) {}

    bool Create(HWND owner) {
        owner_ = owner;
        HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"LitheMdShortcuts";
        RegisterClassExW(&wc);
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
        UINT dpi = GetDpiForWindow(owner);
        if (!dpi) dpi = 96;
        float sc = dpi / 96.0f;
        RECT rc{ 0, 0, (LONG)std::lround(700 * sc), (LONG)std::lround(680 * sc) };
        AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi);
        RECT orc{};
        GetWindowRect(owner, &orc);
        int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
        int ox = orc.left + ((orc.right - orc.left) - ww) / 2;
        int oy = orc.top + ((orc.bottom - orc.top) - wh) / 2;
        RECT work{};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
            if (ox < work.left + 8) ox = work.left + 8;
            if (oy < work.top + 8) oy = work.top + 8;
            if (ox + ww > work.right - 8) ox = work.right - ww - 8;
            if (oy + wh > work.bottom - 8) oy = work.bottom - wh - 8;
        }
        hwnd_ = CreateWindowExW(0, L"LitheMdShortcuts", TranslateUi(L"快捷键").c_str(), style, ox, oy, ww, wh,
                                owner, nullptr, inst, this);
        if (!hwnd_) return false;
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        SetFocus(hwnd_);
        SetDarkTitle(hwnd_, settings_->dark);
        UpdateWindow(hwnd_);
        return true;
    }

    HWND hwnd() const { return hwnd_; }
    void Activate() {
        if (hwnd_) { ShowWindow(hwnd_, SW_RESTORE); SetForegroundWindow(hwnd_); SetFocus(hwnd_); }
    }
    void ApplyTheme() {
        if (hwnd_) { SetDarkTitle(hwnd_, settings_->dark); InvalidateRect(hwnd_, nullptr, FALSE); }
    }

private:
    struct Hit {
        int index = -1;
        DRect key;
        DRect reset;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        ShortcutWindow* self = (ShortcutWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (msg == WM_NCCREATE) {
            auto* cs = (CREATESTRUCTW*)lp;
            self = (ShortcutWindow*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
            self->hwnd_ = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        return self->Handle(msg, wp, lp);
    }

    const ShortcutCommandDef* CommandAt(int index) const {
        auto& defs = ShortcutCommands();
        if (index < 0 || index >= (int)defs.size()) return nullptr;
        return &defs[(size_t)index];
    }

    std::wstring BindingText(const ShortcutCommandDef& def) const {
        auto keys = EffectiveShortcuts(*settings_, def);
        std::wstring text;
        for (auto& key : keys) {
            if (!text.empty()) text += L", ";
            text += ShortcutToString(key);
        }
        return text.empty() ? L"未绑定" : text;
    }

    bool IsCustom(const ShortcutCommandDef& def) const {
        return settings_->shortcuts.find(def.id) != settings_->shortcuts.end();
    }

    std::wstring ConflictText() const {
        std::vector<std::pair<ShortcutKey, std::wstring>> all;
        for (auto& def : ShortcutCommands()) {
            for (auto& key : EffectiveShortcuts(*settings_, def)) {
                for (auto& existing : all) {
                    if (existing.first == key) {
                        return L"重复绑定：" + existing.second + L" / " + def.label + L"（先到先得）";
                    }
                }
                all.push_back({ key, def.label });
            }
        }
        return L"";
    }

    void Paint() {
        Canvas c;
        if (!c.Begin(hwnd_, Palette::Get(settings_->theme))) return;
        c.SetTranslateUi(true);
        auto& p = c.palette();
        float w = c.widthDip(), h = c.heightDip();
        c.FillRect(0, 0, w, h, p.bg);
        hits_.clear();
        c.DrawText(L"快捷键", 24, 16, 300, 34, 23, p.fg, true);
        c.DrawText(L"点击按键框后直接按组合键录制；Esc 取消，Backspace 清除；重复绑定会被忽略。",
                   24, 48, w - 48, 20, 11.5f, p.muted);
        c.FillRect(24, 78, w - 48, 1, p.border);

        float colW = (w - 72) / 2;
        float xs[2] = { 24, 24 + colW + 24 };
        const wchar_t* titles[2] = { L"应用命令", L"文档导航" };
        for (int col = 0; col < 2; col++) {
            float baseY = 94;
            c.DrawText(titles[col], xs[col], baseY, colW, 22, 13, p.fg, true);
            float rowY = baseY + 26;
            auto& defs = ShortcutCommands();
            for (size_t ci = 0; ci < defs.size(); ci++) {
                auto& def = defs[ci];
                if ((col == 1) != def.navigation) continue;
                int index = (int)ci;
                float keyW = 138, resetW = 22;
                DRect label{ xs[col], rowY, colW - keyW - resetW - 16, 20 };
                DRect key{ xs[col] + colW - keyW - resetW - 4, rowY, keyW, 20 };
                DRect reset{ xs[col] + colW - resetW, rowY, resetW, 20 };
                hits_.push_back({ index, key, reset });
                bool custom = IsCustom(def);
                Color lc = custom ? p.accent : p.fg;
                c.DrawText(def.label, label.x, label.y, label.w, 20, 11.0f, lc, false, false,
                           DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
                c.FillRound(key.x, key.y, key.w, key.h, 5, p.surface);
                c.StrokeRound(key.x, key.y, key.w, key.h, 5,
                              recording_ == index ? p.accent : p.border);
                std::wstring text = recording_ == index ? L"按组合键…" : BindingText(def);
                c.DrawText(FitText(c, text, 10.0f, false, key.w - 12), key.x + 6, key.y, key.w - 12,
                           20, 10.0f, recording_ == index ? p.accent : p.muted, false, false,
                           DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
                c.DrawText(L"↺", reset.x, reset.y, reset.w, 20, 12, p.muted, false, false,
                           DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
                rowY += 22;
            }
        }

        std::wstring notice = notice_;
        if (notice.empty()) notice = ConflictText();
        if (!notice.empty())
            c.DrawText(FitText(c, notice, 11.0f, false, w - 48), 24, h - 84, w - 48, 26, 11.0f,
                       settings_->dark ? HexColor(L"#D29922") : HexColor(L"#9A6700"), false, false,
                       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);

        resetAll_ = { 24, h - 46, 132, 32 };
        close_ = { w - 118, h - 46, 94, 32 };
        c.StrokeRound(resetAll_.x, resetAll_.y, resetAll_.w, resetAll_.h, 7, p.border);
        c.DrawText(L"恢复全部默认", resetAll_.x, resetAll_.y, resetAll_.w, resetAll_.h, 11.5f, p.fg,
                   false, false, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        c.FillRound(close_.x, close_.y, close_.w, close_.h, 7, p.accent);
        c.DrawText(L"关闭", close_.x, close_.y, close_.w, close_.h, 12.0f, HexColor(L"#FFFFFF"), true,
                   false, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        c.End();
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd_, &ps);
            Paint();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_MOUSEMOVE: {
            float scale = GetDpiForWindow(hwnd_) / 96.0f;
            float x = GET_X_LPARAM(lp) / scale, y = GET_Y_LPARAM(lp) / scale;
            int hover = -1;
            for (auto& hit : hits_) {
                if (hit.key.Contains(x, y) || hit.reset.Contains(x, y)) { hover = hit.index; break; }
            }
            if (hover != hover_) {
                hover_ = hover;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            SetCursor(LoadCursorW(nullptr, hover >= 0 ? IDC_HAND : IDC_ARROW));
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = -1;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            float scale = GetDpiForWindow(hwnd_) / 96.0f;
            float x = GET_X_LPARAM(lp) / scale, y = GET_Y_LPARAM(lp) / scale;
            if (close_.Contains(x, y)) { DestroyWindow(hwnd_); return 0; }
            if (resetAll_.Contains(x, y)) {
                settings_->shortcuts.clear();
                settings_->Save();
                if (changed_) changed_();
                recording_ = -1;
                notice_ = L"已恢复全部默认快捷键。";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            for (auto& hit : hits_) {
                if (hit.key.Contains(x, y)) {
                    recording_ = hit.index;
                    notice_.clear();
                    SetFocus(hwnd_);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
                if (hit.reset.Contains(x, y)) {
                    auto* def = CommandAt(hit.index);
                    if (def) {
                        settings_->shortcuts.erase(def->id);
                        settings_->Save();
                        if (changed_) changed_();
                        notice_ = std::wstring(L"已恢复：") + def->label;
                        recording_ = -1;
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                    return 0;
                }
            }
            return 0;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (recording_ < 0) return DefWindowProcW(hwnd_, msg, wp, lp);
            auto* def = CommandAt(recording_);
            if (!def) return 0;
            UINT vk = (UINT)wp;
            if (vk == VK_ESCAPE) {
                recording_ = -1;
                notice_.clear();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
                vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
                vk == VK_LWIN || vk == VK_RWIN || vk == VK_PROCESSKEY) {
                return 0;
            }
            UINT mods = 0;
            if (GetKeyState(VK_CONTROL) & 0x8000) mods |= ShortcutCtrl;
            if (GetKeyState(VK_MENU) & 0x8000) mods |= ShortcutAlt;
            if (GetKeyState(VK_SHIFT) & 0x8000) mods |= ShortcutShift;
            if ((vk == VK_BACK || vk == VK_DELETE) && mods == 0) {
                settings_->shortcuts[def->id] = {};
                settings_->Save();
                if (changed_) changed_();
                recording_ = -1;
                notice_ = std::wstring(L"已清除：") + def->label;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            ShortcutKey key{ mods, vk };
            if (!IsShortcutAllowed(key)) {
                notice_ = L"该组合键不可用，请换一个。";
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            for (auto& other : ShortcutCommands()) {
                if (_wcsicmp(other.id, def->id) == 0) continue;
                for (auto& bound : EffectiveShortcuts(*settings_, other)) {
                    if (bound == key) {
                        notice_ = std::wstring(L"已被“") + other.label + L"”使用，请换一个。";
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        return 0;
                    }
                }
            }
            settings_->shortcuts[def->id] = { key };
            settings_->Save();
            if (changed_) changed_();
            recording_ = -1;
            notice_ = std::wstring(L"已绑定：") + ShortcutToString(key);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        case WM_NCDESTROY:
            if (closed_) closed_();
            break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Settings* settings_ = nullptr;
    std::function<void()> changed_;
    std::function<void()> closed_;
    std::vector<Hit> hits_;
    DRect resetAll_;
    DRect close_;
    int recording_ = -1;
    int hover_ = -1;
    std::wstring notice_;
};

// ================================================================ Settings
class SettingsWindow {
public:
    SettingsWindow(Settings* settings, std::wstring currentFile, std::function<void()> changed,
                   std::function<void()> closed,
                   std::function<void(int)> command,
                   std::function<std::wstring()> encodingSummary,
                   std::function<bool()> encodingEnabled)
        : settings_(settings), currentFile_(std::move(currentFile)), changed_(std::move(changed)),
          closed_(std::move(closed)), command_(std::move(command)),
          encodingSummary_(std::move(encodingSummary)),
          encodingEnabled_(std::move(encodingEnabled)) {
        for (auto& ext : AssociationExtensions()) selectedExts_.insert(ext);
    }

    bool Create(HWND owner) {
        owner_ = owner;
        HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"LitheMdSettings";
        RegisterClassExW(&wc);
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | WS_VSCROLL;
        UINT dpi = GetDpiForWindow(owner);
        if (!dpi) dpi = 96;
        float sc = dpi / 96.0f;
        RECT rc{ 0, 0, (LONG)std::lround(620 * sc), (LONG)std::lround(870 * sc) };
        AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi);
        int ww = rc.right - rc.left;
        int wh = rc.bottom - rc.top;
        RECT work{};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
            ww = std::min(ww, (int)(work.right - work.left - 24));
            wh = std::min(wh, (int)(work.bottom - work.top - 24));
        }
        RECT orc{};
        GetWindowRect(owner, &orc);
        int ox = orc.left + ((orc.right - orc.left) - ww) / 2;
        int oy = orc.top + ((orc.bottom - orc.top) - wh) / 2;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
            if (ox < work.left + 8) ox = work.left + 8;
            if (oy < work.top + 8) oy = work.top + 8;
            if (ox + ww > work.right - 8) ox = work.right - ww - 8;
            if (oy + wh > work.bottom - 8) oy = work.bottom - wh - 8;
        }
        hwnd_ = CreateWindowExW(0, L"LitheMdSettings", TranslateUi(L"设置").c_str(), style, ox, oy, ww, wh,
                                owner, nullptr, inst, this);
        if (!hwnd_) return false;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
            RECT wr{};
            GetWindowRect(hwnd_, &wr);
            int maxH = (work.bottom - work.top) - 24;
            int maxW = (work.right - work.left) - 24;
            int newW = std::min((int)(wr.right - wr.left), maxW);
            int newH = std::min((int)(wr.bottom - wr.top), maxH);
            int newX = std::max((int)work.left + 8, std::min((int)wr.left, (int)work.right - newW - 8));
            int newY = std::max((int)work.top + 8, std::min((int)wr.top, (int)work.bottom - newH - 8));
            if (newW != wr.right - wr.left || newH != wr.bottom - wr.top ||
                newX != wr.left || newY != wr.top) {
                SetWindowPos(hwnd_, nullptr, newX, newY, newW, newH,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        SetDarkTitle(hwnd_, settings_->dark);
        UpdateWindow(hwnd_);
        return true;
    }

    HWND hwnd() const { return hwnd_; }
    void Activate() {
        if (hwnd_) { ShowWindow(hwnd_, SW_RESTORE); SetForegroundWindow(hwnd_); }
    }
    void ApplyTheme() {
        if (hwnd_) { SetDarkTitle(hwnd_, settings_->dark); InvalidateRect(hwnd_, nullptr, FALSE); }
        if (shortcutsWin_) shortcutsWin_->ApplyTheme();
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        SettingsWindow* self = (SettingsWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (msg == WM_NCCREATE) {
            auto* cs = (CREATESTRUCTW*)lp;
            self = (SettingsWindow*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
            self->hwnd_ = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        return self->Handle(msg, wp, lp);
    }

    void AddBtn(int id, float x, float y, float w, float h, std::wstring text,
                bool primary = false, bool enabled = true) {
        Rect r{ x, y, w, h, id, std::move(text), primary, enabled };
        rects_.push_back(std::move(r));
    }

    void Paint();
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void Click(int id);
    bool AllExtensionsSelected() const {
        for (auto& ext : AssociationExtensions())
            if (!selectedExts_.count(ext)) return false;
        return true;
    }

    struct Rect {
        float x, y, w, h;
        int id;
        std::wstring text;
        bool primary;
        bool enabled = true;
        bool Contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    };
    struct Check {
        std::wstring ext;
        DRect rect;
        int toolIndex = -1;
    };

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    std::unique_ptr<Canvas> canvas_;
    Settings* settings_ = nullptr;
    ShortcutWindow* shortcutsWin_ = nullptr;
    std::wstring currentFile_;
    std::function<void()> changed_;
    std::function<void()> closed_;
    std::function<void(int)> command_;
    std::function<std::wstring()> encodingSummary_;
    std::function<bool()> encodingEnabled_;
    std::vector<Rect> rects_;
    std::vector<Check> checks_;
    std::vector<Check> toolChecks_;
    std::set<std::wstring> selectedExts_;
    int hover_ = 0;
    float scrollY_ = 0.0f;
    float contentHeight_ = 2010.0f;
    std::wstring notice_;
    std::wstring pendingBackgroundDeletePath_;
    std::wstring pendingBackgroundDeleteId_;
};

void SettingsWindow::Paint() {
    if (!canvas_) canvas_ = std::make_unique<Canvas>();
    Canvas& c = *canvas_;
    if (!c.Begin(hwnd_, Palette::Get(settings_->theme))) return;
    c.SetTranslateUi(true);
    auto& p = c.palette();
    float w = c.widthDip(), h = c.heightDip();
    c.FillRect(0, 0, w, h, p.bg);
    float maxScroll = std::max(0.0f, contentHeight_ - h);
    scrollY_ = Clampf(scrollY_, 0.0f, maxScroll);
    UINT dpi = GetDpiForWindow(hwnd_);
    if (!dpi) dpi = 96;
    float sc = dpi / 96.0f;
    SCROLLINFO si{};
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = (int)std::lround(contentHeight_ * sc);
    si.nPage = (int)std::lround(h * sc);
    si.nPos = (int)std::lround(scrollY_ * sc);
    SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
    ShowScrollBar(hwnd_, SB_VERT, maxScroll > 0.5f);
    c.SetTransform(0, -scrollY_);
    rects_.clear();
    checks_.clear();
    c.DrawText(L"设置", 24, 16, 300, 34, 23, p.fg, true);
    c.FillRect(24, 78, w - 48, 1, p.border);

    c.DrawText(L"外观", 24, 94, 120, 22, 13, p.fg, true);
    AddBtn(40, 24, 122, 84, 36, L"蓝白", settings_->theme == L"blue");
    AddBtn(41, 116, 122, 84, 36, L"深色", settings_->theme == L"dark");
    AddBtn(42, 208, 122, 84, 36, L"暖纸", settings_->theme == L"warm");
    AddBtn(43, 300, 122, 84, 36, L"护眼", settings_->theme == L"green");

    c.DrawText(L"滚轮速度", 410, 94, 160, 22, 13, p.fg, true);
    AddBtn(3, 410, 122, 38, 36, L"−");
    c.StrokeRound(452, 122, 80, 36, 6, p.border);
    c.DrawText(std::to_wstring(settings_->wheelStep), 452, 122, 80, 36, 14, p.fg, true, false,
               DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    AddBtn(4, 540, 122, 38, 36, L"+");
    c.DrawText(L"每格滚动 " + std::to_wstring(settings_->wheelStep) + L" 行", 410, 164, 170, 16, 10.5f, p.muted);

    c.FillRect(24, 180, w - 48, 1, p.border);
    c.DrawText(L"文件关联与默认应用", 24, 194, 300, 22, 13, p.fg, true);

    bool allSelected = AllExtensionsSelected();
    DRect allRect{ w - 100, 190, 82, 24 };
    checks_.push_back({ L"*", allRect });
    c.FillRound(allRect.x + 1, allRect.y + 4, 14, 14, 3, allSelected ? p.accent : p.bg);
    c.StrokeRound(allRect.x + 1, allRect.y + 4, 14, 14, 3, allSelected ? p.accent : p.border);
    if (allSelected)
        c.DrawText(L"✓", allRect.x + 1, allRect.y + 3, 14, 14, 10.5f, HexColor(L"#FFFFFF"), true, false,
                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    c.DrawText(L"全选", allRect.x + 22, allRect.y, 56, 24, 11.5f,
               allSelected ? p.fg : p.muted, false, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);

    std::vector<std::wstring> exts = AssociationExtensions();
    for (size_t i = 0; i < exts.size(); i++) {
        float cx = 24 + (float)(i % 3) * 190.0f;
        float cy = 268 + (float)(i / 3) * 23.0f;
        DRect r{ cx, cy, 180, 22 };
        checks_.push_back({ exts[i], r });
        bool selected = selectedExts_.count(exts[i]) != 0;
        bool isDefault = AssociationIsDefault(exts[i]);
        Color border = selected ? p.accent : p.border;
        c.FillRound(cx + 1, cy + 4, 14, 14, 3, selected ? p.accent : p.bg);
        c.StrokeRound(cx + 1, cy + 4, 14, 14, 3, border);
        if (selected)
            c.DrawText(L"✓", cx + 1, cy + 3, 14, 14, 10.5f, HexColor(L"#FFFFFF"), true, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        c.DrawText(exts[i], cx + 22, cy, 116, 22, 11.5f, selected ? p.fg : p.muted, false, false,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        if (isDefault)
            c.DrawText(L"默认", cx + 136, cy, 40, 22, 9.5f, p.accent, false, false,
                       DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    }

    AddBtn(5, 24, 466, 124, 34, L"加入打开方式");
    AddBtn(6, 156, 466, 112, 34, L"取消注册");
    AddBtn(7, 276, 466, 122, 34, L"申请默认应用");
    AddBtn(8, 406, 466, 140, 34, L"Windows 默认应用");
    AddBtn(9, 24, 508, 244, 34, L"取消选中格式的默认应用");
    if (!notice_.empty())
        c.DrawText(notice_, 24, 548, w - 48, 34, 11,
                   settings_->dark ? HexColor(L"#3FB950") : HexColor(L"#1A7F37"), false, false,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);

    c.FillRect(24, 588, w - 48, 1, p.border);
    c.DrawText(L"画笔工具按钮", 24, 598, 220, 22, 13, p.fg, true);
    const wchar_t* toolLabels[] = { L"笔", L"擦", L"线", L"箭", L"框", L"圆", L"字" };
    const int toolOrder[] = { 0, 2, 3, 4, 5, 6 }; // select and eraser are pinned
    toolChecks_.clear();
    for (int k = 0; k < 6; k++) {
        int i = toolOrder[k];
        float cx = 24 + k * 80.0f;
        DRect r{ cx, 650, 74, 24 };
        toolChecks_.push_back({ L"", r, i });
        bool selected = (settings_->inkToolsVisible & (1 << i)) != 0;
        c.FillRound(cx + 1, 654, 14, 14, 3, selected ? p.accent : p.bg);
        c.StrokeRound(cx + 1, 654, 14, 14, 3, selected ? p.accent : p.border);
        if (selected)
            c.DrawText(L"✓", cx + 1, 653, 14, 14, 10.5f, HexColor(L"#FFFFFF"), true, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        c.DrawText(toolLabels[i], cx + 22, 646, 48, 24, 11.5f, selected ? p.fg : p.muted, false, false,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    }

    c.FillRect(24, 690, w - 48, 1, p.border);
    c.DrawText(L"配置文件", 24, 702, 160, 22, 13, p.fg, true);
    std::wstring cfg = settings_->configPath.empty() ? Settings::ResolveConfigPath() : settings_->configPath;
    c.FillRound(24, 726, w - 48, 34, 6, p.surface);
    c.StrokeRound(24, 726, w - 48, 34, 6, p.border);
    c.DrawText(FitText(c, cfg, 11.0f, false, w - 76), 34, 726, w - 76, 34, 11.0f, p.fg, false, true,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    AddBtn(10, 24, 768, 86, 32, L"打开");
    AddBtn(11, 118, 768, 86, 32, L"复制路径");
    AddBtn(12, 212, 768, 86, 32, L"定位");
    AddBtn(16, 306, 768, 96, 32, L"复制配置");
    AddBtn(17, 410, 768, 96, 32, L"快捷键");

    c.FillRect(24, 862, w - 48, 1, p.border);
    c.DrawText(L"新建画布", 24, 874, 220, 22, 13, p.fg, true);
    c.DrawText(L"默认 " + std::to_wstring(settings_->canvasWidth) + L" × " +
               std::to_wstring(settings_->canvasHeight),
               24, 898, w - 48, 20, 11.5f, p.muted);
    AddBtn(20, 24, 924, 94, 34, L"1024 × 1024");
    AddBtn(21, 126, 924, 94, 34, L"2048 × 2048");
    AddBtn(22, 228, 924, 94, 34, L"4096 × 4096");
    AddBtn(23, 340, 924, 88, 34, L"白色背景");
    AddBtn(24, 436, 924, 88, 34, L"黑色背景");

    c.FillRect(24, 1040, w - 48, 1, p.border);
    c.DrawText(L"文本与编码", 24, 1052, 220, 22, 13, p.fg, true);
    bool encOk = !encodingEnabled_ || encodingEnabled_();
    std::wstring encSummary = encodingSummary_ ? encodingSummary_() : L"当前没有活动文档";
    c.DrawText(encSummary, 24, 1076, w - 48, 38, 11.5f, p.muted, false, false,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
    AddBtn(30, 24, 1120, 96, 32, L"自动检测", false, encOk);
    AddBtn(31, 128, 1120, 96, 32, L"重新检测", false, encOk);
    AddBtn(32, 232, 1120, 96, 32, L"修复乱码", false, encOk);

    const wchar_t* encLabels[] = {
        L"UTF-8", L"UTF-16 LE", L"UTF-16 BE", L"UTF-32 LE", L"UTF-32 BE",
        L"GB18030", L"GBK", L"Big5", L"Shift-JIS", L"EUC-KR",
        L"Windows-1252", L"Windows-1251", L"Windows-1250", L"Latin-1", L"ANSI"
    };
    for (int i = 0; i < 15; i++) {
        float x = 24 + (i % 4) * 150.0f;
        float y = 1168 + (i / 4) * 36.0f;
        AddBtn(100 + i, x, y, 140, 32, encLabels[i], false, encOk);
    }

    c.DrawText(L"文档语言", 24, 1328, 160, 20, 11.5f, p.fg, true);
    const wchar_t* langLabels[] = { L"自动", L"简体中文", L"繁体中文", L"日文", L"韩文", L"西文" };
    for (int i = 0; i < 6; i++)
        AddBtn(200 + i, 24 + i * 94.0f, 1352, 86, 32, langLabels[i], false, encOk);

    c.DrawText(L"界面语言", 24, 1404, 160, 20, 11.5f, p.fg, true);
    const auto& uiLangs = UiLanguages();
    for (size_t i = 0; i < uiLangs.size() && i < 10; i++) {
        bool selected = _wcsicmp(uiLangs[i].code.c_str(), settings_->uiLanguage.c_str()) == 0;
        float x = 24 + (float)(i % 5) * 112.0f;
        float y = 1428 + (float)(i / 5) * 40.0f;
        AddBtn(300 + (int)i, x, y, 104, 32, uiLangs[i].name, selected);
    }
    AddBtn(33, 24, 1512, 116, 32, L"翻译文件夹");
    AddBtn(34, 148, 1512, 116, 32, L"重新扫描");

    c.FillRect(24, 1564, w - 48, 1, p.border);
    c.DrawText(L"阅读版式", 24, 1576, 220, 22, 13, p.fg, true);
    c.DrawText(L"字号", 24, 1606, 60, 20, 11.5f, p.muted);
    AddBtn(50, 64, 1600, 34, 32, L"A−");
    c.StrokeRound(104, 1600, 78, 32, 6, p.border);
    c.DrawText(std::to_wstring((int)std::lround(settings_->fontScale * 100)) + L"%",
               104, 1600, 78, 32, 11.5f, p.fg, true, false,
               DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    AddBtn(52, 188, 1600, 34, 32, L"A+");
    c.DrawText(L"正文宽度", 238, 1606, 76, 20, 11.5f, p.muted);
    AddBtn(53, 318, 1600, 60, 32, L"窄", false, settings_->contentWidth <= 760);
    AddBtn(54, 384, 1600, 70, 32, L"适中", false,
           settings_->contentWidth > 760 && settings_->contentWidth <= 940);
    AddBtn(55, 460, 1600, 60, 32, L"宽", false,
           settings_->contentWidth > 940 && settings_->contentWidth <= 1120);
    AddBtn(56, 526, 1600, 70, 32, L"超宽", false, settings_->contentWidth > 1120);

    c.FillRect(24, 1660, w - 48, 1, p.border);
    c.DrawText(L"背景图片", 24, 1672, 300, 22, 13, p.fg, true);
    std::wstring background = settings_->backgroundImage;
    if (background.empty()) background = L"未设置背景图片";
    else if (!FileExists(background)) background += L"（文件不存在）";
    background = FitText(c, background, 10.5f, false, w - 76);
    c.FillRound(24, 1698, w - 48, 30, 6, p.surface);
    c.StrokeRound(24, 1698, w - 48, 30, 6, p.border);
    c.DrawText(background, 34, 1698, w - 76, 30, 10.5f, p.fg, false, true,
               DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    AddBtn(60, 24, 1738, 132, 34, L"选择图片");
    AddBtn(61, 164, 1738, 104, 34, L"清除");
    c.DrawText(L"背景不透明度", 300, 1744, 120, 20, 11.5f, p.fg, true);
    int opacity = std::clamp(settings_->backgroundOpacity,
                            Settings::MinBackgroundOpacity, Settings::MaxBackgroundOpacity);
    AddBtn(62, 424, 1738, 34, 34, L"−");
    c.StrokeRound(462, 1738, 70, 34, 6, p.border);
    c.DrawText(std::to_wstring(opacity) + L"%", 462, 1738, 70, 34, 12, p.fg, true, false,
               DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    AddBtn(63, 536, 1738, 34, 34, L"+");
    int fit = std::clamp(settings_->backgroundFit,
                         Settings::MinBackgroundFit, Settings::MaxBackgroundFit);
    c.DrawText(L"填充方式", 24, 1786, 80, 20, 11.5f, p.fg, true);
    AddBtn(64, 104, 1780, 96, 32, L"铺满裁剪", fit == 0);
    AddBtn(65, 208, 1780, 96, 32, L"完整显示", fit == 1);
    AddBtn(66, 312, 1780, 80, 32, L"拉伸", fit == 2);
    AddBtn(67, 400, 1780, 80, 32, L"平铺", fit == 3);
    int anchor = std::clamp(settings_->backgroundAnchor,
                            Settings::MinBackgroundAnchor, Settings::MaxBackgroundAnchor);
    c.DrawText(L"裁剪位置", 24, 1828, 80, 20, 11.5f, p.fg, true);
    const wchar_t* anchorLabels[] = { L"左上", L"上", L"右上", L"左", L"中", L"右", L"左下", L"下", L"右下" };
    for (int i = 0; i < 9; i++)
        AddBtn(70 + i, 104 + i * 52.0f, 1822, 46, 32, anchorLabels[i], anchor == i);
    c.DrawText(L"5% – 60%，默认 15%；位置仅在铺满裁剪时生效", 24, 1866, w - 48, 18, 10.5f, p.muted);
    c.DrawText(L"选择后复制到配置目录并生成 ID，原图移动或删除不影响背景", 24, 1884, w - 48, 18, 10.5f, p.muted);
    c.FillRect(24, 1914, w - 48, 1, p.border);
    AddBtn(13, 24, 1928, 120, 34, L"恢复默认");
    AddBtn(14, 152, 1928, 132, 34, L"清空本地数据");
    AddBtn(15, w - 118, 1928, 94, 34, L"关闭", true);
    std::wstring footer = std::wstring(L"LitheMD ") + kAppVersion +
                          L"  ·  MIT License  ·  © 2026 LitheMD";
    c.DrawText(footer, 24, 1974, w - 48, 18, 10, p.muted, false, false,
               DWRITE_TEXT_ALIGNMENT_CENTER);

    for (auto& r : rects_) {
        if (r.enabled && r.id == hover_) c.FillRound(r.x, r.y, r.w, r.h, 7, p.hover);
        if (!r.enabled) {
            Color border = p.border;
            border.a *= 0.55f;
            Color fg = p.muted;
            fg.a *= 0.55f;
            c.StrokeRound(r.x, r.y, r.w, r.h, 7, border);
            c.DrawText(r.text, r.x, r.y, r.w, r.h, 12, fg, false, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        } else if (r.primary) {
            c.FillRound(r.x, r.y, r.w, r.h, 7, p.accent);
            c.DrawText(r.text, r.x, r.y, r.w, r.h, 12.5f, HexColor(L"#FFFFFF"), true, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        } else {
            c.StrokeRound(r.x, r.y, r.w, r.h, 7, p.border);
            c.DrawText(r.text, r.x, r.y, r.w, r.h, 12, p.fg, false, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    c.ResetTransform();
    c.End();
}

void SettingsWindow::Click(int id) {
    bool persist = true;
    if (id >= 100 && id < 115) {
        if (command_) command_(CMD_ENC_UTF8 + (id - 100));
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (id >= 200 && id < 206) {
        if (command_) command_(CMD_LANG_AUTO + (id - 200));
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (id >= 300 && id < 300 + (int)UiLanguages().size()) {
        settings_->uiLanguage = UiLanguages()[(size_t)(id - 300)].code;
        SetUiLanguage(settings_->uiLanguage);
        settings_->Save();
        if (hwnd_) {
            SetWindowTextW(hwnd_, TranslateUi(L"设置").c_str());
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        if (changed_) changed_();
        return;
    }
    switch (id) {
    case 40: settings_->theme = L"blue"; break;
    case 41: settings_->theme = L"dark"; break;
    case 42: settings_->theme = L"warm"; break;
    case 43: settings_->theme = L"green"; break;
    case 50: settings_->fontScale = std::max(Settings::MinFontScale, settings_->fontScale - 0.05); break;
    case 52: settings_->fontScale = std::min(Settings::MaxFontScale, settings_->fontScale + 0.05); break;
    case 53: settings_->contentWidth = 720; break;
    case 54: settings_->contentWidth = 860; break;
    case 55: settings_->contentWidth = 1040; break;
    case 56: settings_->contentWidth = 1200; break;
    case 60: {
        wchar_t file[32768] = L"";
        std::wstring filter = TranslateUi(L"图片") + L'\0' +
            L"*.png;*.jpg;*.jpeg;*.jpe;*.jfif;*.bmp;*.gif;*.tif;*.tiff;*.webp;*.ico\0" +
            TranslateUi(L"所有文件") + L'\0' + L"*.*\0" + L'\0';
        std::wstring title = TranslateUi(L"选择背景图片");
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        ofn.lpstrFilter = filter.c_str();
        ofn.lpstrFile = file;
        ofn.nMaxFile = 32768;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
        ofn.lpstrTitle = title.c_str();
        if (!GetOpenFileNameW(&ofn)) return;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (GetFileAttributesExW(file, GetFileExInfoStandard, &fad)) {
            uint64_t size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
            if (size > 64ull * 1024 * 1024) {
                MessageBoxW(hwnd_, TranslateUi(L"图片文件不能超过 64 MB。").c_str(), L"LitheMD",
                            MB_OK | MB_ICONWARNING);
                return;
            }
        }
        ImageRef probe;
        probe.path = file;
        probe.cacheKey = L"probe:" + std::wstring(file) + L":" +
                         std::to_wstring(FileWriteTimeUtc(file));
        float imageW = 0, imageH = 0;
        if (!ImagePixelSize(probe, imageW, imageH)) {
            MessageBoxW(hwnd_, TranslateUi(L"无法读取这个图片格式，请选择 PNG、JPEG、BMP、GIF、TIFF、WEBP 或 ICO 文件。").c_str(),
                        L"LitheMD", MB_OK | MB_ICONWARNING);
            return;
        }
        if ((uint64_t)imageW * (uint64_t)imageH > 64ull * 1000 * 1000) {
            MessageBoxW(hwnd_, TranslateUi(L"图片尺寸过大，请选择不超过 6000 万像素的图片。").c_str(),
                        L"LitheMD", MB_OK | MB_ICONWARNING);
            return;
        }
        std::wstring oldPath = settings_->backgroundImage;
        std::wstring oldId = settings_->backgroundImageId;
        pendingBackgroundDeletePath_ = oldPath;
        pendingBackgroundDeleteId_ = oldId;
        std::wstring copyError;
        if (!CopyBackgroundToConfig(*settings_, file, copyError)) {
            pendingBackgroundDeletePath_.clear();
            pendingBackgroundDeleteId_.clear();
            MessageBoxW(hwnd_, TranslateUi(copyError).c_str(), L"LitheMD", MB_OK | MB_ICONWARNING);
            return;
        }
        if (_wcsicmp(oldPath.c_str(), settings_->backgroundImage.c_str()) == 0)
            pendingBackgroundDeletePath_.clear();
        notice_ = L"背景图片已复制到配置目录。";
        break;
    }
    case 61:
        pendingBackgroundDeletePath_ = settings_->backgroundImage;
        pendingBackgroundDeleteId_ = settings_->backgroundImageId;
        settings_->backgroundImage.clear();
        settings_->backgroundImageId.clear();
        notice_ = L"已清除背景图片。";
        break;
    case 62:
        settings_->backgroundOpacity = std::max(Settings::MinBackgroundOpacity,
                                                settings_->backgroundOpacity - 5);
        break;
    case 63:
        settings_->backgroundOpacity = std::min(Settings::MaxBackgroundOpacity,
                                                settings_->backgroundOpacity + 5);
        break;
    case 64:
    case 65:
    case 66:
    case 67:
        settings_->backgroundFit = id - 64;
        break;
    case 70:
    case 71:
    case 72:
    case 73:
    case 74:
    case 75:
    case 76:
    case 77:
    case 78:
        settings_->backgroundAnchor = id - 70;
        break;
    case 3: settings_->wheelStep = std::max(1, settings_->wheelStep - 1); break;
    case 4: settings_->wheelStep = std::min(10, settings_->wheelStep + 1); break;
    case 5: {
        auto sel = SelectedExtensions(selectedExts_);
        bool ok = RegisterAssociations(ExePath(), sel);
        if (!ok) {
            notice_ = L"注册未完全成功，请检查当前用户权限。";
        } else if (sel.empty()) {
            notice_ = L"已将 LitheMD 加入“打开方式”。勾选文件格式后可同时注册对应的扩展名。";
        } else {
            notice_ = L"已将 LitheMD 加入“打开方式”，并注册了选中的 " +
                      std::to_wstring(sel.size()) + L" 种文件格式。";
        }
        break;
    }
    case 6: {
        auto sel = SelectedExtensions(selectedExts_);
        if (sel.empty()) { notice_ = L"请先勾选要取消注册的文件格式。"; break; }
        UnregisterAssociations(sel);
        notice_ = L"已取消选中格式的“打开方式”注册。若某个格式当前仍由 LitheMD 打开，请在系统默认应用中改选其他应用。";
        break;
    }
    case 7: {
        auto sel = SelectedExtensions(selectedExts_);
        if (sel.empty()) { notice_ = L"请先勾选要设为默认的文件格式。"; break; }
        RegisterAssociations(ExePath(), sel);
        if (sel.size() == 1) {
            std::wstring file = AssociationProbeFile(sel[0]);
            OPENASINFO info{};
            info.pcszFile = file.c_str();
            info.oaifInFlags = OAIF_ALLOW_REGISTRATION | OAIF_REGISTER_EXT;
            HRESULT hr = SHOpenWithDialog(owner_, &info);
            notice_ = SUCCEEDED(hr) ? L"已在系统弹窗中完成选择。勾选“始终使用”即可设为默认。"
                                    : L"系统选择窗口未能完成。也可以在“Windows 默认应用”中调整。";
        } else {
            bool ok = LaunchAssociationUI();
            if (!ok) ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
            notice_ = ok ? L"已打开系统的默认应用权限设置，请勾选需要关联的格式。"
                         : L"已打开 Windows 默认应用设置，请在系统界面中完成权限确认。";
        }
        break;
    }
    case 8:
        ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case 9: {
        auto sel = SelectedExtensions(selectedExts_);
        if (sel.empty()) { notice_ = L"请先勾选要取消默认关联的文件格式。"; break; }
        UnregisterAssociations(sel);
        ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr, nullptr, SW_SHOWNORMAL);
        notice_ = L"已取消选中格式的注册。Windows 不允许程序直接清除默认应用，请在系统设置中为这些格式选择其他应用。";
        break;
    }
    case 10: {
        if (settings_->configPath.empty()) settings_->Save();
        OpenExternal(settings_->configPath.empty() ? Settings::ResolveConfigPath() : settings_->configPath);
        break;
    }
    case 11: {
        std::wstring cfg = settings_->configPath.empty() ? Settings::ResolveConfigPath() : settings_->configPath;
        SetClipboardText(hwnd_, cfg);
        notice_ = L"配置路径已复制到剪贴板。";
        break;
    }
    case 12: {
        std::wstring cfg = settings_->configPath.empty() ? Settings::ResolveConfigPath() : settings_->configPath;
        std::wstring args = L"/select,\"" + cfg + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        break;
    }
    case 13:
        settings_->theme = L"blue";
        settings_->dark = false;
        settings_->wheelStep = 3;
        settings_->zoom = 1.0;
        settings_->fontScale = 1.0;
        settings_->contentWidth = 1040;
        settings_->inkToolsVisible = Settings::DefaultInkTools;
        settings_->inkPresets = DefaultInkPresets();
        settings_->inkToolbarPresets = { 0, 1, 2, 3, 4 };
        settings_->documentLanguage = DocLanguage::Auto;
        settings_->canvasWidth = 2048;
        settings_->canvasHeight = 2048;
        settings_->canvasDark = false;
        pendingBackgroundDeletePath_ = settings_->backgroundImage;
        pendingBackgroundDeleteId_ = settings_->backgroundImageId;
        settings_->backgroundImage.clear();
        settings_->backgroundImageId.clear();
        settings_->backgroundOpacity = Settings::DefaultBackgroundOpacity;
        settings_->backgroundFit = 0;
        settings_->backgroundAnchor = Settings::DefaultBackgroundAnchor;
        settings_->shortcuts.clear();
        notice_ = L"已恢复默认（主题、滚轮速度、缩放、画笔工具按钮、背景图片、快捷键）。";
        break;
    case 17: {
        if (!shortcutsWin_) {
            shortcutsWin_ = new ShortcutWindow(settings_,
                [this] { if (changed_) changed_(); },
                [this] { shortcutsWin_ = nullptr; });
        }
        if (shortcutsWin_->hwnd()) {
            shortcutsWin_->Activate();
        } else if (!shortcutsWin_->Create(hwnd_)) {
            delete shortcutsWin_;
            shortcutsWin_ = nullptr;
            notice_ = L"无法打开快捷键设置窗口。";
        }
        break;
    }
    case 16: {
        settings_->Save();
        std::wstring cfg = settings_->configPath.empty() ? Settings::ResolveConfigPath() : settings_->configPath;
        std::vector<uint8_t> bytes;
        std::wstring enc, text;
        if (ReadFileBytes(cfg, bytes)) text = DecodeBytes(bytes, enc);
        if (!text.empty()) {
            SetClipboardText(hwnd_, text);
            notice_ = L"配置内容已复制到剪贴板。";
        } else {
            notice_ = L"无法读取配置文件。";
        }
        break;
    }
    case 14: {
        if (MessageBoxW(hwnd_, TranslateUi(L"将删除本地配置、最近记录和配置缓存。已打开的文件不会被删除。\n\n是否继续？").c_str(),
                        TranslateUi(L"清空本地数据").c_str(), MB_YESNO | MB_ICONWARNING) != IDYES)
            return;
        DeleteManagedBackground(*settings_, settings_->backgroundImage,
                                settings_->backgroundImageId);
        std::wstring localDir = LocalDataDir();
        std::vector<std::wstring> paths = {
            PathJoin(localDir, L"config.json"),
            PathJoin(localDir, L"state.ini"),
            PathJoin(localDir, L"reading-positions.json")
        };
        if (!settings_->configPath.empty()) paths.push_back(settings_->configPath);
        for (auto& path : paths) {
            if (FileExists(path)) DeleteFileW(path.c_str());
        }
        RemoveDirectoryW(localDir.c_str());
        settings_->theme = L"blue";
        settings_->dark = false;
        settings_->zoom = 1.0;
        settings_->wheelStep = 3;
        settings_->inkToolsVisible = Settings::DefaultInkTools;
        settings_->inkPresets = DefaultInkPresets();
        settings_->inkToolbarPresets = { 0, 1, 2, 3, 4 };
        settings_->documentLanguage = DocLanguage::Auto;
        settings_->canvasWidth = 2048;
        settings_->canvasHeight = 2048;
        settings_->canvasDark = false;
        settings_->backgroundImage.clear();
        settings_->backgroundImageId.clear();
        settings_->backgroundOpacity = Settings::DefaultBackgroundOpacity;
        settings_->backgroundFit = 0;
        settings_->backgroundAnchor = Settings::DefaultBackgroundAnchor;
        settings_->shortcuts.clear();
        settings_->recent.clear();
        settings_->recentBlocked.clear();
        settings_->workspaceHistory.clear();
        settings_->workspaceRoot.clear();
        settings_->workspaceVisible = false;
        settings_->configPath = Settings::ResolveConfigPath();
        selectedExts_.clear();
        notice_ = L"本地配置和历史记录已清空。";
        persist = false;
        break;
    }
    case 20:
    case 21:
    case 22: {
        int size = id == 20 ? 1024 : (id == 21 ? 2048 : 4096);
        settings_->canvasWidth = size;
        settings_->canvasHeight = size;
        notice_ = L"新建画布默认尺寸已设为 " + std::to_wstring(size) + L" × " + std::to_wstring(size) + L"。";
        break;
    }
    case 23:
        settings_->canvasDark = false;
        notice_ = L"新建画布默认使用白色背景。";
        break;
    case 24:
        settings_->canvasDark = true;
        notice_ = L"新建画布默认使用黑色背景。";
        break;
    case 30: if (command_) command_(CMD_ENC_AUTO); break;
    case 31: if (command_) command_(CMD_ENC_REDETECT); break;
    case 32: if (command_) command_(CMD_ENC_REPAIR); break;
    case 33: {
        std::wstring cfg = settings_->configPath.empty() ? Settings::ResolveConfigPath() : settings_->configPath;
        std::wstring dir = PathJoin(PathDir(cfg), L"translations");
        EnsureDir(dir);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    case 34:
        ReloadUiLanguages();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    case 15:
        DestroyWindow(hwnd_);
        return;
    default:
        return;
    }
    if (persist) {
        bool saved = settings_->Save();
        if (saved && !pendingBackgroundDeletePath_.empty()) {
            DeleteManagedBackground(*settings_, pendingBackgroundDeletePath_,
                                    pendingBackgroundDeleteId_);
            pendingBackgroundDeletePath_.clear();
            pendingBackgroundDeleteId_.clear();
        }
    }
    if (changed_) changed_();
    if (hwnd_) {
        SetDarkTitle(hwnd_, settings_->dark);
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
    }
}

LRESULT SettingsWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        Paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        float scale = GetDpiForWindow(hwnd_) / 96.0f;
        float x = GET_X_LPARAM(lp) / scale, y = GET_Y_LPARAM(lp) / scale + scrollY_;
        int hover = 0;
        for (auto& r : rects_) if (r.enabled && r.Contains(x, y)) { hover = r.id; break; }
        bool overCheck = false;
        for (auto& ch : checks_) if (ch.rect.Contains(x, y)) { overCheck = true; break; }
        for (auto& ch : toolChecks_) if (ch.rect.Contains(x, y)) { overCheck = true; break; }
        if (hover != hover_) { hover_ = hover; InvalidateRect(hwnd_, nullptr, FALSE); }
        SetCursor(LoadCursorW(nullptr, (hover || overCheck) ? IDC_HAND : IDC_ARROW));
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        hover_ = 0;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        float scale = GetDpiForWindow(hwnd_) / 96.0f;
        float x = GET_X_LPARAM(lp) / scale, y = GET_Y_LPARAM(lp) / scale + scrollY_;
        for (auto& ch : toolChecks_) {
            if (ch.rect.Contains(x, y)) {
                if (ch.toolIndex >= 0) {
                    settings_->inkToolsVisible ^= (1 << ch.toolIndex);
                    settings_->Save();
                    if (changed_) changed_();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            }
        }
        for (auto& ch : checks_) {
            if (ch.rect.Contains(x, y)) {
                if (ch.ext == L"*") {
                    if (AllExtensionsSelected()) {
                        selectedExts_.clear();
                    } else {
                        for (auto& ext : AssociationExtensions()) selectedExts_.insert(ext);
                    }
                } else if (selectedExts_.count(ch.ext)) {
                    selectedExts_.erase(ch.ext);
                } else {
                    selectedExts_.insert(ch.ext);
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        for (auto& r : rects_) if (r.Contains(x, y)) { if (r.enabled) Click(r.id); break; }
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_MOUSEWHEEL: {
        float scale = GetDpiForWindow(hwnd_) / 96.0f;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        float h = rc.bottom / scale;
        float maxScroll = std::max(0.0f, contentHeight_ - h);
        scrollY_ = Clampf(scrollY_ - GET_WHEEL_DELTA_WPARAM(wp) / 120.0f * 60.0f, 0.0f, maxScroll);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_VSCROLL: {
        float scale = GetDpiForWindow(hwnd_) / 96.0f;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        float h = rc.bottom / scale;
        float maxScroll = std::max(0.0f, contentHeight_ - h);
        float step = 44.0f;
        switch (LOWORD(wp)) {
        case SB_LINEUP: scrollY_ -= step; break;
        case SB_LINEDOWN: scrollY_ += step; break;
        case SB_PAGEUP: scrollY_ -= h * 0.85f; break;
        case SB_PAGEDOWN: scrollY_ += h * 0.85f; break;
        case SB_TOP: scrollY_ = 0; break;
        case SB_BOTTOM: scrollY_ = maxScroll; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: {
            SCROLLINFO si{};
            si.cbSize = sizeof(si);
            si.fMask = SIF_TRACKPOS;
            if (GetScrollInfo(hwnd_, SB_VERT, &si)) scrollY_ = si.nTrackPos / scale;
            break;
        }
        default: break;
        }
        scrollY_ = Clampf(scrollY_, 0.0f, maxScroll);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd_);
        return 0;
    case WM_NCACTIVATE:
        SetDarkTitle(hwnd_, settings_->dark);
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        if (closed_) closed_();
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        delete this;
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ================================================================ MainWindow
class MainWindow {
public:
    MainWindow(Settings settings, std::vector<std::wstring> initialFiles, bool openSettings)
        : settings_(std::move(settings)), initialFiles_(std::move(initialFiles)), openSettings_(openSettings) {}
    ~MainWindow() { watcher_.Stop(); }

    bool Create(HINSTANCE inst) {
        inst_ = inst;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"LitheMdMain";
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = wc.hIcon;
        RegisterClassExW(&wc);

        hwnd_ = CreateWindowExW(0, L"LitheMdMain", L"LitheMD",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                1180, 820, nullptr, nullptr, inst, this);
        if (!hwnd_) return false;
        {
            UINT dpi = GetDpiForWindow(hwnd_);
            if (!dpi) dpi = 96;
            float sc = dpi / 96.0f;
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi)) {
                int mw = mi.rcWork.right - mi.rcWork.left;
                int mh = mi.rcWork.bottom - mi.rcWork.top;
                int ww = std::min((int)std::lround(1180 * sc), mw - 40);
                int wh = std::min((int)std::lround(820 * sc), mh - 40);
                int wx = mi.rcWork.left + (mw - ww) / 2;
                int wy = mi.rcWork.top + (mh - wh) / 2;
                SetWindowPos(hwnd_, nullptr, wx, wy, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        SetDarkTitle(hwnd_, settings_.dark);
        DragAcceptFiles(hwnd_, TRUE);
        positionsPath_ = PathJoin(PathDir(settings_.configPath.empty() ? Settings::ResolveConfigPath()
                                                                      : settings_.configPath),
                                  L"reading-positions.json");
        positions_ = ReadingPositions::Load(positionsPath_);
        tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                   WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                   hwnd_, nullptr, inst, nullptr);
        if (tooltip_) {
            TOOLINFOW ti{};
            ti.cbSize = sizeof(ti);
            ti.uFlags = TTF_SUBCLASS;
            ti.hwnd = hwnd_;
            ti.uId = 1;
            ti.lpszText = const_cast<wchar_t*>(L"");
            SendMessageW(tooltip_, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 900);
        }

        DocView::Register(inst);
        OutlineView::Register(inst);
        WorkspaceView::Register(inst);

        home_ = std::make_unique<DocView>();
        home_->SetSettings(&settings_);
        if (!home_->Create(hwnd_, 100)) return false;
        WireView(home_.get(), true);
        home_->ApplyTheme(settings_.theme);
        home_->SetWheelStep(settings_.wheelStep);
        home_->SetZoom(settings_.zoom);
        home_->SetFontScale(settings_.fontScale);
        home_->SetContentWidth(settings_.contentWidth);
        home_->SetRecent(settings_.recent);
        home_->SetWorkspaceHistory(settings_.workspaceHistory);

        outline_.Create(hwnd_, 101);
        outline_.ApplyTheme(settings_.theme);
        outline_.SetCallback([this](int block) {
            if (DocView* v = ActiveView()) v->ScrollToBlock(block);
            outline_.SetActive(block);
        });

        workspace_.Create(hwnd_, 102);
        workspace_.ApplyTheme(settings_.theme);
        workspace_.SetCallbacks([this](const std::wstring& path) {
            OpenFile(path, true, true);
        }, [this] {
            settings_.workspaceVisible = false;
            settings_.Save();
            LayoutChildren();
            InvalidateChrome();
        });
        if (!settings_.workspaceRoot.empty() && DirExists(settings_.workspaceRoot)) {
            std::wstring wsError;
            if (!workspace_.OpenRoot(settings_.workspaceRoot, &wsError))
                settings_.workspaceVisible = false;
        } else {
            settings_.workspaceVisible = false;
        }

        LayoutChildren();
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        UpdateWindow(hwnd_);
        for (size_t i = 0; i < initialFiles_.size(); i++)
            OpenFile(initialFiles_[i], true, i + 1 == initialFiles_.size());
        UpdateTitle();
        InvalidateChrome();
        if (openSettings_) PostMessageW(hwnd_, WM_COMMAND, CMD_APP_OPEN_SETTINGS, 0);
        return true;
    }

    int Run() {
        RebuildShortcuts();
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (presentation_ && msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
                TogglePresentation();
                continue;
            }
            if (HandleShortcutMessage(msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return (int)msg.wParam;
    }

private:
    struct Tab {
        std::unique_ptr<DocView> view;
        std::wstring path;
        std::wstring title;
        uint64_t fileTime = 0;
        TextEncoding encoding = TextEncoding::Auto;
        DocLanguage language = DocLanguage::Auto;
        bool error = false;
        bool externalChange = false;
    };

    struct TabHit {
        int index = -1;          // -1 = home
        bool home = false;
        bool close = false;
        DRect rect;
        DRect closeRect;
    };

    struct ActiveShortcut {
        ShortcutKey key;
        int cmd = 0;
        bool navigation = false;
    };

    struct PendingOpen {
        std::wstring path;
        bool addRecent = true;
        bool activate = true;
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        MainWindow* self = (MainWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (msg == WM_NCCREATE) {
            auto* cs = (CREATESTRUCTW*)lp;
            self = (MainWindow*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
            self->hwnd_ = hwnd;
        }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        return self->Handle(msg, wp, lp);
    }

    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_GETMINMAXINFO: {
            auto* mmi = (MINMAXINFO*)lp;
            UINT dpi = GetDpiForWindow(hwnd_);
            if (!dpi) dpi = 96;
            float sc = dpi / 96.0f;
            mmi->ptMinTrackSize.x = (LONG)std::lround(640 * sc);
            mmi->ptMinTrackSize.y = (LONG)std::lround(500 * sc);
            return 0;
        }
        case WM_SIZE:
            LayoutChildren();
            InvalidateChrome();
            // Paint chrome and the visible child synchronously. Otherwise the
            // newly exposed strips can stay black until the next idle paint
            // while the user is dragging the window edge.
            RedrawWindow(hwnd_, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            if (resizeTimer_) KillTimer(hwnd_, resizeTimer_);
            resizeTimer_ = SetTimer(hwnd_, 1, 16, nullptr);
            return 0;
        case WM_ENTERSIZEMOVE:
            return 0;
        case WM_EXITSIZEMOVE:
            LayoutChildren();
            InvalidateChrome();
            UpdateWindow(hwnd_);
            if (resizeTimer_) {
                KillTimer(hwnd_, resizeTimer_);
                resizeTimer_ = 0;
            }
            RedrawWindow(hwnd_, nullptr, nullptr,
                         RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            return 0;
        case WM_TIMER:
            if (wp == 1) {
                KillTimer(hwnd_, 1);
                resizeTimer_ = 0;
                RedrawWindow(hwnd_, nullptr, nullptr,
                             RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
                return 0;
            }
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd_, &ps);
            PaintChrome();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_COMMAND:
            if (HIWORD(wp) == 0) {
                Command(LOWORD(wp));
                return 0;
            }
            break;
        case WM_MOUSEMOVE: {
            float scale = GetDpiForWindow(hwnd_) / 96.0f;
            float x = GET_X_LPARAM(lp) / scale, y = GET_Y_LPARAM(lp) / scale;
            if (y >= kHeaderH && y < kHeaderH + kTabH) {
                int hover = -2;
                bool close = false;
                for (auto& h : tabHits_) {
                    if (h.closeRect.Contains(x, y)) { hover = h.index; close = true; break; }
                    if (h.rect.Contains(x, y)) { hover = h.index; break; }
                }
                if (hover != hoverTab_ || close != hoverTabClose_) {
                    hoverTab_ = hover;
                    hoverTabClose_ = close;
                    UpdateTabTooltip(hover);
                    InvalidateChrome();
                }
                hoverCmd_ = 0;
                SetCursor(LoadCursorW(nullptr, (hover >= -1) ? IDC_HAND : IDC_ARROW));
            } else {
                if (hoverTab_ != -2) {
                    hoverTab_ = -2;
                    hoverTabClose_ = false;
                    if (tooltip_) SendMessageW(tooltip_, TTM_POP, 0, 0);
                    InvalidateChrome();
                }
                int hover = 0;
                for (auto& b : buttons_)
                    if (b.enabled && b.rect.Contains(x, y)) { hover = b.cmd; break; }
                if (hover != hoverCmd_) { hoverCmd_ = hover; InvalidateChrome(); }
                SetCursor(LoadCursorW(nullptr, hover ? IDC_HAND : IDC_ARROW));
            }
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            hoverCmd_ = 0;
            hoverTab_ = -2;
            hoverTabClose_ = false;
            if (tooltip_) SendMessageW(tooltip_, TTM_POP, 0, 0);
            InvalidateChrome();
            return 0;
        case WM_LBUTTONDOWN: {
            float scale = GetDpiForWindow(hwnd_) / 96.0f;
            float x = GET_X_LPARAM(lp) / scale, y = GET_Y_LPARAM(lp) / scale;
            if (y >= kHeaderH && y < kHeaderH + kTabH) {
                for (auto& h : tabHits_) {
                    if (h.closeRect.Contains(x, y)) {
                        if (!h.home) CloseTab(h.index);
                        return 0;
                    }
                    if (h.rect.Contains(x, y)) {
                        SwitchTab(h.home ? -1 : h.index);
                        return 0;
                    }
                }
                return 0;
            }
            for (auto& b : buttons_) {
                if (b.enabled && b.rect.Contains(x, y)) {
                    Command(b.cmd);
                    return 0;
                }
            }
            return 0;
        }
        case WM_MBUTTONDOWN: {
            float scale = GetDpiForWindow(hwnd_) / 96.0f;
            float x = GET_X_LPARAM(lp) / scale, y = GET_Y_LPARAM(lp) / scale;
            if (y >= kHeaderH && y < kHeaderH + kTabH) {
                for (auto& h : tabHits_) {
                    if (h.rect.Contains(x, y)) {
                        if (!h.home) CloseTab(h.index);
                        return 0;
                    }
                }
                return 0;
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ScreenToClient(hwnd_, &pt);
            float scale = GetDpiForWindow(hwnd_) / 96.0f;
            float y = pt.y / scale;
            if (y >= kHeaderH && y < kHeaderH + kTabH) {
                scrollTabs(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -90.0f : 90.0f);
                return 0;
            }
            break;
        }
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            UINT count = std::min<UINT>(n, 32);
            for (UINT i = 0; i < count; i++) {
                wchar_t path[32768];
                if (DragQueryFileW(drop, i, path, 32768) > 0) OpenFile(path, true, i + 1 == count);
            }
            DragFinish(drop);
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE && active_ >= 0 && !loading_) {
                Tab& t = tabs_[(size_t)active_];
                uint64_t now = FileWriteTimeUtc(t.path);
                if (now && t.fileTime && now != t.fileTime) {
                    if (t.view->HasUnsavedNotes()) {
                        t.externalChange = true;
                        InvalidateChrome();
                    } else {
                        Reload(true);
                    }
                }
            }
            return 0;
        case kFileChangedMsg:
            OnFilesChanged();
            return 0;
        case WM_DPICHANGED: {
            RECT* r = (RECT*)lp;
            SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            SetDarkTitle(hwnd_, settings_.dark);
            ForEachView([](DocView* v) { v->RefreshLayout(); });
            LayoutChildren();
            InvalidateChrome();
            return 0;
        }
        case WM_CLOSE:
            if (!ConfirmCloseAll()) return 0;
            RememberAllReadingPositions();
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }

    DocView* ActiveView() const {
        if (active_ >= 0 && active_ < (int)tabs_.size()) return tabs_[(size_t)active_].view.get();
        return home_.get();
    }
    Tab* ActiveTab() {
        if (active_ >= 0 && active_ < (int)tabs_.size()) return &tabs_[(size_t)active_];
        return nullptr;
    }
    std::wstring ActivePath() const {
        if (active_ >= 0 && active_ < (int)tabs_.size()) return tabs_[(size_t)active_].path;
        return L"";
    }
    std::wstring TabTitle(const Tab& tab) const {
        return tab.title.empty() ? PathName(tab.path) : tab.title;
    }
    std::wstring ActiveTitle() const {
        if (active_ >= 0 && active_ < (int)tabs_.size()) return TabTitle(tabs_[(size_t)active_]);
        return L"主页";
    }

    template <class Fn>
    void ForEachView(Fn fn) {
        if (home_) fn(home_.get());
        for (auto& t : tabs_) if (t.view) fn(t.view.get());
    }

    void WireView(DocView* view, bool home) {
        DocViewEvents ev;
        ev.openFile = [this] { OpenFileDialog(); };
        ev.openMenu = [this] { ShowOpenMenu(); };
        ev.openFolder = [this] { OpenFolderDialog(); };
        ev.openPath = [this](const std::wstring& p) { OpenFile(p, true, true); };
        ev.openWorkspace = [this](const std::wstring& p) {
            if (DirExists(p)) OpenWorkspace(p, true);
            else MessageBoxW(hwnd_, TranslateUi(L"该工作区不存在或无法访问。").c_str(),
                             L"LitheMD", MB_OK | MB_ICONWARNING);
        };
        ev.clearWorkspaces = [this] {
            if (settings_.workspaceHistory.empty()) return;
            if (MessageBoxW(hwnd_, TranslateUi(L"清空全部最近工作区？").c_str(), L"LitheMD",
                            MB_YESNO | MB_ICONQUESTION) != IDYES) return;
            settings_.ClearWorkspaces();
            if (home_) home_->SetWorkspaceHistory(settings_.workspaceHistory);
            InvalidateChrome();
        };
        ev.removeRecent = [this](const std::wstring& p) {
            settings_.RemoveRecent(p);
            if (home_) home_->SetRecent(settings_.recent);
            InvalidateChrome();
        };
        ev.clearRecent = [this] {
            if (settings_.recent.empty()) return;
            if (MessageBoxW(hwnd_, TranslateUi(L"清空全部最近记录？").c_str(), L"LitheMD",
                            MB_YESNO | MB_ICONQUESTION) != IDYES) return;
            settings_.ClearRecent();
            if (home_) home_->SetRecent(settings_.recent);
            InvalidateChrome();
        };
        ev.reload = [this, view, home] {
            if (!home && view == ActiveView()) Reload(false);
        };
        ev.statusChanged = [this, view] {
            if (view == ActiveView()) {
                if (outline_.hwnd() && view->Doc()) outline_.SetActive(view->VisibleHeadingBlock());
                InvalidateChrome();
            }
        };
        ev.outlineChanged = [this, view] {
            if (view == ActiveView()) { UpdateOutline(); LayoutChildren(); InvalidateChrome(); }
        };
        ev.sourceModeChanged = [this, view] {
            if (view == ActiveView()) { UpdateOutline(); InvalidateChrome(); }
        };
        ev.openColorPicker = [this, view](int preset) { OpenColorPicker(view, preset); };
        ev.requestZoom = [this](int dir) {
            if (dir == 0) SetZoom(1.0);
            else SetZoom(settings_.zoom + (dir > 0 ? 0.1 : -0.1));
        };
        view->SetEvents(std::move(ev));
    }

    void RebuildShortcuts() {
        shortcuts_.clear();
        for (auto& def : ShortcutCommands()) {
            for (auto& key : EffectiveShortcuts(settings_, def)) {
                if (!key.Valid() || !IsShortcutAllowed(key)) continue;
                bool duplicate = false;
                for (auto& existing : shortcuts_) {
                    if (existing.key == key) { duplicate = true; break; }
                }
                if (!duplicate) shortcuts_.push_back({ key, def.cmd, def.navigation });
            }
        }
    }

    bool HandleShortcutMessage(const MSG& msg) {
        if (msg.message != WM_KEYDOWN && msg.message != WM_SYSKEYDOWN) return false;
        if (msg.hwnd != hwnd_ && !IsChild(hwnd_, msg.hwnd)) return false;
        if (IsTextInputFocus(GetFocus())) return false;
        UINT vk = (UINT)msg.wParam;
        if (vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
            vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
            vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
            vk == VK_LWIN || vk == VK_RWIN || vk == VK_PROCESSKEY) return false;
        UINT mods = 0;
        if (GetKeyState(VK_CONTROL) & 0x8000) mods |= ShortcutCtrl;
        if (GetKeyState(VK_MENU) & 0x8000) mods |= ShortcutAlt;
        if (GetKeyState(VK_SHIFT) & 0x8000) mods |= ShortcutShift;
        for (auto& binding : shortcuts_) {
            if (binding.key.mods != mods || binding.key.vk != vk) continue;
            if (msg.lParam & 0x40000000) {
                // Auto-repeat is useful for scrolling, but not for toggles.
                if (!binding.navigation) return true;
            }
            DispatchShortcut(binding.cmd);
            return true;
        }
        return false;
    }

    void DispatchShortcut(int cmd) {
        DocView* view = ActiveView();
        switch (cmd) {
        case CMD_COPY:
            if (view) view->CopySelection();
            return;
        case CMD_SCROLL_UP:
            if (view) view->ScrollBy(0, -40 * (float)settings_.zoom);
            return;
        case CMD_SCROLL_DOWN:
            if (view) view->ScrollBy(0, 40 * (float)settings_.zoom);
            return;
        case CMD_SCROLL_LEFT:
            if (view) view->ScrollBy(-40 * (float)settings_.zoom, 0);
            return;
        case CMD_SCROLL_RIGHT:
            if (view) view->ScrollBy(40 * (float)settings_.zoom, 0);
            return;
        case CMD_PAGE_UP:
            if (view) view->ScrollPage(-1);
            return;
        case CMD_PAGE_DOWN:
            if (view) view->ScrollPage(1);
            return;
        case CMD_SCROLL_HOME:
            if (view) view->ScrollHome();
            return;
        case CMD_SCROLL_END:
            if (view) view->ScrollEnd();
            return;
        default:
            Command(cmd);
            return;
        }
    }

    void Command(int id) {
        if ((id >= CMD_ENC_AUTO && id <= CMD_ENC_REPAIR) ||
            (id >= CMD_LANG_AUTO && id <= CMD_LANG_WESTERN)) {
            if (!EncodingApplies()) return;
        }
        switch (id) {
        case CMD_OPEN: OpenFileDialog(); break;
        case CMD_OPEN_MENU: ShowOpenMenu(); break;
        case CMD_OPEN_FOLDER: OpenFolderDialog(); break;
        case CMD_PATH: ShowPathDialog(); break;
        case CMD_WORKSPACE: ToggleWorkspace(); break;
        case CMD_REFRESH_WORKSPACE:
            if (workspace_.HasRoot()) {
                workspace_.Refresh();
                workspace_.SetActivePath(ActivePath(), false);
                LayoutChildren();
                InvalidateChrome();
            }
            break;
        case CMD_LOCATE:
            if (!ActivePath().empty() && workspace_.HasRoot()) {
                if (workspace_.Model().Locate(ActivePath())) {
                    workspace_.SetActivePath(ActivePath(), false);
                    InvalidateChrome();
                }
            }
            break;
        case CMD_SPLIT: ToggleSplit(); break;
        case CMD_PRESENT: TogglePresentation(); break;
        case CMD_RELOAD: if (!ActivePath().empty()) Reload(false); break;
        case CMD_CLOSE_TAB: CloseCurrentTab(); break;
        case CMD_PREVIEW: if (DocView* v = ActiveView()) v->SetSource(false); break;
        case CMD_SOURCE: if (DocView* v = ActiveView()) v->SetSource(true); break;
        case CMD_INK:
            if (active_ < 0 || !(ActiveView() && ActiveView()->Doc())) {
                NewCanvas();
            } else if (DocView* v = ActiveView()) {
                v->SetInkMode(!v->InkMode());
                InvalidateChrome();
            }
            break;
        case CMD_UNDO_INK:
            if (DocView* v = ActiveView()) v->UndoInk();
            break;
        case CMD_REDO_INK:
            if (DocView* v = ActiveView()) v->RedoInk();
            break;
        case CMD_SAVE_NOTES:
            if (DocView* v = ActiveView(); v && v->CanSaveNotes() && v->HasUnsavedNotes()) {
                std::wstring noteError;
                if (!v->SaveNotes(&noteError)) {
                    std::wstring msg = noteError.empty() ? L"保存笔记失败" : noteError;
                    MessageBoxW(hwnd_, TranslateUi(msg).c_str(),
                                L"LitheMD", MB_OK | MB_ICONWARNING);
                } else
                    InvalidateChrome();
            }
            break;
        case CMD_PASTE_INK:
            if (DocView* v = ActiveView()) v->PasteInkSelection();
            break;
        case CMD_DELETE_INK:
            if (DocView* v = ActiveView()) v->DeleteInkSelection();
            break;
        case CMD_GROUP_INK:
            if (DocView* v = ActiveView()) v->GroupInkSelection(true);
            break;
        case CMD_UNGROUP_INK:
            if (DocView* v = ActiveView()) v->GroupInkSelection(false);
            break;
        case CMD_HIGHLIGHT_PDF:
            if (DocView* v = ActiveView()) v->SetPdfHighlighter();
            break;
        case CMD_EXPORT: {
            DocView* v = ActiveView();
            if (v && v->Doc()) {
                auto* win = new ExportWindow(v, &settings_);
                if (!win->Create(hwnd_)) delete win;
            }
            break;
        }
        case CMD_ENC_AUTO:
        case CMD_ENC_REDETECT:
            if (Tab* tab = ActiveTab()) {
                tab->encoding = TextEncoding::Auto;
                Reload(false);
            }
            break;
        case CMD_ENC_REPAIR:
            Reload(false, true);
            break;
        case CMD_TOC: ToggleOutline(); break;
        case CMD_THEME: ToggleTheme(); break;
        case CMD_SETTINGS:
        case CMD_APP_OPEN_SETTINGS: OpenSettings(); break;
        case CMD_ZOOM_IN: SetZoom(settings_.zoom + 0.1); break;
        case CMD_ZOOM_OUT: SetZoom(settings_.zoom - 0.1); break;
        case CMD_ZOOM_RESET: SetZoom(1.0); break;
        case CMD_JSON_EXPAND: if (DocView* v = ActiveView()) v->ExpandJson(true); break;
        case CMD_JSON_COLLAPSE: if (DocView* v = ActiveView()) v->ExpandJson(false); break;
        }
        if (id >= CMD_ENC_UTF8 && id <= CMD_ENC_ANSI) {
            static const TextEncoding map[] = {
                TextEncoding::Utf8, TextEncoding::Utf16LE, TextEncoding::Utf16BE,
                TextEncoding::Utf32LE, TextEncoding::Utf32BE, TextEncoding::Gb18030,
                TextEncoding::Gbk, TextEncoding::Big5, TextEncoding::ShiftJis,
                TextEncoding::EucKr, TextEncoding::Windows1252, TextEncoding::Windows1251,
                TextEncoding::Windows1250, TextEncoding::Latin1, TextEncoding::Ansi
            };
            if (Tab* tab = ActiveTab()) {
                tab->encoding = map[id - CMD_ENC_UTF8];
                Reload(false);
            }
            return;
        }
        if (id >= CMD_LANG_AUTO && id <= CMD_LANG_WESTERN) {
            static const DocLanguage map[] = {
                DocLanguage::Auto, DocLanguage::SimplifiedChinese, DocLanguage::TraditionalChinese,
                DocLanguage::Japanese, DocLanguage::Korean, DocLanguage::Western
            };
            if (Tab* tab = ActiveTab()) {
                tab->language = map[id - CMD_LANG_AUTO];
                settings_.documentLanguage = tab->language;
                settings_.Save();
                Reload(false);
            }
            return;
        }
    }

    void EnsureActiveTabVisible() {
        if (!hwnd_) return;
        if (active_ < 0) return;
        float scale = GetDpiForWindow(hwnd_) / 96.0f;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        float w = rc.right / scale;
        for (auto& h : tabHits_) {
            if (h.home) continue;
            if (h.index == active_) {
                if (h.rect.x < 8) tabScroll_ += h.rect.x - 8;
                float right = h.rect.x + h.rect.w;
                if (right > w - 8) tabScroll_ += right - (w - 8);
                if (tabScroll_ < 0) tabScroll_ = 0;
                break;
            }
        }
    }

    void RememberReadingPosition(DocView* view, const std::wstring& path) {
        if (!view || path.empty()) return;
        double fraction = view->ScrollFraction();
        if (fraction < 0) return;
        positions_.Set(path, fraction);
        positions_.Save(positionsPath_);
    }

    void RememberTabPosition(int index) {
        if (index < 0 || index >= (int)tabs_.size()) return;
        RememberReadingPosition(tabs_[(size_t)index].view.get(), tabs_[(size_t)index].path);
    }

    void RememberAllReadingPositions() {
        for (auto& tab : tabs_) RememberReadingPosition(tab.view.get(), tab.path);
    }

    void SwitchTab(int index) {
        if (index < -1 || index >= (int)tabs_.size()) return;
        if (index == active_) {
            if (DocView* v = ActiveView()) v->FocusView();
            return;
        }
        RememberTabPosition(active_);
        bool reloadAfter = false;
        if (index >= 0) {
            Tab& next = tabs_[(size_t)index];
            if (next.externalChange) {
                if (next.view->HasUnsavedNotes() && !ConfirmSaveNotes(next.view.get())) return;
                next.externalChange = false;
                reloadAfter = true;
            }
        }
        active_ = index;
        EnsureActiveTabVisible();
        LayoutChildren();
        UpdateTitle();
        UpdateOutline();
        workspace_.SetActivePath(ActivePath(), true);
        StartWatching();
        if (reloadAfter) Reload(true);
        UpdateSplitContent();
        InvalidateChrome();
        if (DocView* v = ActiveView()) v->FocusView();
    }

    void StartWatching() {
        std::wstring path = ActivePath();
        if (path.empty()) {
            watcher_.Stop();
            return;
        }
        std::wstring dir = PathDir(path);
        if (!DirExists(dir)) {
            watcher_.Stop();
            return;
        }
        watcher_.Start(hwnd_, kFileChangedMsg, dir);
    }

    void OnFilesChanged() {
        if (loading_) return;
        bool changed = false;
        for (auto& tab : tabs_) {
            if (tab.path.empty()) continue;
            uint64_t now = FileWriteTimeUtc(tab.path);
            if (now && tab.fileTime && now == tab.fileTime) continue;
            if (tab.view && tab.view->HasUnsavedNotes()) {
                tab.externalChange = true;
                changed = true;
                continue;
            }
            if (&tab == ActiveTab()) {
                Reload(true);
                changed = true;
            } else {
                tab.externalChange = true;
                changed = true;
            }
        }
        if (changed) InvalidateChrome();
    }

    bool ConfirmSaveNotes(DocView* view) {
        if (!view || !view->HasUnsavedNotes() || !view->CanSaveNotes()) return true;
        int result = MessageBoxW(hwnd_,
            TranslateUi(L"当前文档有未保存的笔记。\n\n是否保存后再继续？").c_str(),
            L"LitheMD", MB_YESNOCANCEL | MB_ICONWARNING);
        if (result == IDCANCEL) return false;
        if (result == IDNO) return true;
        std::wstring error;
        if (!view->SaveNotes(&error)) {
            std::wstring msg = error.empty() ? L"保存笔记失败" : error;
            MessageBoxW(hwnd_, TranslateUi(msg).c_str(),
                        L"LitheMD", MB_OK | MB_ICONERROR);
            return false;
        }
        return true;
    }

    bool ConfirmCloseAll() {
        bool dirty = false;
        for (auto& tab : tabs_) if (tab.view && tab.view->HasUnsavedNotes() && tab.view->CanSaveNotes()) dirty = true;
        if (!dirty) return true;
        int result = MessageBoxW(hwnd_,
            TranslateUi(L"有标签页含未保存的笔记。\n\n是否保存全部笔记后退出？").c_str(),
            L"LitheMD", MB_YESNOCANCEL | MB_ICONWARNING);
        if (result == IDCANCEL) return false;
        if (result == IDNO) return true;
        for (auto& tab : tabs_) {
            if (!tab.view || !tab.view->HasUnsavedNotes()) continue;
            std::wstring error;
            if (!tab.view->SaveNotes(&error)) {
                std::wstring msg = error.empty() ? L"保存笔记失败" : error;
                MessageBoxW(hwnd_, TranslateUi(msg).c_str(),
                            L"LitheMD", MB_OK | MB_ICONERROR);
                return false;
            }
        }
        return true;
    }

    void CloseTab(int index) {
        if (index < 0 || index >= (int)tabs_.size()) return;
        if (!ConfirmSaveNotes(tabs_[(size_t)index].view.get())) return;
        RememberTabPosition(index);
        int old = active_;
        DestroyWindow(tabs_[(size_t)index].view->hwnd());
        tabs_.erase(tabs_.begin() + index);
        if (tabs_.empty()) {
            active_ = -1;
        } else if (old == index) {
            active_ = std::min(index, (int)tabs_.size() - 1);
        } else if (old > index) {
            active_ = old - 1;
        }
        if (active_ < -1) active_ = -1;
        LayoutChildren();
        UpdateTitle();
        UpdateOutline();
        workspace_.SetActivePath(ActivePath(), false);
        StartWatching();
        UpdateSplitContent();
        InvalidateChrome();
        if (DocView* v = ActiveView()) v->FocusView();
    }

    void CloseCurrentTab() {
        if (active_ >= 0) CloseTab(active_);
    }

    void LayoutChildren() {
        if (!hwnd_) return;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        float scale = GetDpiForWindow(hwnd_) / 96.0f;
        int header = (int)std::lround(kHeaderH * scale);
        int tab = (int)std::lround(kTabH * scale);
        int status = (int)std::lround(kStatusH * scale);
        int bodyY = header + tab;
        int bodyH = std::max(0, (int)rc.bottom - bodyY - status);
        int bodyW = std::max(0, (int)rc.right);
        DocView* active = ActiveView();
        bool outline = !presentation_ && active_ >= 0 && active && outlineWanted_ && active->CanOutline();
        int outlineW = outline ? (int)std::lround(kOutlineW * scale) : 0;
        bool workspaceShown = workspace_.hwnd() && workspace_.HasRoot() && settings_.workspaceVisible &&
                              !presentation_;
        int workspaceW = workspaceShown ? (int)std::lround(248 * scale) : 0;
        int contentX = presentation_ ? 0 : workspaceW;
        int contentW = std::max(0, bodyW - workspaceW - outlineW);
        int contentBodyY = presentation_ ? 0 : bodyY;
        int contentBodyH = presentation_ ? (int)rc.bottom : bodyH;

        auto setVisible = [](HWND child, bool show) {
            if (!child) return;
            bool visible = IsWindowVisible(child) != FALSE;
            if (visible != show) ShowWindow(child, show ? SW_SHOW : SW_HIDE);
        };
        // Do not repaint every tab while the user is dragging the window edge.
        // Move only the active view; hidden tabs are laid out when activated.
        int activeW = (split_ && !presentation_) ? contentW / 2 : contentW;
        if (home_) {
            if (active_ < 0)
                MoveWindow(home_->hwnd(), contentX, contentBodyY, contentW, contentBodyH, FALSE);
            setVisible(home_->hwnd(), active_ < 0);
        }
        for (size_t i = 0; i < tabs_.size(); i++) {
            if ((int)i == active_ && tabs_[i].view)
                MoveWindow(tabs_[i].view->hwnd(), contentX, contentBodyY, activeW, contentBodyH, FALSE);
            if (tabs_[i].view)
                setVisible(tabs_[i].view->hwnd(), (int)i == active_);
        }
        if (splitView_ && split_ && !presentation_) {
            int leftW = contentW / 2;
            int rightW = contentW - leftW;
            if (DocView* v = ActiveView())
                MoveWindow(v->hwnd(), contentX, contentBodyY, leftW, contentBodyH, FALSE);
            MoveWindow(splitView_->hwnd(), contentX + leftW, contentBodyY, rightW, contentBodyH, FALSE);
            setVisible(splitView_->hwnd(), true);
        } else if (splitView_) {
            setVisible(splitView_->hwnd(), false);
        }
        if (workspace_.hwnd()) {
            if (workspaceShown)
                MoveWindow(workspace_.hwnd(), 0, bodyY, workspaceW, bodyH, FALSE);
            setVisible(workspace_.hwnd(), workspaceShown);
        }
        if (outline_.hwnd()) {
            if (outline)
                MoveWindow(outline_.hwnd(), contentX + contentW, bodyY, outlineW, bodyH, FALSE);
            setVisible(outline_.hwnd(), outline);
        }
        if (outline && active && active->Doc()) outline_.SetItems(active->Doc()->outline);
        EnsureActiveTabVisible();
    }

    void UpdateOutline() {
        DocView* view = ActiveView();
        bool show = active_ >= 0 && view && outlineWanted_ && view->CanOutline();
        if (outline_.hwnd()) {
            if (show) {
                outline_.SetItems(view->Doc() ? view->Doc()->outline : std::vector<Heading>{});
                outline_.SetActive(view->VisibleHeadingBlock());
                ShowWindow(outline_.hwnd(), SW_SHOW);
            } else {
                outline_.SetItems({});
                ShowWindow(outline_.hwnd(), SW_HIDE);
            }
        }
        LayoutChildren();
    }

    void ToggleOutline() {
        DocView* v = ActiveView();
        if (active_ < 0 || !v || !v->CanOutline()) return;
        outlineWanted_ = !outlineWanted_;
        UpdateOutline();
        InvalidateChrome();
    }

    void ApplySettingsToViews() {
        if (!positionsPath_.empty() && !FileExists(positionsPath_)) positions_.Clear();
        RebuildShortcuts();
        ForEachView([this](DocView* v) {
            v->ApplyTheme(settings_.theme);
            v->SetWheelStep(settings_.wheelStep);
            v->SetZoom(settings_.zoom);
            v->SetFontScale(settings_.fontScale);
            v->SetContentWidth(settings_.contentWidth);
            v->SetBackground(settings_.backgroundImage, settings_.backgroundOpacity,
                             settings_.backgroundFit, settings_.backgroundAnchor);
        });
        if (home_) {
            home_->SetRecent(settings_.recent);
            home_->SetWorkspaceHistory(settings_.workspaceHistory);
        }
        outline_.ApplyTheme(settings_.theme);
        workspace_.ApplyTheme(settings_.theme);
        SetDarkTitle(hwnd_, settings_.dark);
    }

    bool EncodingApplies() {
        Tab* tab = ActiveTab();
        Document* doc = tab && tab->view ? tab->view->Doc() : nullptr;
        return doc && !IsBinaryKind(doc->kind) && !Trim(doc->text).empty();
    }

    std::wstring EncodingSummary() {
        Tab* tab = ActiveTab();
        if (!tab) return L"当前没有活动文档";
        Document* doc = tab->view ? tab->view->Doc() : nullptr;
        if (!doc) return L"当前文档未加载完成";
        if (IsBinaryKind(doc->kind)) return L"当前文件不支持编码调整";
        if (Trim(doc->text).empty()) return L"文档为空";
        std::wstring s = L"当前编码：";
        s += doc->encoding.empty() ? EncodingName(tab->encoding) : doc->encoding;
        s += L"    文档语言：";
        s += LanguageShortName(doc->language);
        if (doc->encodingUncertain) s += L"    ·    编码不确定";
        if (doc->encodingRepaired) s += L"    ·    已修复乱码";
        return s;
    }

    void OpenSettings() {
        if (settingsWin_) { settingsWin_->Activate(); return; }
        ReloadUiLanguages();
        SetUiLanguage(settings_.uiLanguage);
        settingsWin_ = new SettingsWindow(&settings_, ActivePath(), [this] {
            ApplySettingsToViews();
            UpdateTitle();
            InvalidateChrome();
        }, [this] {
            settingsWin_ = nullptr;
            if (home_) home_->SetRecent(settings_.recent);
            InvalidateChrome();
        }, [this](int cmd) {
            Command(cmd);
        }, [this] {
            return EncodingSummary();
        }, [this] {
            return EncodingApplies();
        });
        bool ok = settingsWin_->Create(hwnd_);
        if (!ok) {
            delete settingsWin_;
            settingsWin_ = nullptr;
        }
    }

    void ToggleTheme() {
        settings_.theme = settings_.theme == L"dark" ? L"blue" : L"dark";
        settings_.dark = settings_.theme == L"dark";
        settings_.Save();
        ForEachView([this](DocView* v) { v->ApplyTheme(settings_.theme); });
        outline_.ApplyTheme(settings_.theme);
        SetDarkTitle(hwnd_, settings_.dark);
        if (settingsWin_) settingsWin_->ApplyTheme();
        InvalidateChrome();
    }

    void SetZoom(double z) {
        z = std::max(0.6, std::min(2.5, z));
        z = std::round(z * 100.0) / 100.0;
        if (std::abs(z - settings_.zoom) < 0.001) return;
        settings_.zoom = z;
        settings_.Save();
        ForEachView([z](DocView* v) { v->SetZoom(z); });
        InvalidateChrome();
    }

    void ShowOpenMenu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        std::wstring openFile = TranslateUi(L"打开文件");
        std::wstring openFolder = TranslateUi(L"打开文件夹");
        AppendMenuW(menu, MF_STRING, CMD_OPEN, openFile.c_str());
        AppendMenuW(menu, MF_STRING, CMD_OPEN_FOLDER, openFolder.c_str());
        if (workspace_.HasRoot()) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            std::wstring toggle = TranslateUi(settings_.workspaceVisible ? L"隐藏工作区" : L"显示工作区");
            std::wstring refresh = TranslateUi(L"刷新工作区");
            AppendMenuW(menu, MF_STRING, CMD_WORKSPACE, toggle.c_str());
            AppendMenuW(menu, MF_STRING, CMD_REFRESH_WORKSPACE, refresh.c_str());
            if (!ActivePath().empty()) {
                std::wstring locate = TranslateUi(L"定位当前文件");
                AppendMenuW(menu, MF_STRING, CMD_LOCATE, locate.c_str());
            }
        }
        SetMenuDefaultItem(menu, CMD_OPEN, FALSE);
        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);
        int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                                     pt.x, pt.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
        if (command) Command(command);
    }

    void OpenFileDialog() {
        wchar_t file[32768] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd_;
        std::wstring filter = TranslateUi(L"所有支持的文件") + L'\0' +
            L"*.md;*.markdown;*.mdown;*.mkd;*.mdx;*.json;*.jsonc;*.ipynb;*.csv;*.tsv;*.txt;*.log;*.yml;*.yaml;*.xml;*.ini;*.toml;*.doc;*.docx;*.xls;*.xlsx;*.pdf;*.zip;*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff;*.ico;*.webp\0" +
            L"Markdown\0*.md;*.markdown;*.mdown;*.mkd;*.mdx\0" +
            TranslateUi(L"JSON / Notebook / 对话记录") + L"\0*.json;*.jsonc;*.geojson;*.webmanifest;*.ipynb\0" +
            TranslateUi(L"表格") + L"\0*.csv;*.tsv;*.xls;*.xlsx\0" +
            L"Word\0*.doc;*.docx\0" +
            L"PDF\0*.pdf\0" +
            L"ZIP\0*.zip\0" +
            TranslateUi(L"图片") + L"\0*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff;*.ico;*.webp\0" +
            TranslateUi(L"纯文本") + L"\0*.txt;*.log;*.yml;*.yaml;*.xml;*.ini;*.toml\0" +
            TranslateUi(L"所有文件") + L"\0*.*\0" + L'\0';
        std::wstring dialogTitle = TranslateUi(L"打开文件 - LitheMD");
        ofn.lpstrFilter = filter.c_str();
        ofn.lpstrFile = file;
        ofn.nMaxFile = 32768;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
        ofn.lpstrTitle = dialogTitle.c_str();
        if (GetOpenFileNameW(&ofn)) OpenFile(file, true, true);
    }

    void OpenFolderDialog() {
        std::wstring folder = PickFolder(hwnd_, TranslateUi(L"打开文件夹 - LitheMD"));
        if (!folder.empty()) OpenWorkspace(folder, true);
    }

    void OpenWorkspace(const std::wstring& path, bool show) {
        std::wstring error;
        if (!workspace_.OpenRoot(path, &error)) {
            MessageBoxW(hwnd_, TranslateUi(error.empty() ? L"无法打开该文件夹。" : error).c_str(),
                        L"LitheMD", MB_OK | MB_ICONWARNING);
            return;
        }
        settings_.workspaceRoot = workspace_.Model().Root();
        settings_.workspaceHistory.erase(
            std::remove_if(settings_.workspaceHistory.begin(), settings_.workspaceHistory.end(),
                [&](const std::wstring& p) {
                    return _wcsicmp(p.c_str(), settings_.workspaceRoot.c_str()) == 0;
                }),
            settings_.workspaceHistory.end());
        settings_.workspaceHistory.insert(settings_.workspaceHistory.begin(), settings_.workspaceRoot);
        while (settings_.workspaceHistory.size() > Settings::MaxWorkspaceHistory)
            settings_.workspaceHistory.pop_back();
        settings_.workspaceVisible = show;
        settings_.Save();
        if (home_) home_->SetWorkspaceHistory(settings_.workspaceHistory);
        workspace_.SetActivePath(ActivePath(), false);
        LayoutChildren();
        InvalidateChrome();
    }

    void ToggleWorkspace() {
        if (settings_.workspaceVisible) {
            settings_.workspaceVisible = false;
            settings_.Save();
            LayoutChildren();
            InvalidateChrome();
            return;
        }
        if (!workspace_.HasRoot()) {
            OpenFolderDialog();
            return;
        }
        settings_.workspaceVisible = true;
        settings_.Save();
        workspace_.SetActivePath(ActivePath(), false);
        LayoutChildren();
        InvalidateChrome();
    }

    void ShowPathDialog() {
        std::wstring initial = ActivePath();
        if (initial.empty()) initial = settings_.workspaceRoot;
        bool ok = false;
        std::wstring input = ShowTextInput(hwnd_, TranslateUi(L"输入路径 - LitheMD"),
                                           TranslateUi(L"输入文件、文件夹或 ZIP 路径，支持 %USERPROFILE% 和相对路径。"),
                                           initial, false, settings_.dark, &ok);
        if (!ok) return;
        std::wstring base = ActivePath().empty() ? settings_.workspaceRoot : PathDir(ActivePath());
        std::wstring path = ResolveUserPath(input, base);
        if (path.empty()) return;
        if (DirExists(path)) {
            OpenWorkspace(path, true);
        } else if (FileExists(path)) {
            if (IsArchivePath(path)) OpenArchive(path);
            else OpenFile(path, true, true);
        } else {
            MessageBoxW(hwnd_, TranslateUi(L"路径不存在或无法访问。").c_str(), L"LitheMD",
                        MB_OK | MB_ICONWARNING);
        }
    }

    void UpdateSplitContent() {
        if (!split_ || !splitView_) return;
        std::wstring path = ActivePath();
        if (path.empty()) {
            splitView_->ClearDocument();
            return;
        }
        if (_wcsicmp(splitPath_.c_str(), path.c_str()) == 0) return;
        std::wstring err;
        TextEncoding encoding = TextEncoding::Auto;
        DocLanguage language = settings_.documentLanguage;
        std::unique_ptr<Document> doc = LoadDocument(path, err, encoding, language, false);
        if (doc) {
            splitView_->SetDocument(std::move(doc));
            double saved = positions_.Get(path);
            if (saved >= 0) splitView_->RestoreScrollFraction(saved);
        } else {
            splitView_->SetError(err.empty() ? L"无法打开该文件" : err, path, L"");
        }
        splitPath_ = path;
    }

    void ToggleSplit() {
        if (active_ < 0 || !ActiveView() || !ActiveView()->Doc()) return;
        split_ = !split_;
        if (split_ && !splitView_) {
            auto view = std::make_unique<DocView>();
            view->SetSettings(&settings_);
            if (!view->Create(hwnd_, 300)) {
                split_ = false;
                return;
            }
            view->ApplyTheme(settings_.theme);
            view->SetWheelStep(settings_.wheelStep);
            view->SetZoom(settings_.zoom);
            view->SetFontScale(settings_.fontScale);
            view->SetContentWidth(settings_.contentWidth);
            WireView(view.get(), false);
            splitView_ = std::move(view);
        }
        if (split_) {
            splitPath_.clear();
            UpdateSplitContent();
        } else if (splitView_) {
            splitView_->ClearDocument();
            splitPath_.clear();
        }
        LayoutChildren();
        InvalidateChrome();
    }

    void TogglePresentation() {
        if (active_ < 0 || !ActiveView() || !ActiveView()->Doc()) return;
        presentation_ = !presentation_;
        if (presentation_) {
            savedStyle_ = (DWORD)GetWindowLongPtrW(hwnd_, GWL_STYLE);
            savedPlacement_.length = sizeof(savedPlacement_);
            placementSaved_ = GetWindowPlacement(hwnd_, &savedPlacement_) != FALSE;
            MONITORINFO mi{ sizeof(mi) };
            GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(hwnd_, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        } else {
            SetWindowLongPtrW(hwnd_, GWL_STYLE, savedStyle_);
            if (placementSaved_) SetWindowPlacement(hwnd_, &savedPlacement_);
            SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        SetDarkTitle(hwnd_, settings_.dark);
        LayoutChildren();
        InvalidateChrome();
        if (DocView* v = ActiveView()) v->FocusView();
    }

    void OpenColorPicker(DocView* view, int presetIndex = -1) {
        if (!view) return;
        if (presetIndex < 0) {
            int position = view->CurrentInkColorIndex();
            if (position >= 0 && position < (int)settings_.inkToolbarPresets.size())
                presetIndex = settings_.inkToolbarPresets[(size_t)position];
        }
        if (settings_.inkPresets.empty()) settings_.inkPresets = DefaultInkPresets();
        if (presetIndex < 0 || presetIndex >= (int)settings_.inkPresets.size()) presetIndex = 0;
        Color initial = settings_.inkPresets[(size_t)presetIndex];
        auto* win = new ColorPickerWindow(hwnd_, &settings_, presetIndex, initial,
            [this] {
                ForEachView([this](DocView* v) { v->SetSettings(&settings_); });
                InvalidateChrome();
            });
        if (!win->Create()) delete win;
    }

    void NewCanvas() {
        int w = std::clamp(settings_.canvasWidth, Settings::MinCanvasSize, Settings::MaxCanvasSize);
        int h = std::clamp(settings_.canvasHeight, Settings::MinCanvasSize, Settings::MaxCanvasSize);
        static const unsigned char whitePng[] = {
            0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
            0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,
            0x89,0x00,0x00,0x00,0x0C,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0xF8,0xFF,0xFF,0x3F,
            0x00,0x05,0xFE,0x02,0xFE,0x0D,0xEF,0x46,0xB8,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,
            0x44,0xAE,0x42,0x60,0x82
        };
        static const unsigned char blackPng[] = {
            0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
            0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,
            0x89,0x00,0x00,0x00,0x0C,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0x60,0x60,0x60,0x00,
            0x00,0x00,0x04,0x00,0x01,0xF6,0x17,0x38,0x55,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,
            0x44,0xAE,0x42,0x60,0x82
        };
        auto doc = std::make_unique<Document>();
        doc->kind = DocKind::Image;
        doc->title = L"画布 " + std::to_wstring(w) + L"×" + std::to_wstring(h);
        doc->subtitle = settings_.canvasDark ? L"黑色画布" : L"白色画布";
        doc->image = std::make_shared<ImageRef>();
        const unsigned char* data = settings_.canvasDark ? blackPng : whitePng;
        size_t size = settings_.canvasDark ? sizeof(blackPng) : sizeof(whitePng);
        doc->image->bytes.assign(data, data + size);
        doc->image->mime = L"image/png";
        doc->image->cacheKey = std::wstring(L"canvas:") + (settings_.canvasDark ? L"dark:" : L"light:") +
                               std::to_wstring(w) + L"x" + std::to_wstring(h);
        doc->imageW = w;
        doc->imageH = h;
        doc->sourceAllowed = false;
        doc->sourceDefault = false;

        auto view = std::make_unique<DocView>();
        view->SetSettings(&settings_);
        if (!view->Create(hwnd_, 200 + (int)tabs_.size())) {
            MessageBoxW(hwnd_, TranslateUi(L"无法创建画布窗口。").c_str(), L"LitheMD", MB_OK | MB_ICONWARNING);
            return;
        }
        view->ApplyTheme(settings_.theme);
        view->SetWheelStep(settings_.wheelStep);
        view->SetZoom(settings_.zoom);
        view->SetFontScale(settings_.fontScale);
        view->SetContentWidth(settings_.contentWidth);
        WireView(view.get(), false);
        view->SetDocument(std::move(doc));
        Tab tab;
        tab.title = L"画布 " + std::to_wstring(w) + L"×" + std::to_wstring(h);
        tab.view = std::move(view);
        tabs_.push_back(std::move(tab));
        SwitchTab((int)tabs_.size() - 1);
        if (DocView* v = ActiveView()) v->SetInkMode(true);
        UpdateTitle();
        InvalidateChrome();
    }

    void OpenArchive(const std::wstring& path);

    void OpenFile(const std::wstring& pathIn, bool addRecent, bool activate) {
        if (pathIn.empty()) return;
        if (loading_) {
            if (pendingOpens_.size() < 32)
                pendingOpens_.push_back({ pathIn, addRecent, activate });
            return;
        }
        std::wstring path = AbsolutePath(pathIn);
        if (DirExists(path)) {
            OpenWorkspace(path, true);
            ProcessPendingOpens();
            return;
        }
        if (IsArchivePath(path) && FileExists(path)) {
            OpenArchive(path);
            ProcessPendingOpens();
            return;
        }
        for (size_t i = 0; i < tabs_.size(); i++) {
            if (_wcsicmp(tabs_[i].path.c_str(), path.c_str()) == 0) {
                if (activate) {
                    SwitchTab((int)i);
                    Tab* current = ActiveTab();
                    if (current && _wcsicmp(current->path.c_str(), path.c_str()) == 0) {
                        uint64_t now = FileWriteTimeUtc(path);
                        if (now && now != current->fileTime) Reload(false);
                    }
                }
                ProcessPendingOpens();
                return;
            }
        }
        loading_ = true;
        HCURSOR oldCursor = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        std::wstring err;
        TextEncoding encoding = TextEncoding::Auto;
        DocLanguage language = settings_.documentLanguage;
        std::unique_ptr<Document> doc = LoadDocument(path, err, encoding, language, false);
        auto view = std::make_unique<DocView>();
        view->SetSettings(&settings_);
        if (!view->Create(hwnd_, 200 + (int)tabs_.size())) {
            loading_ = false;
            SetCursor(oldCursor);
            MessageBoxW(hwnd_, TranslateUi(L"无法创建文档窗口。").c_str(), L"LitheMD", MB_OK | MB_ICONWARNING);
            ProcessPendingOpens();
            return;
        }
        view->ApplyTheme(settings_.theme);
        view->SetWheelStep(settings_.wheelStep);
        view->SetZoom(settings_.zoom);
        view->SetFontScale(settings_.fontScale);
        view->SetContentWidth(settings_.contentWidth);
        WireView(view.get(), false);
        Tab tab;
        tab.path = path;
        tab.fileTime = FileWriteTimeUtc(path);
        tab.encoding = encoding;
        tab.language = language;
        tab.view = std::move(view);
        tab.error = !doc;
        if (doc) {
            tab.view->SetDocument(std::move(doc));
            double saved = positions_.Get(path);
            if (saved >= 0) tab.view->RestoreScrollFraction(saved);
            if (addRecent) {
                settings_.AddRecent(path);
                if (home_) home_->SetRecent(settings_.recent);
            }
        } else {
            tab.view->SetError(err.empty() ? L"无法打开该文件" : err, path, SourceTextForError(path));
        }
        tabs_.push_back(std::move(tab));
        loading_ = false;
        SetCursor(oldCursor);
        if (activate) {
            tabScroll_ = 1000000.0f;
            SwitchTab((int)tabs_.size() - 1);
            workspace_.SetActivePath(path, true);
        } else {
            LayoutChildren();
            InvalidateChrome();
        }
        ProcessPendingOpens();
    }

    void ProcessPendingOpens() {
        if (loading_) return;
        while (!loading_ && !pendingOpens_.empty()) {
            PendingOpen next = std::move(pendingOpens_.front());
            pendingOpens_.pop_front();
            OpenFile(next.path, next.addRecent, next.activate);
        }
    }

    void Reload(bool automatic, bool repairMojibake = false) {
        Tab* tab = ActiveTab();
        if (!tab || loading_) return;
        if (!FileExists(tab->path)) {
            if (!automatic) {
                tab->view->SetError(L"文件已被移动或删除。", tab->path, L"");
                tab->error = true;
                UpdateTitle();
                InvalidateChrome();
            }
            return;
        }
        if (!automatic && !ConfirmSaveNotes(tab->view.get())) return;
        loading_ = true;
        bool wasSource = tab->view->IsSource();
        std::wstring err;
        std::unique_ptr<Document> doc = LoadDocument(tab->path, err, tab->encoding, tab->language,
                                                      repairMojibake);
        if (!doc) {
            tab->view->SetError(err.empty() ? L"无法重新加载该文件" : err, tab->path, SourceTextForError(tab->path));
            tab->error = true;
        } else {
            tab->fileTime = FileWriteTimeUtc(tab->path);
            tab->view->SetDocument(std::move(doc), true, true);
            if (wasSource && tab->view->CanSource()) tab->view->SetSource(true);
            tab->error = false;
            tab->externalChange = false;
        }
        loading_ = false;
        StartWatching();
        UpdateTitle();
        UpdateOutline();
        if (split_ && _wcsicmp(splitPath_.c_str(), tab->path.c_str()) == 0) {
            splitPath_.clear();
            UpdateSplitContent();
        }
        LayoutChildren();
        InvalidateChrome();
    }

    void UpdateTitle() {
        std::wstring title = L"LitheMD";
        if (active_ >= 0 && active_ < (int)tabs_.size()) {
            std::wstring active = ActiveTitle();
            if (active == L"主页" || active.rfind(L"画布", 0) == 0)
                active = TranslateUi(active);
            title += L" — " + active;
        }
        SetWindowTextW(hwnd_, title.c_str());
    }

    std::wstring StatusLeftString() const {
        DocView* view = ActiveView();
        Document* doc = view ? view->Doc() : nullptr;
        if (active_ < 0) return L"就绪  ·  拖入文件或按 Ctrl+O 打开";
        if (!doc) {
            std::wstring title = ActiveTitle();
            return title.empty() ? L"打开失败" : title + L"  ·  打开失败";
        }
        std::wstring s = ActiveTitle();
        s += L"  ·  " + DocKindName(doc->kind);
        s += L"  ·  " + FormatBytes(doc->size);
        if (doc->kind == DocKind::Image) {
            if (doc->imageW > 0) s += L"  ·  " + std::to_wstring(doc->imageW) + L" × " + std::to_wstring(doc->imageH);
        } else {
            if (doc->lines > 0) s += L"  ·  " + std::to_wstring(doc->lines) + L" 行";
            if (!doc->encoding.empty()) {
                s += L"  ·  " + doc->encoding;
                if (doc->encodingRepaired) s += L"  ·  已修复乱码";
                else if (doc->encodingUncertain) s += L"  ·  编码待确认";
                std::wstring lang = LanguageShortName(doc->language);
                if (!lang.empty()) s += L"  ·  " + lang;
            }
        }
        switch (doc->kind) {
        case DocKind::Json: s += L"  ·  " + std::to_wstring(doc->jsonNodes) + L" 个节点"; break;
        case DocKind::Notebook:
            if (!doc->meta.empty()) s += L"  ·  " + doc->meta;
            break;
        case DocKind::Chat: s += L"  ·  " + std::to_wstring((int)doc->blocks.size()) + L" 个块"; break;
        case DocKind::Csv:
        case DocKind::Xlsx:
        case DocKind::Xls:
            s += L"  ·  " + std::to_wstring(doc->grid.sheets.size()) + L" 个工作表";
            s += L"  ·  " + std::to_wstring(doc->csvRows) + L" 行";
            break;
        case DocKind::Pdf: s += L"  ·  " + std::to_wstring(doc->pdfPages) + L" 页"; break;
        case DocKind::Docx:
        case DocKind::Doc: s += L"  ·  " + std::to_wstring(doc->docParagraphs) + L" 个段落"; break;
        default: break;
        }
        if (view && view->HasUnsavedNotes()) s += L"  ·  笔记未保存";
        if (view && !view->NotesError().empty()) s += L"  ·  笔记文件异常";
        if (active_ >= 0 && active_ < (int)tabs_.size() && tabs_[(size_t)active_].externalChange)
            s += L"  ·  文件已在外部修改";
        return s;
    }

    std::wstring StatusRightString() const {
        int pct = (int)std::lround(settings_.zoom * 100.0);
        std::wstring s = std::to_wstring(pct) + L"%";
        DocView* view = ActiveView();
        Document* doc = view ? view->Doc() : nullptr;
        if (doc) {
            if (view->IsSource()) s += L"  ·  源码";
            else if (doc->kind == DocKind::Json) s += L"  ·  JSON 树";
            else if (doc->kind == DocKind::Csv || doc->kind == DocKind::Xlsx || doc->kind == DocKind::Xls)
                s += L"  ·  可拖动列宽 / 行高";
            else if (doc->kind == DocKind::Pdf) s += L"  ·  PDF 页面";
            else if (doc->kind == DocKind::Image) s += L"  ·  图片视图";
            if (view->InkMode()) s += L"  ·  画笔注释";
            if (doc->renderMs >= 0.5)
                s += L"  ·  渲染 " + std::to_wstring((int)std::lround(doc->renderMs)) + L" ms";
        }
        return s;
    }

    void BuildButtons(float widthDip) {
        buttons_.clear();
        float y = 9, h = 40;
        auto add = [&](int cmd, float w, std::wstring text, std::wstring glyph = L"",
                       bool iconTop = false, bool active = false, bool enabled = true) {
            HeaderBtn b;
            b.cmd = cmd;
            b.rect = { 0, y, w, h };
            b.text = std::move(text);
            b.glyph = std::move(glyph);
            b.iconTop = iconTop;
            b.active = active;
            b.enabled = enabled;
            buttons_.push_back(std::move(b));
        };
        add(CMD_OPEN_MENU, 78, L"打开", L"\uE838", false, false, true);
        buttons_.back().dropdown = true;
        add(CMD_RELOAD, 46, L"刷新", L"\uE72C", true, false, active_ >= 0 && !ActivePath().empty());
        std::vector<HeaderBtn> right;
        auto addRight = [&](int cmd, float w, std::wstring text, std::wstring glyph = L"",
                            bool iconTop = false, bool active = false, bool enabled = true,
                            int priority = 50) {
            HeaderBtn b;
            b.cmd = cmd;
            b.rect = { 0, y, w, h };
            b.text = std::move(text);
            b.glyph = std::move(glyph);
            b.iconTop = iconTop;
            b.active = active;
            b.enabled = enabled;
            b.priority = priority;
            right.push_back(std::move(b));
        };
        DocView* view = ActiveView();
        Document* doc = view ? view->Doc() : nullptr;
        bool json = doc && doc->kind == DocKind::Json && !view->IsSource();
        if (json) {
            addRight(CMD_JSON_COLLAPSE, 48, L"折叠", L"", false, false, true, 85);
            addRight(CMD_JSON_EXPAND, 48, L"展开", L"", false, false, true, 84);
        }
        if (view && view->CanSource() && doc && doc->kind != DocKind::Text) {
            addRight(CMD_SOURCE, 46, L"源码", L"", false, view->IsSource(), true, 55);
            addRight(CMD_PREVIEW, 46, L"预览", L"", false, !view->IsSource(), true, 55);
        }
        addRight(CMD_ZOOM_OUT, 30, L"A−", L"", false, false, true, 90);
        addRight(CMD_ZOOM_RESET, 42, std::to_wstring((int)std::lround(settings_.zoom * 100)) + L"%",
                 L"", false, false, true, 90);
        addRight(CMD_ZOOM_IN, 30, L"A+", L"", false, false, true, 90);
        bool canInk = active_ < 0 || (view && (view->CanAnnotate() || !view->Doc()));
        addRight(CMD_INK, 46, L"画笔", L"\uE70F", true, canInk && view && view->InkMode(), canInk, 50);
        addRight(CMD_TOC, 46, L"大纲", L"\uE8FD", true, outlineWanted_,
                 active_ >= 0 && view && view->CanOutline(), 80);
        addRight(CMD_SAVE_NOTES, 50, L"保存", L"\uE74E", true, false,
                 view && view->CanSaveNotes() && view->HasUnsavedNotes(), 20);
        addRight(CMD_EXPORT, 50, L"导出", L"", false, false,
                 active_ >= 0 && view && view->Doc(), 25);
        addRight(CMD_THEME, 46, settings_.dark ? L"浅色" : L"深色",
                 settings_.dark ? L"\uE706" : L"\uE708", true, false, true, 40);
        addRight(CMD_SETTINGS, 46, L"设置", L"\uE713", true, false, true, 10);

        // Keep the title readable and avoid overlapping controls on narrow
        // windows. Lower-priority buttons disappear first; save/settings stay.
        float minTitle = 100.0f;
        float available = std::max(0.0f, widthDip - 14.0f - 252.0f - minTitle);
        auto totalWidth = [&]() {
            float total = 0;
            for (auto& b : right) total += b.rect.w + 4;
            return total;
        };
        while (!right.empty() && totalWidth() > available) {
            int drop = -1;
            int worstPriority = -1;
            for (int i = 0; i < (int)right.size(); i++) {
                if (right[(size_t)i].cmd == CMD_SAVE_NOTES ||
                    right[(size_t)i].cmd == CMD_SETTINGS) continue;
                if (right[(size_t)i].priority > worstPriority) {
                    worstPriority = right[(size_t)i].priority;
                    drop = i;
                }
            }
            if (drop < 0) break;
            right.erase(right.begin() + drop);
        }

        float rx = widthDip - 14;
        for (auto it = right.rbegin(); it != right.rend(); ++it) {
            rx -= it->rect.w;
            it->rect.x = rx;
            rx -= 4;
        }
        for (auto& b : right) buttons_.push_back(b);
        for (auto& b : buttons_) {
            if (b.cmd == CMD_OPEN_MENU) b.rect.x = 118;
            else if (b.cmd == CMD_RELOAD) b.rect.x = 200;
        }
    }

    void DrawButton(Canvas& c, const HeaderBtn& b) {
        auto& p = c.palette();
        bool hover = hoverCmd_ == b.cmd && b.enabled;
        if (b.active) c.FillRound(b.rect.x, b.rect.y, b.rect.w, b.rect.h, 6, p.accentSoft);
        else if (hover) c.FillRound(b.rect.x, b.rect.y, b.rect.w, b.rect.h, 6, p.hover);
        Color fg = !b.enabled ? p.muted : (b.active ? p.accent : p.fg);
        Color sub = p.muted;
        if (!b.enabled) {
            fg.a = 0.45f;
            sub.a = 0.45f;
        }
        if (b.iconTop) {
            c.DrawIcon(b.glyph, b.rect.x, b.rect.y + 3, b.rect.w, 22, 15, fg);
            c.DrawText(b.text, b.rect.x, b.rect.y + 23, b.rect.w, 14, 9.0f, sub, false, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
        } else if (!b.glyph.empty()) {
            c.DrawIcon(b.glyph, b.rect.x + 7, b.rect.y, 24, b.rect.h, 15, fg);
            c.DrawText(b.text, b.rect.x + 31, b.rect.y, b.rect.w - 34, b.rect.h, 12.5f, fg, false, false,
                       DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        } else {
            c.DrawText(b.text, b.rect.x, b.rect.y, b.rect.w, b.rect.h, 12.0f, fg, b.active, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        }
        if (b.dropdown) {
            float aw = 12.0f;
            float ax = b.rect.x + b.rect.w - aw - 3;
            float ay = b.iconTop ? b.rect.y + b.rect.h - 15 : b.rect.y + (b.rect.h - aw) / 2;
            c.DrawIcon(L"\uE70D", ax, ay, aw, aw, b.iconTop ? 7.0f : 8.0f, sub);
        }
    }

    void UpdateTabTooltip(int index) {
        if (!tooltip_) return;
        if (index < 0) {
            SendMessageW(tooltip_, TTM_POP, 0, 0);
            return;
        }
        const TabHit* hit = nullptr;
        for (auto& h : tabHits_) {
            if (h.index == index) { hit = &h; break; }
        }
        if (!hit) {
            SendMessageW(tooltip_, TTM_POP, 0, 0);
            return;
        }
        std::wstring text;
        if (hit->home) text = L"主页";
        else if (index >= 0 && index < (int)tabs_.size()) text = tabs_[(size_t)index].path;
        TOOLINFOW ti{};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = hwnd_;
        ti.uId = 1;
        ti.rect.left = (LONG)hit->rect.x;
        ti.rect.top = (LONG)hit->rect.y;
        ti.rect.right = (LONG)(hit->rect.x + hit->rect.w);
        ti.rect.bottom = (LONG)(hit->rect.y + hit->rect.h);
        ti.lpszText = text.empty() ? const_cast<wchar_t*>(L"") : &text[0];
        SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
        SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
        if (text.empty()) SendMessageW(tooltip_, TTM_POP, 0, 0);
    }

    void DrawTabs(Canvas& c, float viewW) {
        auto& p = c.palette();
        float y = (float)kHeaderH;
        c.FillRect(0, y, viewW, kTabH, p.surface);
        c.FillRect(0, y + kTabH - 1, viewW, 1, p.border);
        tabHits_.clear();
        float x = 8 - tabScroll_;
        auto draw = [&](int index, bool home, bool active, const std::wstring& title) {
            float tw = home ? 78.0f : Clampf(c.MeasureText(title, 12.0f, false, false, 400, false) + 44.0f, 108.0f, 210.0f);
            DRect r{ x, y + 4, tw, kTabH - 8 };
            if (r.x + r.w > 0 && r.x < viewW) {
                bool hover = hoverTab_ == index;
                if (active) c.FillRound(r.x, r.y, r.w, r.h, 5, p.bg);
                else if (hover) c.FillRound(r.x, r.y, r.w, r.h, 5, p.hover);
                if (active) c.StrokeRound(r.x, r.y, r.w, r.h, 5, p.border);
                float textW = r.w - (home ? 22.0f : 30.0f);
                c.DrawText(FitText(c, title, 12.0f, active, textW), r.x + 10, r.y, textW, r.h, 12.0f,
                           active ? p.fg : p.muted, active, false,
                           DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
                DRect closeR{};
                if (!home) {
                    closeR = { r.x + r.w - 24, r.y + (r.h - 20) / 2, 20, 20 };
                    bool ch = hoverTab_ == index && hoverTabClose_;
                    if (ch) c.FillRound(closeR.x, closeR.y, closeR.w, closeR.h, 4, p.hover);
                    c.DrawText(L"×", closeR.x, closeR.y - 1, closeR.w, closeR.h, 14.0f,
                               ch ? p.fg : p.muted, false, false,
                               DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
                }
                TabHit hit;
                hit.index = index;
                hit.home = home;
                hit.rect = r;
                hit.closeRect = closeR;
                tabHits_.push_back(hit);
            }
            x += tw + 4;
        };
        draw(-1, true, active_ < 0, L"主页");
        for (size_t i = 0; i < tabs_.size(); i++)
            draw((int)i, false, active_ == (int)i, TabTitle(tabs_[i]));
        tabTotal_ = x + tabScroll_ - 8;
        float maxScroll = std::max(0.0f, tabTotal_ - (viewW - 16.0f));
        if (tabScroll_ < 0) tabScroll_ = 0;
        if (tabScroll_ > maxScroll) tabScroll_ = maxScroll;
    }

    void PaintChrome() {
        if (!chromeCanvas_) chromeCanvas_ = std::make_unique<Canvas>();
        Canvas& c = *chromeCanvas_;
        if (!c.Begin(hwnd_, Palette::Get(settings_.theme), true)) return;
        c.SetTranslateUi(true);
        auto& p = c.palette();
        float w = c.widthDip(), h = c.heightDip();
        if (presentation_) {
            c.FillRect(0, 0, w, h, p.bg);
            c.End();
            return;
        }
        c.FillRect(0, 0, w, kHeaderH, p.bg);
        c.FillRect(0, kHeaderH - 1, w, 1, p.border);
        c.FillRound(14, 14, 28, 28, 7, p.accent);
        c.DrawText(L"md", 14, 14, 28, 28, 12, HexColor(L"#FFFFFF"), true, false,
                   DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        c.DrawText(L"LitheMD", 50, 15, 70, 24, 15, p.fg, true);
        BuildButtons(w);
        float rightMin = w;
        for (auto& b : buttons_) if (b.rect.x > 250) rightMin = std::min(rightMin, b.rect.x);
        float titleX = 252;
        float titleW = std::max(0.0f, rightMin - titleX - 14);
        if (titleW > 80) {
            std::wstring title = L"LitheMD";
            std::wstring sub = L"轻量本地文档阅读器";
            DocView* view = ActiveView();
            Document* doc = view ? view->Doc() : nullptr;
            if (active_ >= 0) {
                title = ActiveTitle();
                sub = doc ? (DocKindName(doc->kind) + L"  ·  " + FormatBytes(doc->size)) : L"打开失败";
                if (doc && view->IsSource()) sub += L"  ·  源码";
            }
            DWRITE_TEXT_ALIGNMENT ta = titleW >= 360 ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING;
            float tx = titleX + (ta == DWRITE_TEXT_ALIGNMENT_LEADING ? 8 : 0);
            float tw = titleW - (ta == DWRITE_TEXT_ALIGNMENT_LEADING ? 8 : 0);
            float ty = titleW < 260 ? 20 : 10;
            c.DrawText(FitText(c, title, 14, true, tw), tx, ty, tw, 24, 14, p.fg, true, false,
                       ta, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
            if (titleW >= 300)
                c.DrawText(FitText(c, sub, 10.5f, false, tw), tx, ty + 21, tw, 17, 10.5f, p.muted, false, false,
                           ta, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, false);
        }
        for (auto& b : buttons_) DrawButton(c, b);
        DrawTabs(c, w);

        float statusY = h - kStatusH;
        c.FillRect(0, statusY, w, kStatusH, p.bg);
        c.FillRect(0, statusY, w, 1, p.border);
        std::wstring right = StatusRightString();
        float rightW = c.MeasureText(right, 11.0f, false, false, 100000.0f, false);
        float avail = std::max(40.0f, w - rightW - 60);
        std::wstring left = StatusLeftString();
        c.DrawText(FitText(c, left, 11.0f, false, avail), 16, statusY + 1, avail, kStatusH - 2, 11.0f, p.muted, false, false,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        if (rightW < w - 100)
            c.DrawText(right, w - rightW - 16, statusY + 1, rightW + 2, kStatusH - 2, 11.0f, p.muted, false, false,
                       DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        c.End();
    }

    void scrollTabs(float delta) {
        float maxScroll = std::max(0.0f, tabTotal_ - 200.0f);
        float before = tabScroll_;
        tabScroll_ = Clampf(tabScroll_ + delta, 0, maxScroll);
        if (tabScroll_ != before) InvalidateChrome();
    }

    void InvalidateChrome() {
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT_PTR resizeTimer_ = 0;
    std::unique_ptr<Canvas> chromeCanvas_;
    Settings settings_;
    std::vector<std::wstring> initialFiles_;
    bool openSettings_ = false;
    bool loading_ = false;
    std::deque<PendingOpen> pendingOpens_;
    bool outlineWanted_ = false;
    std::unique_ptr<DocView> home_;
    std::vector<Tab> tabs_;
    int active_ = -1;
    OutlineView outline_;
    WorkspaceView workspace_;
    FileWatcher watcher_;
    ReadingPositions positions_;
    std::wstring positionsPath_;
    bool split_ = false;
    std::unique_ptr<DocView> splitView_;
    std::wstring splitPath_;
    bool presentation_ = false;
    DWORD savedStyle_ = 0;
    WINDOWPLACEMENT savedPlacement_{};
    bool placementSaved_ = false;
    SettingsWindow* settingsWin_ = nullptr;
    std::vector<HeaderBtn> buttons_;
    std::vector<TabHit> tabHits_;
    HWND tooltip_ = nullptr;
    std::vector<ActiveShortcut> shortcuts_;
    int hoverCmd_ = 0;
    int hoverTab_ = -2;
    bool hoverTabClose_ = false;
    float tabScroll_ = 0;
    float tabTotal_ = 0;
};

void MainWindow::OpenArchive(const std::wstring& path) {
    auto* window = new ArchiveWindow(&settings_, path, [this](const std::wstring& temp,
                                                              const std::wstring& display) {
        OpenFile(temp, false, true);
        if (Tab* tab = ActiveTab(); tab && _wcsicmp(tab->path.c_str(), temp.c_str()) == 0) {
            tab->title = display;
            UpdateTitle();
            InvalidateChrome();
        }
    });
    if (!window->Create(hwnd_)) delete window;
}

} // namespace

int RunApp(HINSTANCE inst, std::vector<std::wstring> initialFiles, bool openSettings) {
    Engine::Instance().Init();
    RemoveTree(PathJoin(LocalDataDir(), L"preview"));
    Settings settings = Settings::Load();
    SetUiLanguage(settings.uiLanguage);
    MainWindow window(std::move(settings), std::move(initialFiles), openSettings);
    if (!window.Create(inst)) {
        MessageBoxW(nullptr, TranslateUi(L"无法创建 LitheMD 窗口。").c_str(), L"LitheMD", MB_OK | MB_ICONERROR);
        return 1;
    }
    return window.Run();
}
