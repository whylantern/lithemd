// Shared helpers for LitheMD: text, files, settings and the colour palette.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <map>
#include <string>
#include <vector>
#include <cstdint>

inline constexpr wchar_t kAppVersion[] = L"2.4.3.2";

// ------------------------------------------------------------------ strings
std::wstring WideFromUtf8(const std::string& s);
std::string Utf8FromWide(const std::wstring& s);
std::wstring DecodeBytes(const std::vector<uint8_t>& bytes, std::wstring& encodingName);
std::wstring NormalizeNewlines(const std::wstring& s);
std::wstring Trim(const std::wstring& s);
std::wstring TrimLeft(const std::wstring& s);
std::wstring TrimRight(const std::wstring& s);
std::wstring ToLower(const std::wstring& s);
bool StartsWithI(const std::wstring& s, const std::wstring& p);
bool EndsWithI(const std::wstring& s, const std::wstring& p);
bool ContainsI(const std::wstring& s, const std::wstring& p);
std::wstring ReplaceAll(std::wstring s, const std::wstring& from, const std::wstring& to);
std::vector<std::wstring> SplitLines(const std::wstring& s);
std::wstring Shorten(const std::wstring& s, size_t max);
std::wstring TrimToMiddle(const std::wstring& s, size_t max);
std::wstring FormatBytes(uint64_t bytes);
int CountLines(const std::wstring& s);
std::wstring FormatFileTime(uint64_t fileTime);

// ------------------------------------------------------------------- files
bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out, std::wstring* error = nullptr);
bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& data, std::wstring* error = nullptr);
bool WriteTextFile(const std::wstring& path, const std::wstring& text);
bool FileExists(const std::wstring& path);
bool DirExists(const std::wstring& path);
bool EnsureDir(const std::wstring& path);
void RemoveTree(const std::wstring& path);
uint64_t FileWriteTimeUtc(const std::wstring& path);
std::wstring ExePath();
std::wstring ExeDir();
std::wstring LocalAppDataDir();
std::wstring LocalDataDir();
std::wstring PathDir(const std::wstring& path);
std::wstring PathName(const std::wstring& path);
std::wstring PathExt(const std::wstring& path);
std::wstring PathJoin(const std::wstring& a, const std::wstring& b);
std::wstring AbsolutePath(const std::wstring& path);
std::wstring TempPath(const std::wstring& name);

// ------------------------------------------------------------------- misc
std::wstring Base64Decode(const std::wstring& text, bool* ok = nullptr);
std::vector<uint8_t> Base64DecodeBytes(const std::wstring& text);
bool IsWebUrl(const std::wstring& url);
void OpenExternal(const std::wstring& pathOrUrl);
void EnableDarkModeForApp();
void ApplyDarkWindowTheme(HWND hwnd, bool dark);
bool SetClipboardText(HWND owner, const std::wstring& text);
std::wstring GetClipboardText(HWND owner);
std::wstring GuidString();
uint64_t UnixNowMs();

// ---------------------------------------------------------------- palette
struct Color {
    float r, g, b, a;
    Color() : r(0), g(0), b(0), a(1) {}
    Color(float rr, float gg, float bb, float aa = 1.0f) : r(rr), g(gg), b(bb), a(aa) {}
};
Color HexColor(const wchar_t* hex, float alpha = 1.0f);
Color ParseColorText(const std::wstring& text, const Color& fallback = Color());
std::wstring ColorToHex(const Color& color, bool withAlpha = true);
Color MixColor(const Color& a, const Color& b, float t);
std::vector<Color> DefaultInkPresets();

struct Palette {
    bool dark = false;
    Color bg, surface, fg, muted, border, codeBg, accent, accentSoft, hover, tableHead;
    Color danger, dangerSoft, quoteBar, selection;
    Color codeKeyword, codeString, codeNumber, codeComment, codeType, codePunct;
    Color jsonKey, jsonString, jsonNumber, jsonBool, jsonNull;
    static const Palette& Light();
    static const Palette& Dark();
    static const Palette& Warm();
    static const Palette& Green();
    static const Palette& Get(bool dark);
    static const Palette& Get(const std::wstring& theme);
};

// ---------------------------------------------------------------- settings
enum class DocKind {
    None, Markdown, Json, Notebook, Chat, Csv, Text,
    Docx, Doc, Xlsx, Xls, Pdf, Image
};

std::wstring DocKindName(DocKind kind);
bool IsBinaryKind(DocKind kind);
bool CanShowSource(DocKind kind);
// Identifies a document by extension and content sniffing. Text is only used
// for extension-less/unknown files; binary formats are detected by extension.
DocKind DetectKind(const std::wstring& path, const std::wstring& text);

// Text decoding. Auto means "UTF-8 first, then BOM/heuristic detection".
enum class TextEncoding : int {
    Auto = 0,
    Utf8,
    Utf16LE,
    Utf16BE,
    Utf32LE,
    Utf32BE,
    Gb18030,
    Gbk,
    Big5,
    ShiftJis,
    EucKr,
    Windows1252,
    Windows1251,
    Windows1250,
    Latin1,
    Ansi
};

// Document language is used as a hint when ranking legacy encodings.
enum class DocLanguage : int {
    Auto = 0,
    SimplifiedChinese,
    TraditionalChinese,
    Japanese,
    Korean,
    Western
};

constexpr UINT ShortcutCtrl = 1;
constexpr UINT ShortcutAlt = 2;
constexpr UINT ShortcutShift = 4;

struct ShortcutKey {
    UINT mods = 0;
    UINT vk = 0;
    bool Valid() const { return vk != 0; }
    bool operator==(const ShortcutKey& other) const {
        return mods == other.mods && vk == other.vk;
    }
};

bool ParseShortcut(const std::wstring& text, ShortcutKey& out);
std::wstring ShortcutToString(const ShortcutKey& key);
bool IsShortcutAllowed(const ShortcutKey& key);
bool IsReservedShortcut(const ShortcutKey& key);

struct DecodedText {
    std::wstring text;
    std::wstring encodingName;
    std::wstring error;
    TextEncoding encoding = TextEncoding::Auto;
    DocLanguage language = DocLanguage::Auto;
    int confidence = 100;
    bool ok = true;
    bool repaired = false;
    bool uncertain = false;
};

DecodedText DecodeText(const std::vector<uint8_t>& bytes,
                       TextEncoding requested = TextEncoding::Auto,
                       DocLanguage language = DocLanguage::Auto,
                       bool forceRepair = false);
std::wstring EncodingName(TextEncoding encoding);
std::wstring EncodingConfigName(TextEncoding encoding);
UINT EncodingCodePage(TextEncoding encoding);
std::wstring LanguageName(DocLanguage language);
std::wstring LanguageShortName(DocLanguage language);
std::wstring LanguageConfigName(DocLanguage language);
DocLanguage ParseLanguage(const std::wstring& value);

struct Settings {
    bool dark = false;
    std::wstring theme = L"blue";
    double zoom = 1.0;
    double fontScale = 1.0;
    int contentWidth = 1040;
    int wheelStep = 3;
    std::vector<std::wstring> recent;
    std::vector<std::wstring> recentBlocked;
    std::vector<std::wstring> workspaceHistory;
    std::wstring workspaceRoot;
    bool workspaceVisible = false;
    std::wstring backgroundImage;
    std::wstring backgroundImageId;
    int backgroundOpacity = 15;
    int backgroundFit = 0; // 0 cover, 1 contain, 2 stretch, 3 tile
    int backgroundAnchor = 4; // 3x3 crop anchor, 4 = center
    std::wstring configPath;
    DocLanguage documentLanguage = DocLanguage::Auto;
    // Only user overrides are stored. Missing commands use built-in defaults;
    // an empty array intentionally disables that command.
    std::map<std::wstring, std::vector<ShortcutKey>> shortcuts;

    static constexpr int MinWheelStep = 1;
    static constexpr int MaxWheelStep = 10;
    static constexpr size_t MaxRecent = 500;
    static constexpr size_t MaxWorkspaceHistory = 20;
    static constexpr int DefaultInkTools = 87; // pen, eraser, line, rectangle, text
    int inkToolsVisible = DefaultInkTools;
    int canvasWidth = 2048;
    int canvasHeight = 2048;
    bool canvasDark = false;
    Color customColors[2] = {
        Color(0.545f, 0.361f, 0.965f, 1.0f),
        Color(0.063f, 0.725f, 0.506f, 1.0f)
    };
    // Up to 12 saved pen colors. The five indices in inkToolbarPresets are
    // shown on the main ink toolbar; the rest stay in the palette window.
    std::vector<Color> inkPresets;
    std::vector<int> inkToolbarPresets;
    std::wstring uiLanguage = L"zh-CN";
    static constexpr int MinCanvasSize = 256;
    static constexpr int MaxCanvasSize = 8192;
    static constexpr int MinBackgroundOpacity = 5;
    static constexpr int MaxBackgroundOpacity = 60;
    static constexpr int DefaultBackgroundOpacity = 15;
    static constexpr int MinBackgroundFit = 0;
    static constexpr int MaxBackgroundFit = 3;
    static constexpr int MinBackgroundAnchor = 0;
    static constexpr int MaxBackgroundAnchor = 8;
    static constexpr int DefaultBackgroundAnchor = 4;
    static constexpr double MinFontScale = 0.85;
    static constexpr double MaxFontScale = 1.35;
    static constexpr int MinContentWidth = 680;
    static constexpr int MaxContentWidth = 1400;

    static std::wstring ResolveConfigPath();
    static Settings Load();
    bool Save();
    void AddRecent(const std::wstring& path);
    void RemoveRecent(const std::wstring& path);
    void ClearRecent();
    bool RecentBlocked(const std::wstring& path) const;
    void AddWorkspace(const std::wstring& path);
    void ClearWorkspaces();
    void Clamp();
};
