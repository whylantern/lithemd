#include "archive_window.h"
#include "view.h"
#include "render.h"
#include "dialogs.h"
#include "path_util.h"
#include "i18n.h"
#include <algorithm>
#include <cmath>
#include <memory>
#include <windowsx.h>
#include <dwmapi.h>

namespace {

constexpr int kToolbarH = 108;
constexpr int kHeaderRowH = 26;
constexpr int kRowH = 28;
constexpr int kStatusH = 30;
constexpr UINT kArchiveProgress = WM_APP + 21;
constexpr UINT kArchiveDone = WM_APP + 22;

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

std::wstring EntryTime(uint64_t dosTime) {
    uint16_t time = (uint16_t)(dosTime & 0xFFFF);
    uint16_t date = (uint16_t)((dosTime >> 16) & 0xFFFF);
    int year = 1980 + (date >> 9);
    int month = (date >> 5) & 0x0F;
    int day = date & 0x1F;
    int hour = time >> 11;
    int minute = (time >> 5) & 0x3F;
    if (month < 1 || month > 12 || day < 1 || day > 31) return L"";
    wchar_t buf[40];
    swprintf(buf, 40, L"%04d-%02d-%02d %02d:%02d", year, month, day, hour, minute);
    return buf;
}

std::wstring SafeFileName(const std::wstring& name) {
    std::wstring base = PathName(name);
    if (base.empty()) base = L"entry";
    for (auto& c : base) {
        if (c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' || c == L'\\' ||
            c == L'|' || c == L'?' || c == L'*' || c < 32)
            c = L'_';
    }
    while (!base.empty() && (base.back() == L'.' || base.back() == L' ')) base.pop_back();
    if (base.empty()) base = L"entry";
    return base;
}

struct ExtractJob {
    HWND hwnd = nullptr;
    std::shared_ptr<ArchiveReader> reader;
    std::wstring dest;
    std::wstring password;
    bool overwrite = false;
};

struct ExtractResult {
    bool ok = false;
    std::wstring error;
};

DWORD WINAPI ExtractThread(LPVOID param) {
    std::unique_ptr<ExtractJob> job((ExtractJob*)param);
    uint64_t last = 0;
    std::wstring error;
    auto progress = [&](size_t index, const std::wstring&) {
        uint64_t now = GetTickCount64();
        if (now - last > 120 || index + 1 == job->reader->Entries().size()) {
            last = now;
            if (IsWindow(job->hwnd))
                PostMessageW(job->hwnd, kArchiveProgress, (WPARAM)(index + 1), 0);
        }
        return true;
    };
    bool ok = job->reader->ExtractAll(job->dest, job->password, progress, job->overwrite, &error);
    auto* result = new ExtractResult{ ok, std::move(error) };
    if (IsWindow(job->hwnd)) {
        if (!PostMessageW(job->hwnd, kArchiveDone, 0, (LPARAM)result)) delete result;
    } else {
        delete result;
    }
    return 0;
}

} // namespace

ArchiveWindow::ArchiveWindow(Settings* settings, std::wstring path,
                             std::function<void(const std::wstring&, const std::wstring&)> previewFile)
    : settings_(settings), path_(std::move(path)), previewFile_(std::move(previewFile)) {}

ArchiveWindow::~ArchiveWindow() = default;

bool ArchiveWindow::Create(HWND owner) {
    owner_ = owner;
    reader_ = std::make_shared<ArchiveReader>();
    std::wstring error;
    if (!reader_->Open(path_, &error)) {
        MessageBoxW(owner, TranslateUi(error.empty() ? L"无法打开压缩包。" : error).c_str(),
                    L"LitheMD", MB_OK | MB_ICONWARNING);
        return false;
    }
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrW(owner, GWLP_HINSTANCE);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"LitheMdArchive";
    RegisterClassExW(&wc);
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    UINT dpi = GetDpiForWindow(owner);
    if (!dpi) dpi = 96;
    float sc = dpi / 96.0f;
    RECT rc{ 0, 0, (LONG)std::lround(940 * sc), (LONG)std::lround(660 * sc) };
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    RECT work{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        ww = std::min(ww, (int)(work.right - work.left - 40));
        wh = std::min(wh, (int)(work.bottom - work.top - 40));
    }
    RECT orc{};
    GetWindowRect(owner, &orc);
    int x = orc.left + ((orc.right - orc.left) - ww) / 2;
    int y = orc.top + ((orc.bottom - orc.top) - wh) / 2;
    std::wstring title = PathName(path_) + L" - LitheMD";
    hwnd_ = CreateWindowExW(0, L"LitheMdArchive", title.c_str(), style, x, y, ww, wh,
                            owner, nullptr, inst, this);
    if (!hwnd_) return false;
    if (settings_ && settings_->dark) {
        BOOL value = TRUE;
        DwmSetWindowAttribute(hwnd_, 20, &value, sizeof(value));
        DwmSetWindowAttribute(hwnd_, 19, &value, sizeof(value));
        ApplyDarkWindowTheme(hwnd_, true);
    }
    selfDelete_ = true;
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    UpdateWindow(hwnd_);
    return true;
}

void ArchiveWindow::Activate() {
    if (hwnd_) { ShowWindow(hwnd_, SW_RESTORE); SetForegroundWindow(hwnd_); }
}

std::wstring ArchiveWindow::SelectedName() const {
    if (selected_ < 0 || !reader_ || selected_ >= (int)reader_->Entries().size()) return L"";
    return reader_->Entries()[(size_t)selected_].name;
}

void ArchiveWindow::UpdateScroll() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    float scale = GetDpiForWindow(hwnd_) / 96.0f;
    float h = rc.bottom / scale;
    viewH_ = std::max(0.0f, h - kToolbarH - kHeaderRowH - kStatusH);
    contentH_ = reader_ ? reader_->Entries().size() * kRowH : 0;
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, contentH_ - viewH_));
}

DRect ArchiveWindow::RowRect(int index) const {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    float scale = GetDpiForWindow(hwnd_) / 96.0f;
    float w = rc.right / scale;
    return { 0, kToolbarH + kHeaderRowH + index * kRowH - scroll_, w, kRowH };
}

int ArchiveWindow::HitRow(float x, float y) const {
    if (y < kToolbarH + kHeaderRowH) return -1;
    int index = (int)std::floor((y - kToolbarH - kHeaderRowH + scroll_) / kRowH);
    if (!reader_ || index < 0 || index >= (int)reader_->Entries().size()) return -1;
    return index;
}

void ArchiveWindow::LayoutButtons() {
    buttons_.clear();
    float x = 16;
    auto add = [&](int id, float w, std::wstring text, bool enabled = true, bool primary = false) {
        Button b;
        b.id = id;
        b.text = std::move(text);
        b.rect = { x, 64, w, 34 };
        b.enabled = enabled;
        b.primary = primary;
        buttons_.push_back(std::move(b));
        x += w + 8;
    };
    bool hasSelection = selected_ >= 0 && reader_ && selected_ < (int)reader_->Entries().size() &&
                        !reader_->Entries()[(size_t)selected_].isDir && !extracting_;
    add(1, 78, L"预览", hasSelection);
    add(2, 92, L"解压选中", hasSelection);
    add(3, 92, L"全部解压", reader_ && !reader_->Entries().empty() && !extracting_);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    float scale = GetDpiForWindow(hwnd_) / 96.0f;
    float w = rc.right / scale;
    Button close;
    close.id = 4;
    close.text = L"关闭";
    close.rect = { w - 84, 64, 68, 34 };
    buttons_.push_back(std::move(close));
}

void ArchiveWindow::Paint() {
    Canvas c;
    if (!c.Begin(hwnd_, Palette::Get(settings_ ? settings_->theme : L"blue"))) return;
    c.SetTranslateUi(true);
    auto& p = c.palette();
    float w = c.widthDip(), h = c.heightDip();
    c.FillRect(0, 0, w, h, p.bg);
    c.FillRect(0, 0, w, kToolbarH, p.surface);
    c.FillRect(0, kToolbarH - 1, w, 1, p.border);
    c.DrawText(FitText(c, PathName(path_), 15.0f, true, w - 48), 18, 7, w - 48, 22, 15.0f, p.fg, true, false);
    c.DrawText(FitText(c, path_, 10.5f, false, w - 48), 18, 29, w - 48, 18, 10.5f, p.muted, false, false);
    LayoutButtons();
    for (auto& b : buttons_) {
        if (!b.enabled) {
            Color border = p.border; border.a *= 0.55f;
            c.StrokeRound(b.rect.x, b.rect.y, b.rect.w, b.rect.h, 7, border);
            Color fg = p.muted; fg.a *= 0.55f;
            c.DrawText(b.text, b.rect.x, b.rect.y, b.rect.w, b.rect.h, 12, fg, false, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        } else if (b.primary) {
            c.FillRound(b.rect.x, b.rect.y, b.rect.w, b.rect.h, 7, p.accent);
            c.DrawText(b.text, b.rect.x, b.rect.y, b.rect.w, b.rect.h, 12.5f, HexColor(L"#FFFFFF"), true, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        } else {
            if (hoverButton_ == b.id) c.FillRound(b.rect.x, b.rect.y, b.rect.w, b.rect.h, 7, p.hover);
            c.StrokeRound(b.rect.x, b.rect.y, b.rect.w, b.rect.h, 7, p.border);
            c.DrawText(b.text, b.rect.x, b.rect.y, b.rect.w, b.rect.h, 12, p.fg, false, false,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }

    float hy = (float)(kToolbarH + 4);
    c.FillRect(0, kToolbarH, w, kHeaderRowH, p.surface);
    c.FillRect(0, kToolbarH + kHeaderRowH - 1, w, 1, p.border);
    c.DrawText(L"名称", 20, hy, w - 380, kHeaderRowH - 4, 10.5f, p.muted, true);
    c.DrawText(L"大小", w - 330, hy, 96, kHeaderRowH - 4, 10.5f, p.muted, true, false,
               DWRITE_TEXT_ALIGNMENT_TRAILING);
    c.DrawText(L"修改时间", w - 224, hy, 190, kHeaderRowH - 4, 10.5f, p.muted, true, false,
               DWRITE_TEXT_ALIGNMENT_TRAILING);

    UpdateScroll();
    c.PushClip(0, kToolbarH + kHeaderRowH, w, h - kToolbarH - kHeaderRowH - kStatusH);
    visibleRows_.clear();
    if (!reader_ || reader_->Entries().empty()) {
        c.DrawText(L"压缩包中没有条目。", 20, kToolbarH + kHeaderRowH + 30, w - 40, 30, 12.5f, p.muted, false, false,
                   DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        for (size_t i = 0; i < reader_->Entries().size(); i++) {
            DRect r = RowRect((int)i);
            if (r.y + kRowH < kToolbarH + kHeaderRowH || r.y > h - kStatusH) continue;
            visibleRows_.push_back((int)i);
            const ArchiveEntry& e = reader_->Entries()[i];
            if ((int)i == selected_) c.FillRect(4, r.y, w - 12, kRowH, p.accentSoft);
            else if (hover_ == (int)i) c.FillRect(4, r.y, w - 12, kRowH, p.hover);
            std::wstring name = e.name;
            if (!e.isDir && e.encrypted) name = (e.aes ? L"[AES] " : L"[密] ") + name;
            if (e.isDir && !name.empty() && name.back() != L'\\' && name.back() != L'/') name += L'\\';
            Color nameColor = e.isDir ? p.accent : p.fg;
            c.DrawText(FitText(c, name, 12.0f, e.isDir, w - 390), 20, r.y, w - 390, kRowH, 12.0f,
                       nameColor, e.isDir, false, DWRITE_TEXT_ALIGNMENT_LEADING,
                       DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
            if (!e.isDir)
                c.DrawText(FormatBytes(e.size), w - 330, r.y, 96, kRowH, 11.0f, p.muted, false, false,
                           DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
            c.DrawText(EntryTime(e.dosTime), w - 224, r.y, 190, kRowH, 10.5f, p.muted, false, false,
                       DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        }
    }
    c.PopClip();

    if (contentH_ > viewH_ + 0.5f) {
        float trackX = w - 4;
        float thumbH = std::max(28.0f, viewH_ * viewH_ / contentH_);
        float maxScroll = std::max(0.0f, contentH_ - viewH_);
        float thumbY = kToolbarH + kHeaderRowH +
                       (maxScroll > 0 ? scroll_ / maxScroll * (viewH_ - thumbH) : 0);
        Color track = p.border; track.a = 0.4f;
        c.FillRound(trackX, kToolbarH + kHeaderRowH, 2, viewH_, 1, track);
        Color thumb = p.muted; thumb.a = 0.55f;
        c.FillRound(trackX, thumbY, 2, thumbH, 1, thumb);
    }

    float statusY = h - kStatusH;
    c.FillRect(0, statusY, w, kStatusH, p.surface);
    c.FillRect(0, statusY, w, 1, p.border);
    if (extracting_) {
        float barW = std::min(220.0f, w * 0.3f);
        c.FillRound(16, statusY + 12, barW, 6, 3, p.border);
        float frac = progressTotal_ > 0 ? std::min(1.0f, (float)progress_ / progressTotal_) : 0.0f;
        c.FillRound(16, statusY + 12, barW * frac, 6, 3, p.accent);
        c.DrawText(L"正在解压 " + std::to_wstring(progress_) + L" / " + std::to_wstring(progressTotal_),
                   16 + barW + 12, statusY, 260, kStatusH, 11.0f, p.muted, false, false,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else {
        std::wstring status = notice_;
        if (status.empty()) {
            size_t files = 0, dirs = 0, locked = 0;
            if (reader_) {
                for (auto& e : reader_->Entries()) {
                    if (e.isDir) dirs++;
                    else files++;
                    if (e.encrypted) locked++;
                }
            }
            status = std::to_wstring(files) + L" 个文件";
            if (dirs) status += L" · " + std::to_wstring(dirs) + L" 个文件夹";
            if (reader_) status += L" · " + FormatBytes(reader_->TotalSize());
            if (locked) status += L" · " + std::to_wstring(locked) + L" 个加密条目";
        }
        c.DrawText(FitText(c, status, 11.0f, false, w - 32), 16, statusY, w - 32, kStatusH, 11.0f,
                   notice_.empty() ? p.muted : p.fg, false, false,
                   DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    }
    c.End();
}

bool ArchiveWindow::EnsurePassword() {
    if (!reader_) return false;
    if (password_.empty() && reader_->HasEncrypted()) {
        bool ok = false;
        std::wstring value = ShowTextInput(hwnd_, TranslateUi(L"压缩包密码"),
                                           TranslateUi(L"输入密码后可以预览或解压加密条目。"),
                                           L"", true, settings_ && settings_->dark, &ok);
        if (!ok || value.empty()) return false;
        password_ = value;
    }
    return true;
}

bool ArchiveWindow::WritePreview(const ArchiveEntry& entry, std::wstring& tempPath, std::wstring& error) {
    if (!reader_) return false;
    std::vector<uint8_t> data;
    if (!reader_->Read(entry.index, password_, data, &error)) return false;
    uint64_t hash = 1469598103934665603ull;
    for (wchar_t c : reader_->Path()) {
        hash ^= (uint64_t)c;
        hash *= 1099511628211ull;
    }
    wchar_t key[40]{};
    swprintf(key, 40, L"%016llX", (unsigned long long)hash);
    std::wstring dir = PathJoin(PathJoin(LocalDataDir(), L"preview"), key);
    if (!EnsureDir(dir)) { error = L"无法创建预览临时目录。"; return false; }
    tempPath = PathJoin(dir, std::to_wstring(entry.index) + L"-" + SafeFileName(entry.name));
    if (!WriteFileBytes(tempPath, data, &error)) return false;
    return true;
}

void ArchiveWindow::PreviewSelected() {
    if (extracting_ || !reader_) return;
    if (selected_ < 0 || selected_ >= (int)reader_->Entries().size()) return;
    const ArchiveEntry entry = reader_->Entries()[(size_t)selected_];
    if (entry.isDir) {
        notice_ = L"这是文件夹条目，请选择文件。";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (entry.size > 64ull * 1024 * 1024) {
        notice_ = L"条目超过 64 MB，请先解压后再查看。";
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (entry.encrypted && !EnsurePassword()) return;
    std::wstring temp, error;
    if (!WritePreview(entry, temp, error)) {
        if (error.find(L"密码") != std::wstring::npos) password_.clear();
        notice_ = error.empty() ? L"无法预览该条目。" : error;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    notice_ = L"已提取预览：" + entry.name;
    InvalidateRect(hwnd_, nullptr, FALSE);
    if (previewFile_) previewFile_(temp, entry.name);
}

void ArchiveWindow::ExtractSelected() {
    if (extracting_ || !reader_) return;
    if (selected_ < 0 || selected_ >= (int)reader_->Entries().size()) return;
    const ArchiveEntry entry = reader_->Entries()[(size_t)selected_];
    if (entry.isDir) return;
    if (entry.size > 512ull * 1024 * 1024) { notice_ = L"条目过大，已停止解压。"; return; }
    if (entry.encrypted && !EnsurePassword()) return;
    std::wstring dest = PickFolder(hwnd_, TranslateUi(L"选择解压位置"));
    if (dest.empty()) return;
    std::wstring target = PathJoin(dest, SafeFileName(entry.name));
    if (FileExists(target)) {
        if (MessageBoxW(hwnd_, TranslateUi(L"目标位置已有同名文件，是否覆盖？").c_str(),
                        L"LitheMD", MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }
    std::vector<uint8_t> data;
    std::wstring error;
    if (!reader_->Read((size_t)selected_, password_, data, &error)) {
        if (error.find(L"密码") != std::wstring::npos) password_.clear();
        notice_ = error.empty() ? L"解压失败。" : error;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    std::wstring part = target + L".lithemd-part";
    if (!WriteFileBytes(part, data, &error) ||
        !MoveFileExW(part.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(part.c_str());
        notice_ = L"无法写入目标文件。";
    } else {
        notice_ = L"已解压：" + target;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ArchiveWindow::ExtractAll() {
    if (extracting_ || !reader_ || reader_->Entries().empty()) return;
    if (reader_->HasEncrypted() && !EnsurePassword()) return;
    std::wstring dest = PickFolder(hwnd_, TranslateUi(L"选择解压位置"));
    if (dest.empty()) return;
    size_t existing = 0;
    for (auto& e : reader_->Entries()) {
        if (e.isDir) continue;
        std::wstring rel = e.name;
        for (auto& c : rel) if (c == L'/') c = L'\\';
        if (FileExists(PathJoin(dest, rel))) existing++;
    }
    bool overwrite = false;
    if (existing > 0) {
        int answer = MessageBoxW(hwnd_,
            TranslateUi(L"目标文件夹已有同名文件。\n\n覆盖？").c_str(),
            L"LitheMD", MB_YESNO | MB_ICONWARNING);
        if (answer != IDYES) return;
        overwrite = true;
    }
    StartExtraction(dest, overwrite);
}

void ArchiveWindow::StartExtraction(const std::wstring& dest, bool overwrite) {
    if (!reader_ || extracting_) return;
    extracting_ = true;
    progress_ = 0;
    progressTotal_ = (int)reader_->Entries().size();
    notice_.clear();
    auto* job = new ExtractJob{ hwnd_, reader_, dest, password_, overwrite };
    HANDLE thread = CreateThread(nullptr, 0, ExtractThread, job, 0, nullptr);
    if (thread) CloseHandle(thread);
    else { delete job; extracting_ = false; notice_ = L"无法启动解压线程。"; }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void ArchiveWindow::FinishExtraction(bool ok, const std::wstring& error) {
    extracting_ = false;
    if (!ok) {
        if (error.find(L"密码") != std::wstring::npos) password_.clear();
        notice_ = error.empty() ? L"解压失败。" : error;
        MessageBoxW(hwnd_, TranslateUi(notice_).c_str(), L"LitheMD", MB_OK | MB_ICONWARNING);
    } else {
        notice_ = L"解压完成。";
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT ArchiveWindow::Handle(UINT msg, WPARAM wp, LPARAM lp) {
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
    case WM_SIZE:
        UpdateScroll();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)lp;
        UINT dpi = GetDpiForWindow(hwnd_);
        if (!dpi) dpi = 96;
        float sc = dpi / 96.0f;
        mmi->ptMinTrackSize.x = (LONG)std::lround(620 * sc);
        mmi->ptMinTrackSize.y = (LONG)std::lround(420 * sc);
        return 0;
    }
    case WM_MOUSEMOVE: {
        float scale = GetDpiForWindow(hwnd_) / 96.0f;
        float x = GET_X_LPARAM(lp) / scale, y = GET_Y_LPARAM(lp) / scale;
        int button = 0;
        for (auto& b : buttons_)
            if (b.enabled && b.rect.Contains(x, y)) { button = b.id; break; }
        int row = HitRow(x, y);
        if (button != hoverButton_ || row != hover_) {
            hoverButton_ = button;
            hover_ = row;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        SetCursor(LoadCursorW(nullptr, (button || row >= 0) ? IDC_HAND : IDC_ARROW));
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        hover_ = -1;
        hoverButton_ = 0;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        float scale = GetDpiForWindow(hwnd_) / 96.0f;
        float x = GET_X_LPARAM(lp) / scale, y = GET_Y_LPARAM(lp) / scale;
        for (auto& b : buttons_) {
            if (!b.enabled || !b.rect.Contains(x, y)) continue;
            if (b.id == 1) PreviewSelected();
            else if (b.id == 2) ExtractSelected();
            else if (b.id == 3) ExtractAll();
            else if (b.id == 4) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            return 0;
        }
        int row = HitRow(x, y);
        if (row >= 0) {
            selected_ = row;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        float scale = GetDpiForWindow(hwnd_) / 96.0f;
        float x = GET_X_LPARAM(lp) / scale, y = GET_Y_LPARAM(lp) / scale;
        int row = HitRow(x, y);
        if (row >= 0) {
            selected_ = row;
            PreviewSelected();
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd_, &pt);
        float scale = GetDpiForWindow(hwnd_) / 96.0f;
        if (pt.y / scale < kToolbarH + kHeaderRowH) return 0;
        UpdateScroll();
        float delta = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -kRowH * 3 : kRowH * 3;
        scroll_ = std::clamp(scroll_ + delta, 0.0f, std::max(0.0f, contentH_ - viewH_));
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { PostMessageW(hwnd_, WM_CLOSE, 0, 0); return 0; }
        if (wp == VK_RETURN) { PreviewSelected(); return 0; }
        if (!reader_ || reader_->Entries().empty()) return 0;
        if (wp == VK_DOWN || wp == VK_UP || wp == VK_PRIOR || wp == VK_NEXT) {
            int count = (int)reader_->Entries().size();
            int step = wp == VK_DOWN ? 1 : (wp == VK_UP ? -1 : (wp == VK_NEXT ? 10 : -10));
            if (selected_ < 0) selected_ = 0;
            else selected_ = std::clamp(selected_ + step, 0, count - 1);
            UpdateScroll();
            float y = selected_ * kRowH;
            if (y < scroll_) scroll_ = y;
            if (y + kRowH > scroll_ + viewH_) scroll_ = y + kRowH - viewH_;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        return 0;
    case kArchiveProgress:
        progress_ = (int)wp;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case kArchiveDone: {
        std::unique_ptr<ExtractResult> result((ExtractResult*)lp);
        if (result) FinishExtraction(result->ok, result->error);
        return 0;
    }
    case WM_CLOSE:
        if (extracting_) {
            notice_ = L"正在解压，请稍候。";
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        DestroyWindow(hwnd_);
        return 0;
    case WM_NCDESTROY:
        if (selfDelete_) {
            selfDelete_ = false;
            delete this;
            return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

LRESULT CALLBACK ArchiveWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ArchiveWindow* self = (ArchiveWindow*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        self = (ArchiveWindow*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd_ = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    return self->Handle(msg, wp, lp);
}
