#include "common.h"
#include "json.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <cstdio>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <uxtheme.h>

// ------------------------------------------------------------------ strings
std::wstring WideFromUtf8(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n);
    return out;
}

std::string Utf8FromWide(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

namespace {

struct NamedKey {
    const wchar_t* name;
    UINT vk;
};

const NamedKey kNamedKeys[] = {
    { L"Space", VK_SPACE }, { L"Tab", VK_TAB }, { L"Enter", VK_RETURN },
    { L"Backspace", VK_BACK }, { L"Delete", VK_DELETE }, { L"Insert", VK_INSERT },
    { L"Home", VK_HOME }, { L"End", VK_END },
    { L"PageUp", VK_PRIOR }, { L"PageDown", VK_NEXT },
    { L"Up", VK_UP }, { L"Down", VK_DOWN }, { L"Left", VK_LEFT }, { L"Right", VK_RIGHT },
    { L"Escape", VK_ESCAPE },
    { L"Comma", VK_OEM_COMMA }, { L"Period", VK_OEM_PERIOD },
    { L"Slash", VK_OEM_2 }, { L"Semicolon", VK_OEM_1 }, { L"Quote", VK_OEM_7 },
    { L"LeftBracket", VK_OEM_4 }, { L"RightBracket", VK_OEM_6 },
    { L"Backslash", VK_OEM_5 }, { L"Minus", VK_OEM_MINUS }, { L"Equal", VK_OEM_PLUS },
    { L"NumpadPlus", VK_ADD }, { L"NumpadMinus", VK_SUBTRACT },
    { L"NumpadMultiply", VK_MULTIPLY }, { L"NumpadDivide", VK_DIVIDE },
    { L"NumpadDecimal", VK_DECIMAL }
};

const wchar_t* KeyName(UINT vk) {
    if (vk >= 'A' && vk <= 'Z') {
        static wchar_t buf[2];
        buf[0] = (wchar_t)vk;
        buf[1] = 0;
        return buf;
    }
    if (vk >= '0' && vk <= '9') {
        static wchar_t buf[2];
        buf[0] = (wchar_t)vk;
        buf[1] = 0;
        return buf;
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        static wchar_t buf[8];
        swprintf(buf, 8, L"F%u", vk - VK_F1 + 1);
        return buf;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        static wchar_t buf[8];
        swprintf(buf, 8, L"Numpad%u", vk - VK_NUMPAD0);
        return buf;
    }
    for (const auto& n : kNamedKeys) if (n.vk == vk) return n.name;
    return nullptr;
}

bool IsNavigationKey(UINT vk) {
    return vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
           vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT;
}

} // namespace

bool IsReservedShortcut(const ShortcutKey& key) {
    UINT mods = key.mods;
    UINT vk = key.vk;
    if (mods == (ShortcutCtrl | ShortcutAlt) && vk == VK_DELETE) return true;
    if (mods == ShortcutAlt && (vk == VK_F4 || vk == VK_TAB || vk == VK_ESCAPE || vk == VK_SPACE)) return true;
    if (mods == ShortcutCtrl && vk == VK_ESCAPE) return true;
    if (mods == (ShortcutCtrl | ShortcutShift) && vk == VK_ESCAPE) return true;
    return false;
}

bool IsShortcutAllowed(const ShortcutKey& key) {
    if (!key.Valid()) return false;
    if (key.vk == VK_CONTROL || key.vk == VK_LCONTROL || key.vk == VK_RCONTROL ||
        key.vk == VK_SHIFT || key.vk == VK_LSHIFT || key.vk == VK_RSHIFT ||
        key.vk == VK_MENU || key.vk == VK_LMENU || key.vk == VK_RMENU ||
        key.vk == VK_LWIN || key.vk == VK_RWIN) return false;
    if (key.vk == VK_ESCAPE) return false;
    if (IsReservedShortcut(key)) return false;
    bool hasCmd = (key.mods & (ShortcutCtrl | ShortcutAlt)) != 0;
    bool fkey = key.vk >= VK_F1 && key.vk <= VK_F24;
    bool nav = IsNavigationKey(key.vk);
    bool standalone = nav || key.vk == VK_DELETE;
    if (!hasCmd && !fkey && !standalone) {
        // Plain letters/digits, Enter, Tab, Space, etc. would interfere with
        // normal typing and control navigation. Delete is allowed for ink
        // selection; edit controls keep priority over global shortcuts.
        return false;
    }
    if (!hasCmd && (key.vk == VK_TAB || key.vk == VK_RETURN || key.vk == VK_BACK)) return false;
    return true;
}

bool ParseShortcut(const std::wstring& text, ShortcutKey& out) {
    out = ShortcutKey{};
    std::wstring s = Trim(text);
    if (s.empty()) return false;
    size_t pos = 0;
    bool haveKey = false;
    while (pos <= s.size()) {
        size_t plus = s.find(L'+', pos);
        std::wstring token = Trim(s.substr(pos, plus == std::wstring::npos ? std::wstring::npos : plus - pos));
        if (token.empty()) return false;
        std::wstring low = ToLower(token);
        if (low == L"ctrl" || low == L"control") out.mods |= ShortcutCtrl;
        else if (low == L"alt" || low == L"menu") out.mods |= ShortcutAlt;
        else if (low == L"shift") out.mods |= ShortcutShift;
        else if (low == L"win" || low == L"meta") return false;
        else {
            if (haveKey) return false;
            UINT vk = 0;
            if (token.size() == 1) {
                wchar_t c = token[0];
                if (c >= L'a' && c <= L'z') c = (wchar_t)(c - L'a' + L'A');
                if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) vk = (UINT)c;
            } else if (low[0] == L'f' && low.size() <= 3) {
                int number = 0;
                bool digits = true;
                for (size_t i = 1; i < low.size(); i++) {
                    if (low[i] < L'0' || low[i] > L'9') { digits = false; break; }
                    number = number * 10 + (int)(low[i] - L'0');
                }
                if (digits && number >= 1 && number <= 24) vk = VK_F1 + (UINT)(number - 1);
            } else if (low.size() == 7 && low.compare(0, 6, L"numpad") == 0 &&
                       low[6] >= L'0' && low[6] <= L'9') {
                vk = VK_NUMPAD0 + (UINT)(low[6] - L'0');
            } else {
                for (const auto& n : kNamedKeys) {
                    if (_wcsicmp(low.c_str(), n.name) == 0) { vk = n.vk; break; }
                }
            }
            if (!vk) return false;
            out.vk = vk;
            haveKey = true;
        }
        if (plus == std::wstring::npos) break;
        pos = plus + 1;
    }
    return haveKey && IsShortcutAllowed(out);
}

std::wstring ShortcutToString(const ShortcutKey& key) {
    if (!key.Valid()) return L"";
    std::wstring out;
    if (key.mods & ShortcutCtrl) out += L"Ctrl+";
    if (key.mods & ShortcutAlt) out += L"Alt+";
    if (key.mods & ShortcutShift) out += L"Shift+";
    const wchar_t* name = KeyName(key.vk);
    if (name) out += name;
    else {
        wchar_t buf[24];
        swprintf(buf, 24, L"VK%02X", key.vk);
        out += buf;
    }
    return out;
}

static bool IsValidUtf8(const uint8_t* data, size_t size) {
    size_t i = 0;
    while (i < size) {
        uint8_t c = data[i];
        if (c < 0x80) { i++; continue; }
        int extra = 0;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
        else return false;
        if (i + extra >= size) return false;
        for (int k = 1; k <= extra; k++) {
            uint8_t cc = data[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (extra == 1 && cp < 0x80) return false;
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && cp < 0x10000) return false;
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += (size_t)extra + 1;
    }
    return true;
}

namespace {

bool DecodeMultiByte(const uint8_t* data, size_t size, UINT cp, bool strict, std::wstring& out) {
    out.clear();
    if (size == 0) return true;
    if (size > (size_t)INT_MAX) return false;
    DWORD flags = strict ? MB_ERR_INVALID_CHARS : 0;
    int n = MultiByteToWideChar(cp, flags, (const char*)data, (int)size, nullptr, 0);
    if (n <= 0) return false;
    out.resize((size_t)n);
    int got = MultiByteToWideChar(cp, flags, (const char*)data, (int)size, &out[0], n);
    if (got <= 0) { out.clear(); return false; }
    out.resize((size_t)got);
    return true;
}

bool DecodeUtf16Bytes(const std::vector<uint8_t>& bytes, bool little, std::wstring& out) {
    size_t start = 0;
    if (bytes.size() >= 2) {
        uint16_t bom = (uint16_t)(bytes[0] | (bytes[1] << 8));
        if (little && bom == 0xFEFF) start = 2;
        if (!little && bytes[0] == 0xFE && bytes[1] == 0xFF) start = 2;
    }
    size_t count = (bytes.size() - start) / 2;
    out.resize(count);
    for (size_t i = 0; i < count; i++) {
        uint8_t a = bytes[start + i * 2];
        uint8_t b = bytes[start + i * 2 + 1];
        out[i] = (wchar_t)(little ? (a | (b << 8)) : ((a << 8) | b));
    }
    return true;
}

bool DecodeUtf32Bytes(const std::vector<uint8_t>& bytes, bool little, std::wstring& out) {
    size_t start = 0;
    if (bytes.size() >= 4) {
        if (little && bytes[0] == 0xFF && bytes[1] == 0xFE && bytes[2] == 0 && bytes[3] == 0) start = 4;
        if (!little && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0xFE && bytes[3] == 0xFF) start = 4;
    }
    out.clear();
    for (size_t i = start; i + 3 < bytes.size(); i += 4) {
        uint32_t cp = little
            ? ((uint32_t)bytes[i] | ((uint32_t)bytes[i + 1] << 8) |
               ((uint32_t)bytes[i + 2] << 16) | ((uint32_t)bytes[i + 3] << 24))
            : (((uint32_t)bytes[i] << 24) | ((uint32_t)bytes[i + 1] << 16) |
               ((uint32_t)bytes[i + 2] << 8) | (uint32_t)bytes[i + 3]);
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp <= 0xFFFF) {
            out += (wchar_t)cp;
        } else {
            cp -= 0x10000;
            out += (wchar_t)(0xD800 + (cp >> 10));
            out += (wchar_t)(0xDC00 + (cp & 0x3FF));
        }
    }
    return true;
}

bool DecodeAsEncoding(const std::vector<uint8_t>& bytes, TextEncoding e, bool strict, std::wstring& out) {
    switch (e) {
    case TextEncoding::Utf8: {
        const uint8_t* data = bytes.data();
        size_t size = bytes.size();
        if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
            data += 3;
            size -= 3;
        }
        return DecodeMultiByte(data, size, CP_UTF8, strict, out);
    }
    case TextEncoding::Utf16LE: return DecodeUtf16Bytes(bytes, true, out);
    case TextEncoding::Utf16BE: return DecodeUtf16Bytes(bytes, false, out);
    case TextEncoding::Utf32LE: return DecodeUtf32Bytes(bytes, true, out);
    case TextEncoding::Utf32BE: return DecodeUtf32Bytes(bytes, false, out);
    case TextEncoding::Gb18030: return DecodeMultiByte(bytes.data(), bytes.size(), 54936, strict, out);
    case TextEncoding::Gbk: return DecodeMultiByte(bytes.data(), bytes.size(), 936, strict, out);
    case TextEncoding::Big5: return DecodeMultiByte(bytes.data(), bytes.size(), 950, strict, out);
    case TextEncoding::ShiftJis: return DecodeMultiByte(bytes.data(), bytes.size(), 932, strict, out);
    case TextEncoding::EucKr: return DecodeMultiByte(bytes.data(), bytes.size(), 949, strict, out);
    case TextEncoding::Windows1252: return DecodeMultiByte(bytes.data(), bytes.size(), 1252, strict, out);
    case TextEncoding::Windows1251: return DecodeMultiByte(bytes.data(), bytes.size(), 1251, strict, out);
    case TextEncoding::Windows1250: return DecodeMultiByte(bytes.data(), bytes.size(), 1250, strict, out);
    case TextEncoding::Latin1: return DecodeMultiByte(bytes.data(), bytes.size(), 28591, strict, out);
    case TextEncoding::Ansi: return DecodeMultiByte(bytes.data(), bytes.size(), GetACP(), strict, out);
    default: return false;
    }
}

bool IsAsciiSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

bool IsCjk(wchar_t c) {
    return (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) ||
           (c >= 0xF900 && c <= 0xFAFF);
}

bool IsTraditionalMarker(wchar_t c) {
    static const std::wstring markers = L"這為個國時來們於對會發動過說產種後學經進著電裡兩體機當點從業應開還這麼";
    return markers.find(c) != std::wstring::npos;
}

bool IsSimplifiedMarker(wchar_t c) {
    static const std::wstring markers = L"这为个国时来们于对会发动过说产种后学经进着电里两体机当点从业应开还这么";
    return markers.find(c) != std::wstring::npos;
}

int MojibakeHits(const std::wstring& text) {
    // Only multi-character signatures are counted. Single letters such as
    // æ/å/ä or rare CJK characters can be legitimate text; using them as
    // evidence would make normal UTF-8 documents look like mojibake.
    static const wchar_t* signatures[] = {
        L"锟斤拷", L"�", L"ï»¿", L"â€",
        L"Ã¤", L"Ã¥", L"Ã¶", L"Ã©", L"Ã¨", L"Ã¼", L"ÃŸ",
        L"æ–‡", L"ä¸­", L"æµ‹", L"æ–°", L"é¡µ", L"çš„", L"ä»–", L"æˆ‘",
        L"ä½ ", L"æ˜¯", L"ä¸€", L"äºº", L"å¤§", L"å°�", L"æ—¥", L"æœ¬",
        L"å·¥", L"ç”Ÿ", L"ç”¨", L"å�‘", L"é€š", L"ç»“", L"æžœ", L"è¿”",
        L"å›ž", L"å¥½", L"å¯¹", L"æœ‰", L"äº†", L"åœ¨", L"ä¸Š", L"ä¸‹",
        L"涓枃", L"鏂囦", L"鐨勶", L"浠ｇ", L"銆傚", L"鈥滃"
    };
    size_t sampleSize = (std::min)(text.size(), (size_t)200000);
    std::wstring sample;
    const std::wstring* view = &text;
    if (sampleSize < text.size()) {
        sample = text.substr(0, sampleSize);
        view = &sample;
    }
    int hits = 0;
    for (const wchar_t* sig : signatures) {
        size_t len = wcslen(sig);
        size_t pos = 0;
        while ((pos = view->find(sig, pos)) != std::wstring::npos) {
            hits++;
            pos += len;
        }
    }
    // UTF-8 decoded as Latin-1 leaves C1 control characters behind. They are
    // strong evidence of a wrong single-byte decode.
    for (wchar_t c : *view) if (c >= 0x80 && c <= 0x9F) hits++;
    return hits;
}

const std::wstring& CommonCjk() {
    static const std::wstring s =
        L"的一是在不了有和人这中大为上个国我以要他时来用们生到作地于出就分对成会可主发年动同工也能下过子说产种面而方后多定行学法所民得经十三之进着等部度家电力里如水化高自二理起小物现实加量都两体制机当使点从业本去把性好应开它合还因由其些然前外天政四日那社义事平形相全表间样与关各重新线内数正心反你明看原又么利比或但质气第向道命此变条只没结解问意建月公无系军很情者最立代想已通并提直题党程展五果料象员革位入常文总次品式活设及管特件长求老头基资边流路级少图山统接知较将组见计别她手角期根论运农指几九区强放决西被干做必战先回则任取据处队南给色光门即保治北造百规热领七海口东导器压志世金增争济阶油思术极交受联什认六共权收证改清己美再采转更单风切打白教速花带安场身车例真务具万每目至达走积示议声报斗完类八离华名确才科张信马节话米整空元况今集温传土许步群广石记需段研界拉林律叫且究观越织装影算低持音众书布复容儿须际商非验连断深难近矿千周委素技备半办青省列习响约支般史感劳便团往酸历市克何除消构府称太准精值号率族维划选标写存候毛亲快效斯院查江型眼王按格养易置派层片始却专状育厂京识适属圆包火住调满县局照参红细引听该铁价严龙飞";
    return s;
}

const std::wstring& CommonCjkTraditional() {
    static const std::wstring s =
        L"的一是在不了有和人這中大為上個國我以要他時來用們生到作地於出就分對成會可主發年動同工也能下過子說產種面而方後多定行學法所民得經十三之進著等部度家電力裡如水化高自二理起小物現實加量都兩體制機當使點從業本去把性好應開它合還因由其些然前外天政四日那社義事平形相全表間樣與關各重新線內數正心反你明看原又麼利比或但質氣第向道命此變條只沒結解問意建月公無系軍很情者最立代想已通並提直題黨程展五果料象員革位入常文總次品式活設及管特件長求老頭基資邊流路級少圖山統接知較將組見計別她手角期根論運農指幾九區強放決西被幹做必戰先回則任取據處隊南給色光門即保治北造百規熱領七海口東導器壓志世金增爭濟階油思術極交受聯什認六共權收證改清己美再採轉更單風切打白教速花帶安場身車例真務具萬每目至達走積示議聲報鬥完類八離華名確才科張信馬節話米整空元況今集溫傳土許步群廣石記需段研界拉林律叫且究觀越織裝影算低持音眾書布復容兒須際商非驗連斷深難近礦千周委素技備半辦青省列習響約支般史感勞便團往酸歷市克何除消構府稱太準精值號率族維劃選標寫存候毛親快效斯院查江型眼王按格養易置派層片始卻專狀育廠京識適屬圓包火住調滿縣局照參紅細引聽該鐵價嚴龍飛";
    return s;
}

const std::wstring& CommonKana() {
    static const std::wstring s =
        L"のにをはがでとしていうあかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわんがぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽっゃゅょー";
    return s;
}

const std::wstring& CommonHangul() {
    static const std::wstring s =
        L"이다는을를에의가하한지기사리도로으스니어나대주시구아자장정수해보전무";
    return s;
}

double TextQuality(const std::wstring& text, DocLanguage language) {
    if (text.empty()) return 0.0;
    size_t sample = (std::min)(text.size(), (size_t)200000);
    double score = 0.0;
    int cjk = 0, kana = 0, hangul = 0, latin = 0, cyrillic = 0, simp = 0, trad = 0;
    int cjkCommon = 0, kanaCommon = 0, hangulCommon = 0;
    const std::wstring& commonCjk = CommonCjk();
    const std::wstring& commonTrad = CommonCjkTraditional();
    const std::wstring& commonKana = CommonKana();
    const std::wstring& commonHangul = CommonHangul();
    for (size_t i = 0; i < sample; i++) {
        wchar_t c = text[i];
        if (c == 0xFFFD) score -= 30.0;
        else if (c < 0x20 && c != L'\n' && c != L'\r' && c != L'\t') score -= 12.0;
        else if (c >= 0x80 && c <= 0x9F) score -= 10.0;
        else if (c == 0x7F) score -= 6.0;
        else if (IsAsciiSpace(c)) score += 0.2;
        else if (IsCjk(c)) {
            cjk++;
            bool common = commonCjk.find(c) != std::wstring::npos ||
                          commonTrad.find(c) != std::wstring::npos;
            if (common) { score += 3.0; cjkCommon++; }
            else score += 0.2;
            if (IsTraditionalMarker(c)) trad++;
            if (IsSimplifiedMarker(c)) simp++;
        } else if (c >= 0x3040 && c <= 0x30FF) {
            kana++;
            if (commonKana.find(c) != std::wstring::npos) { score += 3.5; kanaCommon++; }
            else score += 0.5;
        } else if (c >= 0xAC00 && c <= 0xD7AF) {
            hangul++;
            if (commonHangul.find(c) != std::wstring::npos) { score += 3.5; hangulCommon++; }
            else score += 0.3;
        } else if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
                 (c >= 0x00C0 && c <= 0x024F)) { score += 1.0; latin++; }
        else if (c >= 0x0400 && c <= 0x04FF) { score += 1.0; cyrillic++; }
        else if ((c >= L'0' && c <= L'9') || (c >= 0x20 && c < 0x7F)) score += 0.35;
        else if (c >= 0xE000 && c <= 0xF8FF) score -= 12.0;
        else if (c >= 0x3000 && c <= 0x303F) score += 0.8;
        else score += 0.1;
    }
    if (language == DocLanguage::Japanese) {
        score += kanaCommon * 4.0 + (kana - kanaCommon) * 0.5;
        if (kana == 0 && cjk == 0) score -= 12.0;
    } else if (language == DocLanguage::Korean) {
        score += hangulCommon * 4.0 + (hangul - hangulCommon) * 0.2;
        if (hangul == 0) score -= 12.0;
    } else if (language == DocLanguage::SimplifiedChinese) {
        if (cjk == 0) score -= 15.0;
        score += cjkCommon * 3.0 + (cjk - cjkCommon) * 0.2 + simp * 1.5 - trad * 0.6;
    } else if (language == DocLanguage::TraditionalChinese) {
        if (cjk == 0) score -= 15.0;
        score += cjkCommon * 3.0 + (cjk - cjkCommon) * 0.2 + trad * 1.5 - simp * 0.6;
    } else if (language == DocLanguage::Western) {
        if (latin == 0 && cyrillic == 0) score -= 15.0;
        score += latin * 1.0 + cyrillic * 1.0 - cjk * 1.2;
    } else {
        score += cjkCommon * 0.8 + kanaCommon * 0.8 + hangulCommon * 0.8 +
                 latin * 0.7 + cyrillic * 0.7;
    }
    if (language == DocLanguage::Auto && cjk == 0 && kana == 0 && hangul == 0) {
        if (latin >= 8) score += 22.0;
    }
    score -= MojibakeHits(text) * 10.0;
    return score * 100.0 / (double)(std::max)((size_t)1, sample);
}

DocLanguage InferLanguage(const std::wstring& text) {
    int cjk = 0, kana = 0, hangul = 0, latin = 0, cyrillic = 0, simp = 0, trad = 0;
    size_t sample = (std::min)(text.size(), (size_t)100000);
    for (size_t i = 0; i < sample; i++) {
        wchar_t c = text[i];
        if (IsCjk(c)) {
            cjk++;
            if (IsTraditionalMarker(c)) trad++;
            if (IsSimplifiedMarker(c)) simp++;
        } else if (c >= 0x3040 && c <= 0x30FF) kana++;
        else if (c >= 0xAC00 && c <= 0xD7AF) hangul++;
        else if (c >= 0x0400 && c <= 0x04FF) cyrillic++;
        else if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') ||
                 (c >= 0x00C0 && c <= 0x024F)) latin++;
    }
    if (kana >= 4 && kana * 2 >= cjk) return DocLanguage::Japanese;
    if (hangul >= 4 && hangul * 2 >= cjk) return DocLanguage::Korean;
    if (cjk >= 4 && cjk >= latin + cyrillic) return trad > simp ? DocLanguage::TraditionalChinese
                                                               : DocLanguage::SimplifiedChinese;
    if (latin + cyrillic >= 4) return DocLanguage::Western;
    return DocLanguage::Auto;
}

double EncodingPrior(TextEncoding encoding, DocLanguage language) {
    if (language == DocLanguage::SimplifiedChinese) {
        if (encoding == TextEncoding::Gb18030) return 25.0;
        if (encoding == TextEncoding::Big5) return 8.0;
        if (encoding == TextEncoding::Utf16LE || encoding == TextEncoding::Utf16BE) return -5.0;
        return 0.0;
    }
    if (language == DocLanguage::TraditionalChinese) {
        if (encoding == TextEncoding::Big5) return 25.0;
        if (encoding == TextEncoding::Gb18030) return 8.0;
        if (encoding == TextEncoding::Utf16LE || encoding == TextEncoding::Utf16BE) return -5.0;
        return 0.0;
    }
    if (language == DocLanguage::Japanese) {
        if (encoding == TextEncoding::ShiftJis) return 25.0;
        if (encoding == TextEncoding::Utf16LE || encoding == TextEncoding::Utf16BE) return -5.0;
        return 0.0;
    }
    if (language == DocLanguage::Korean) {
        if (encoding == TextEncoding::EucKr) return 25.0;
        if (encoding == TextEncoding::Utf16LE || encoding == TextEncoding::Utf16BE) return -5.0;
        return 0.0;
    }
    if (language == DocLanguage::Western) {
        if (encoding == TextEncoding::Windows1252) return 15.0;
        if (encoding == TextEncoding::Latin1) return 8.0;
        if (encoding == TextEncoding::Gb18030 || encoding == TextEncoding::Big5 ||
            encoding == TextEncoding::ShiftJis || encoding == TextEncoding::EucKr) return -10.0;
        return 0.0;
    }
    if (encoding == TextEncoding::Gb18030) return 18.0;
    if (encoding == TextEncoding::Big5) return 8.0;
    if (encoding == TextEncoding::ShiftJis) return 4.0;
    if (encoding == TextEncoding::Utf16LE || encoding == TextEncoding::Utf16BE) return -6.0;
    if (encoding == TextEncoding::Latin1) return -2.0;
    return 0.0;
}

std::vector<TextEncoding> CandidateEncodings(DocLanguage language) {
    std::vector<TextEncoding> out;
    auto add = [&](TextEncoding e) {
        if (e == TextEncoding::Auto || e == TextEncoding::Utf8) return;
        for (TextEncoding x : out) if (x == e) return;
        out.push_back(e);
    };
    switch (language) {
    case DocLanguage::TraditionalChinese:
        add(TextEncoding::Big5); add(TextEncoding::Gb18030); add(TextEncoding::ShiftJis);
        add(TextEncoding::EucKr); add(TextEncoding::Utf16LE); add(TextEncoding::Utf16BE);
        add(TextEncoding::Windows1252); add(TextEncoding::Latin1);
        break;
    case DocLanguage::Japanese:
        add(TextEncoding::ShiftJis); add(TextEncoding::Gb18030); add(TextEncoding::Big5);
        add(TextEncoding::EucKr); add(TextEncoding::Utf16LE); add(TextEncoding::Utf16BE);
        add(TextEncoding::Windows1252); add(TextEncoding::Latin1);
        break;
    case DocLanguage::Korean:
        add(TextEncoding::EucKr); add(TextEncoding::Gb18030); add(TextEncoding::Big5);
        add(TextEncoding::ShiftJis); add(TextEncoding::Utf16LE); add(TextEncoding::Utf16BE);
        add(TextEncoding::Windows1252); add(TextEncoding::Latin1);
        break;
    case DocLanguage::Western:
        add(TextEncoding::Windows1252);
        add(TextEncoding::Latin1); add(TextEncoding::Gb18030); add(TextEncoding::Big5);
        add(TextEncoding::ShiftJis); add(TextEncoding::EucKr);
        add(TextEncoding::Utf16LE); add(TextEncoding::Utf16BE);
        break;
    case DocLanguage::SimplifiedChinese:
        add(TextEncoding::Gb18030); add(TextEncoding::Big5); add(TextEncoding::ShiftJis);
        add(TextEncoding::EucKr); add(TextEncoding::Utf16LE); add(TextEncoding::Utf16BE);
        add(TextEncoding::Windows1252); add(TextEncoding::Latin1);
        break;
    default:
        add(TextEncoding::Gb18030); add(TextEncoding::Big5); add(TextEncoding::ShiftJis);
        add(TextEncoding::EucKr); add(TextEncoding::Utf16LE); add(TextEncoding::Utf16BE);
        add(TextEncoding::Windows1252); add(TextEncoding::Latin1);
        break;
    }
    return out;
}

bool EncodeAsCodePage(const std::wstring& text, UINT cp, std::vector<uint8_t>& out) {
    out.clear();
    if (text.empty()) return true;
    if (text.size() > (size_t)INT_MAX) return false;
    BOOL usedDefault = FALSE;
    int n = WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, text.data(), (int)text.size(),
                                nullptr, 0, nullptr, &usedDefault);
    if (n <= 0 || usedDefault) return false;
    out.resize((size_t)n);
    usedDefault = FALSE;
    int got = WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, text.data(), (int)text.size(),
                                  (char*)out.data(), n, nullptr, &usedDefault);
    if (got <= 0 || usedDefault) { out.clear(); return false; }
    out.resize((size_t)got);
    return true;
}

bool TryRepairLine(const std::wstring& line, DocLanguage language, bool force, std::wstring& out) {
    int hits = MojibakeHits(line);
    if (hits < 1) return false;
    double current = TextQuality(line, language);
    const TextEncoding wrong[] = {
        TextEncoding::Gb18030, TextEncoding::Windows1252, TextEncoding::Latin1,
        TextEncoding::Big5, TextEncoding::ShiftJis
    };
    std::wstring best;
    double bestScore = current;
    double need = force ? 2.0 : 5.0;
    for (TextEncoding w : wrong) {
        std::vector<uint8_t> bytes;
        if (!EncodeAsCodePage(line, EncodingCodePage(w), bytes)) continue;
        std::wstring candidate;
        if (!DecodeAsEncoding(bytes, TextEncoding::Utf8, true, candidate)) continue;
        DocLanguage candidateLanguage = InferLanguage(candidate);
        if (candidateLanguage == DocLanguage::Auto) candidateLanguage = language;
        double score = TextQuality(candidate, candidateLanguage);
        if (score > bestScore + need && MojibakeHits(candidate) < hits) {
            bestScore = score;
            best = std::move(candidate);
        }
    }
    if (best.empty()) return false;
    out = std::move(best);
    return true;
}

bool TryRepairMojibake(std::wstring& text, DocLanguage language, bool force) {
    if (MojibakeHits(text) < 1) return false;
    // Repair line by line. A file can mix a normal Chinese title with a body
    // that was already double-encoded; encoding the whole text with the wrong
    // code page would fail on the normal title.
    std::wstring out;
    out.reserve(text.size());
    bool changed = false;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        std::wstring line = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        std::wstring repaired;
        if (TryRepairLine(line, language, force, repaired)) {
            out += repaired;
            changed = true;
        } else {
            out += line;
        }
        if (end == std::wstring::npos) break;
        out += L'\n';
        start = end + 1;
    }
    if (!changed) return false;
    text = std::move(out);
    return true;
}

} // namespace

std::wstring EncodingName(TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::Auto: return L"自动（UTF-8 优先）";
    case TextEncoding::Utf8: return L"UTF-8";
    case TextEncoding::Utf16LE: return L"UTF-16 LE";
    case TextEncoding::Utf16BE: return L"UTF-16 BE";
    case TextEncoding::Utf32LE: return L"UTF-32 LE";
    case TextEncoding::Utf32BE: return L"UTF-32 BE";
    case TextEncoding::Gb18030: return L"GB18030";
    case TextEncoding::Gbk: return L"GBK";
    case TextEncoding::Big5: return L"Big5";
    case TextEncoding::ShiftJis: return L"Shift-JIS";
    case TextEncoding::EucKr: return L"EUC-KR";
    case TextEncoding::Windows1252: return L"Windows-1252";
    case TextEncoding::Windows1251: return L"Windows-1251";
    case TextEncoding::Windows1250: return L"Windows-1250";
    case TextEncoding::Latin1: return L"Latin-1";
    case TextEncoding::Ansi: return L"ANSI (CP" + std::to_wstring(GetACP()) + L")";
    default: return L"未知编码";
    }
}

std::wstring EncodingConfigName(TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::Utf8: return L"utf-8";
    case TextEncoding::Utf16LE: return L"utf-16le";
    case TextEncoding::Utf16BE: return L"utf-16be";
    case TextEncoding::Utf32LE: return L"utf-32le";
    case TextEncoding::Utf32BE: return L"utf-32be";
    case TextEncoding::Gb18030: return L"gb18030";
    case TextEncoding::Gbk: return L"gbk";
    case TextEncoding::Big5: return L"big5";
    case TextEncoding::ShiftJis: return L"shift-jis";
    case TextEncoding::EucKr: return L"euc-kr";
    case TextEncoding::Windows1252: return L"windows-1252";
    case TextEncoding::Windows1251: return L"windows-1251";
    case TextEncoding::Windows1250: return L"windows-1250";
    case TextEncoding::Latin1: return L"latin-1";
    case TextEncoding::Ansi: return L"ansi";
    default: return L"auto";
    }
}

UINT EncodingCodePage(TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::Utf8: return CP_UTF8;
    case TextEncoding::Gb18030: return 54936;
    case TextEncoding::Gbk: return 936;
    case TextEncoding::Big5: return 950;
    case TextEncoding::ShiftJis: return 932;
    case TextEncoding::EucKr: return 949;
    case TextEncoding::Windows1252: return 1252;
    case TextEncoding::Windows1251: return 1251;
    case TextEncoding::Windows1250: return 1250;
    case TextEncoding::Latin1: return 28591;
    case TextEncoding::Ansi: return GetACP();
    default: return 0;
    }
}

std::wstring LanguageName(DocLanguage language) {
    switch (language) {
    case DocLanguage::SimplifiedChinese: return L"简体中文";
    case DocLanguage::TraditionalChinese: return L"繁体中文";
    case DocLanguage::Japanese: return L"日语";
    case DocLanguage::Korean: return L"韩语";
    case DocLanguage::Western: return L"西文";
    default: return L"自动";
    }
}

std::wstring LanguageShortName(DocLanguage language) {
    switch (language) {
    case DocLanguage::SimplifiedChinese: return L"简中";
    case DocLanguage::TraditionalChinese: return L"繁中";
    case DocLanguage::Japanese: return L"日语";
    case DocLanguage::Korean: return L"韩语";
    case DocLanguage::Western: return L"西文";
    default: return L"";
    }
}

std::wstring LanguageConfigName(DocLanguage language) {
    switch (language) {
    case DocLanguage::SimplifiedChinese: return L"zh-cn";
    case DocLanguage::TraditionalChinese: return L"zh-tw";
    case DocLanguage::Japanese: return L"ja";
    case DocLanguage::Korean: return L"ko";
    case DocLanguage::Western: return L"western";
    default: return L"auto";
    }
}

DocLanguage ParseLanguage(const std::wstring& value) {
    std::wstring v = ToLower(Trim(value));
    if (v == L"zh-cn" || v == L"zh_cn" || v == L"zh-hans" || v == L"chinese" ||
        v == L"simplified" || v == L"简体" || v == L"简体中文") return DocLanguage::SimplifiedChinese;
    if (v == L"zh-tw" || v == L"zh_tw" || v == L"zh-hant" || v == L"traditional" ||
        v == L"繁体" || v == L"繁体中文") return DocLanguage::TraditionalChinese;
    if (v == L"ja" || v == L"jp" || v == L"japanese" || v == L"日语") return DocLanguage::Japanese;
    if (v == L"ko" || v == L"kr" || v == L"korean" || v == L"韩语") return DocLanguage::Korean;
    if (v == L"western" || v == L"en" || v == L"latin" || v == L"西文") return DocLanguage::Western;
    return DocLanguage::Auto;
}

DecodedText DecodeText(const std::vector<uint8_t>& bytes, TextEncoding requested,
                       DocLanguage language, bool forceRepair) {
    DecodedText result;
    result.encoding = requested;
    result.language = language;
    const size_t n = bytes.size();

    auto finish = [&](TextEncoding enc, std::wstring text, int confidence) {
        result.encoding = enc;
        result.encodingName = EncodingName(enc);
        result.text = std::move(text);
        result.confidence = confidence;
        if (language == DocLanguage::Auto) result.language = InferLanguage(result.text);
        bool wantsRepair = forceRepair || MojibakeHits(result.text) >= 2;
        if ((requested == TextEncoding::Auto || forceRepair) && wantsRepair &&
            TryRepairMojibake(result.text, result.language, forceRepair)) {
            result.repaired = true;
            result.confidence = (std::max)(result.confidence, 85);
            if (language == DocLanguage::Auto) result.language = InferLanguage(result.text);
        }
        if (result.confidence < 45) result.uncertain = true;
        if (!result.repaired && MojibakeHits(result.text) >= 3) result.uncertain = true;
        return std::move(result);
    };

    if (requested != TextEncoding::Auto) {
        std::wstring text;
        if (!DecodeAsEncoding(bytes, requested, true, text)) {
            result.ok = false;
            result.error = L"无法按 " + EncodingName(requested) + L" 解码该文件";
            result.text.clear();
            return result;
        }
        return finish(requested, std::move(text), 100);
    }

    // BOM first. UTF-32 LE starts with FF FE, so it must be checked before
    // UTF-16 LE.
    if (n >= 4 && bytes[0] == 0xFF && bytes[1] == 0xFE && bytes[2] == 0 && bytes[3] == 0) {
        std::wstring text;
        DecodeUtf32Bytes(bytes, true, text);
        return finish(TextEncoding::Utf32LE, std::move(text), 100);
    }
    if (n >= 4 && bytes[0] == 0 && bytes[1] == 0 && bytes[2] == 0xFE && bytes[3] == 0xFF) {
        std::wstring text;
        DecodeUtf32Bytes(bytes, false, text);
        return finish(TextEncoding::Utf32BE, std::move(text), 100);
    }
    if (n >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        std::wstring text;
        if (!DecodeMultiByte(bytes.data() + 3, n - 3, CP_UTF8, true, text)) {
            result.ok = false;
            result.error = L"UTF-8 BOM 内容无效";
            return result;
        }
        return finish(TextEncoding::Utf8, std::move(text), 100);
    }
    if (n >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        std::wstring text;
        DecodeUtf16Bytes(bytes, true, text);
        return finish(TextEncoding::Utf16LE, std::move(text), 100);
    }
    if (n >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        std::wstring text;
        DecodeUtf16Bytes(bytes, false, text);
        return finish(TextEncoding::Utf16BE, std::move(text), 100);
    }

    // Default behavior: ordinary UTF-8 wins before any legacy heuristic.
    bool hasNul = false;
    for (size_t i = 0; i < (std::min)(n, (size_t)4000); i++) {
        if (bytes[i] == 0) { hasNul = true; break; }
    }
    if (!hasNul && IsValidUtf8(bytes.data(), n)) {
        std::wstring text;
        if (DecodeMultiByte(bytes.data(), n, CP_UTF8, true, text))
            return finish(TextEncoding::Utf8, std::move(text), 95);
    }

    // BOM-less UTF-16: lots of zero bytes in one byte lane.
    if (n >= 4) {
        size_t sample = (std::min)(n, (size_t)4000);
        size_t zEven = 0, zOdd = 0;
        for (size_t i = 0; i < sample; i++) {
            if (bytes[i] == 0) { if ((i & 1) == 0) zEven++; else zOdd++; }
        }
        if (zOdd > sample / 6 && zEven == 0) {
            std::wstring text;
            DecodeUtf16Bytes(bytes, true, text);
            return finish(TextEncoding::Utf16LE, std::move(text), 75);
        }
        if (zEven > sample / 6 && zOdd == 0) {
            std::wstring text;
            DecodeUtf16Bytes(bytes, false, text);
            return finish(TextEncoding::Utf16BE, std::move(text), 75);
        }
    }

    if (IsValidUtf8(bytes.data(), n)) {
        std::wstring text;
        if (DecodeMultiByte(bytes.data(), n, CP_UTF8, true, text))
            return finish(TextEncoding::Utf8, std::move(text), 95);
    }

    // Legacy encodings: decode a sample with each candidate and score the
    // result. This is deliberately a heuristic; the user can always override
    // the choice from the encoding menu.
    size_t sampleSize = (std::min)(n, (size_t)256 * 1024);
    std::vector<uint8_t> sample(bytes.begin(), bytes.begin() + sampleSize);
    std::vector<TextEncoding> candidates = CandidateEncodings(language);
    TextEncoding best = TextEncoding::Latin1;
    double bestScore = -1e9, secondScore = -1e9;
    for (size_t i = 0; i < candidates.size(); i++) {
        std::wstring sampleText;
        if (!DecodeAsEncoding(sample, candidates[i], false, sampleText)) continue;
        double score = TextQuality(sampleText, language) + EncodingPrior(candidates[i], language) +
                       (double)(candidates.size() - i) * 0.01;
        if (score > bestScore) {
            secondScore = bestScore;
            bestScore = score;
            best = candidates[i];
        } else if (score > secondScore) {
            secondScore = score;
        }
    }

    std::wstring text;
    bool strictOk = DecodeAsEncoding(bytes, best, true, text);
    if (!strictOk) DecodeAsEncoding(bytes, best, false, text);
    int confidence = 55;
    if (secondScore > -1e8)
        confidence = (int)std::lround(50.0 + (bestScore - secondScore) * 3.0);
    confidence = (std::max)(10, (std::min)(confidence, 92));
    if (!strictOk) confidence = (std::min)(confidence, 30);
    result = finish(best, std::move(text), confidence);
    if (!strictOk || result.confidence < 40) result.uncertain = true;
    return result;
}

std::wstring DecodeBytes(const std::vector<uint8_t>& bytes, std::wstring& encodingName) {
    DecodedText decoded = DecodeText(bytes, TextEncoding::Auto, DocLanguage::Auto, false);
    encodingName = decoded.encodingName;
    return decoded.text;
}

std::wstring NormalizeNewlines(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\r') {
            if (i + 1 < s.size() && s[i + 1] == L'\n') i++;
            out += L'\n';
        } else out += s[i];
    }
    return out;
}

static bool IsSpace(wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; }
std::wstring TrimLeft(const std::wstring& s) {
    size_t i = 0;
    while (i < s.size() && IsSpace(s[i])) i++;
    return s.substr(i);
}
std::wstring TrimRight(const std::wstring& s) {
    size_t n = s.size();
    while (n > 0 && IsSpace(s[n - 1])) n--;
    return s.substr(0, n);
}
std::wstring Trim(const std::wstring& s) { return TrimRight(TrimLeft(s)); }

std::wstring ToLower(const std::wstring& s) {
    std::wstring out = s;
    for (auto& c : out) c = (wchar_t)towlower(c);
    return out;
}
bool StartsWithI(const std::wstring& s, const std::wstring& p) {
    if (p.size() > s.size()) return false;
    return _wcsnicmp(s.c_str(), p.c_str(), p.size()) == 0;
}
bool EndsWithI(const std::wstring& s, const std::wstring& p) {
    if (p.size() > s.size()) return false;
    return _wcsnicmp(s.c_str() + s.size() - p.size(), p.c_str(), p.size()) == 0;
}
bool ContainsI(const std::wstring& s, const std::wstring& p) {
    if (p.empty()) return true;
    std::wstring ls = ToLower(s), lp = ToLower(p);
    return ls.find(lp) != std::wstring::npos;
}
std::wstring ReplaceAll(std::wstring s, const std::wstring& from, const std::wstring& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::wstring::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}
std::vector<std::wstring> SplitLines(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : NormalizeNewlines(s)) {
        if (c == L'\n') { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}
std::wstring Shorten(const std::wstring& s, size_t max) {
    std::wstring t = ReplaceAll(ReplaceAll(s, L"\r", L" "), L"\n", L"\\n");
    t = ReplaceAll(t, L"\t", L" ");
    if (t.size() <= max) return t;
    return t.substr(0, max) + L"…";
}
std::wstring TrimToMiddle(const std::wstring& s, size_t max) {
    if (s.size() <= max || max < 4) return s;
    size_t keep = (max - 1) / 2;
    return s.substr(0, keep) + L"…" + s.substr(s.size() - keep);
}
std::wstring FormatBytes(uint64_t bytes) {
    wchar_t buf[64];
    if (bytes < 1024) swprintf(buf, 64, L"%llu B", (unsigned long long)bytes);
    else if (bytes < 1024ull * 1024) swprintf(buf, 64, L"%.1f KB", bytes / 1024.0);
    else swprintf(buf, 64, L"%.2f MB", bytes / (1024.0 * 1024.0));
    return buf;
}
int CountLines(const std::wstring& s) {
    if (s.empty()) return 0;
    int n = 1;
    for (wchar_t c : s) if (c == L'\n') n++;
    if (!s.empty() && s.back() == L'\n') n--;
    return n;
}
std::wstring FormatFileTime(uint64_t ft) {
    if (!ft) return L"";
    FILETIME f;
    f.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFF);
    f.dwHighDateTime = (DWORD)(ft >> 32);
    FILETIME local;
    FileTimeToLocalFileTime(&f, &local);
    SYSTEMTIME st;
    FileTimeToSystemTime(&local, &st);
    wchar_t buf[64];
    swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

// ------------------------------------------------------------------- files
bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out, std::wstring* error) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = L"无法打开文件（错误 " + std::to_wstring(GetLastError()) + L"）";
        return false;
    }
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) {
        CloseHandle(h);
        if (error) *error = L"无法读取文件大小";
        return false;
    }
    if (sz.QuadPart > 512ll * 1024 * 1024) {
        CloseHandle(h);
        if (error) *error = L"文件超过 512 MB，已停止读取";
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    size_t done = 0;
    while (done < out.size()) {
        DWORD got = 0;
        DWORD want = (DWORD)(std::min)(out.size() - done, (size_t)1 << 24);
        if (!ReadFile(h, out.data() + done, want, &got, nullptr)) {
            CloseHandle(h);
            if (error) *error = L"读取文件失败";
            out.clear();
            return false;
        }
        if (got == 0) break;
        done += got;
    }
    CloseHandle(h);
    out.resize(done);
    return true;
}

bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& data, std::wstring* error) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error) *error = L"无法写入文件（错误 " + std::to_wstring(GetLastError()) + L"）";
        return false;
    }
    size_t done = 0;
    while (done < data.size()) {
        DWORD wrote = 0;
        DWORD want = (DWORD)(std::min)(data.size() - done, (size_t)1 << 24);
        if (!WriteFile(h, data.data() + done, want, &wrote, nullptr)) {
            CloseHandle(h);
            if (error) *error = L"写入文件失败";
            return false;
        }
        done += wrote;
    }
    CloseHandle(h);
    return true;
}

bool WriteTextFile(const std::wstring& path, const std::wstring& text) {
    std::string u = Utf8FromWide(text);
    std::vector<uint8_t> data(u.begin(), u.end());
    return WriteFileBytes(path, data);
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool DirExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool EnsureDir(const std::wstring& path) {
    if (path.empty()) return false;
    if (DirExists(path)) return true;
    std::wstring parent = PathDir(path);
    if (!parent.empty() && parent != path && !DirExists(parent)) EnsureDir(parent);
    return CreateDirectoryW(path.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS;
}
void RemoveTree(const std::wstring& path) {
    if (path.empty()) return;
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW(PathJoin(path, L"*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring child = PathJoin(path, name);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                RemoveTree(child);
            } else {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(path.c_str());
}
uint64_t FileWriteTimeUtc(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return 0;
    return ((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
}
std::wstring ExePath() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, &buf[0], (DWORD)buf.size());
    buf.resize(n);
    return buf;
}
std::wstring ExeDir() { return PathDir(ExePath()); }
std::wstring LocalAppDataDir() {
    wchar_t* p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p)) && p) {
        out = p;
        CoTaskMemFree(p);
    }
    return out;
}

std::wstring LocalDataDir() {
    std::wstring base = LocalAppDataDir();
    if (base.empty()) return L"";
    std::wstring fresh = PathJoin(base, L"lithemd");
    std::wstring legacy = PathJoin(base, L"openmd");
    if (DirExists(fresh)) return fresh;
    if (DirExists(legacy)) {
        if (MoveFileExW(legacy.c_str(), fresh.c_str(), MOVEFILE_COPY_ALLOWED)) {
            std::wstring oldNotes = PathJoin(fresh, L"openmd-notes");
            std::wstring newNotes = PathJoin(fresh, L"lithemd-notes");
            if (DirExists(oldNotes) && !DirExists(newNotes))
                MoveFileExW(oldNotes.c_str(), newNotes.c_str(), 0);
            return fresh;
        }
        if (DirExists(fresh)) return fresh;
        return legacy;
    }
    EnsureDir(fresh);
    return fresh;
}
std::wstring PathDir(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return L"";
    if (p == 2 && path.size() > 1 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, p);
}
std::wstring PathName(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}
std::wstring PathExt(const std::wstring& path) {
    std::wstring name = PathName(path);
    size_t p = name.find_last_of(L'.');
    if (p == std::wstring::npos || p == 0) return L"";
    return ToLower(name.substr(p));
}
std::wstring PathJoin(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    wchar_t last = a.back();
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}
std::wstring AbsolutePath(const std::wstring& path) {
    wchar_t buf[32768];
    DWORD n = GetFullPathNameW(path.c_str(), 32768, buf, nullptr);
    if (!n || n >= 32768) return path;
    return std::wstring(buf, n);
}
std::wstring TempPath(const std::wstring& name) {
    wchar_t buf[MAX_PATH + 2];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    if (!n) return name;
    return PathJoin(std::wstring(buf, n), name);
}

// ------------------------------------------------------------------- misc
static int B64Value(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return c - L'A';
    if (c >= L'a' && c <= L'z') return c - L'a' + 26;
    if (c >= L'0' && c <= L'9') return c - L'0' + 52;
    if (c == L'+') return 62;
    if (c == L'/') return 63;
    return -1;
}
std::wstring Base64Decode(const std::wstring& text, bool* ok) {
    std::vector<uint8_t> data = Base64DecodeBytes(text);
    if (ok) *ok = !data.empty() || text.empty();
    std::wstring out;
    out.reserve(data.size() / 2);
    for (size_t i = 0; i + 1 < data.size(); i += 2)
        out += (wchar_t)((data[i] << 8) | data[i + 1]);
    if (data.size() & 1) out += (wchar_t)data.back();
    return out;
}
std::vector<uint8_t> Base64DecodeBytes(const std::wstring& text) {
    std::vector<uint8_t> out;
    int val = 0, bits = -8;
    for (wchar_t c : text) {
        if (c == L'=' ) break;
        int v = B64Value(c);
        if (v < 0) {
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') continue;
            continue;
        }
        val = (val << 6) | v;
        bits += 6;
        if (bits >= 0) {
            out.push_back((uint8_t)((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}
bool IsWebUrl(const std::wstring& url) {
    return StartsWithI(url, L"http://") || StartsWithI(url, L"https://") || StartsWithI(url, L"mailto:");
}
void OpenExternal(const std::wstring& pathOrUrl) {
    if (pathOrUrl.empty()) return;
    ShellExecuteW(nullptr, L"open", pathOrUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

namespace {
using SetPreferredAppModeFn = int(WINAPI*)(int);
using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);

HMODULE UxThemeModule() {
    static HMODULE module = LoadLibraryW(L"uxtheme.dll");
    return module;
}

SetPreferredAppModeFn PreferredAppModeFn() {
    static auto fn = (SetPreferredAppModeFn)GetProcAddress(UxThemeModule(), MAKEINTRESOURCEA(135));
    return fn;
}

AllowDarkModeForWindowFn AllowDarkModeFn() {
    static auto fn = (AllowDarkModeForWindowFn)GetProcAddress(UxThemeModule(), MAKEINTRESOURCEA(133));
    return fn;
}
} // namespace

void EnableDarkModeForApp() {
    static bool done = false;
    if (done) return;
    done = true;
    if (auto fn = PreferredAppModeFn()) fn(1); // AllowDark
}

void ApplyDarkWindowTheme(HWND hwnd, bool dark) {
    if (!hwnd) return;
    EnableDarkModeForApp();
    if (auto allow = AllowDarkModeFn()) allow(hwnd, dark ? TRUE : FALSE);
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
}
bool SetClipboardText(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) { CloseClipboard(); return false; }
    void* p = GlobalLock(h);
    if (!p) { GlobalFree(h); CloseClipboard(); return false; }
    memcpy(p, text.c_str(), bytes);
    GlobalUnlock(h);
    if (!SetClipboardData(CF_UNICODETEXT, h)) {
        GlobalFree(h);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}
std::wstring GetClipboardText(HWND owner) {
    if (!OpenClipboard(owner)) return L"";
    std::wstring out;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
        const wchar_t* p = (const wchar_t*)GlobalLock(h);
        if (p) { out = p; GlobalUnlock(h); }
    }
    CloseClipboard();
    return out;
}
std::wstring GuidString() {
    GUID g{};
    if (FAILED(CoCreateGuid(&g))) return L"";
    wchar_t buf[64];
    StringFromGUID2(g, buf, 64);
    return buf;
}
uint64_t UnixNowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) / 10000ull;
}

// ---------------------------------------------------------------- palette
Color HexColor(const wchar_t* hex, float alpha) {
    if (!hex || !*hex) return Color(0, 0, 0, alpha);
    if (*hex == L'#') hex++;
    auto nib = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return 0;
    };
    unsigned v = 0;
    for (int i = 0; i < 6 && hex[i]; i++) v = (v << 4) | nib(hex[i]);
    return Color(((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f, alpha);
}
Color MixColor(const Color& a, const Color& b, float t) {
    return Color(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                 a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

std::vector<Color> DefaultInkPresets() {
    return {
        HexColor(L"#E5484D"), HexColor(L"#F5A524"), HexColor(L"#30A46C"),
        HexColor(L"#3E63DD"), HexColor(L"#1F2328")
    };
}

Color ParseColorText(const std::wstring& text, const Color& fallback) {
    std::wstring s = Trim(text);
    if (!s.empty() && s[0] == L'#') s.erase(s.begin());
    if (s.size() != 6 && s.size() != 8) return fallback;
    auto nib = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    unsigned v = 0;
    for (wchar_t c : s) {
        int n = nib(c);
        if (n < 0) return fallback;
        v = (v << 4) | (unsigned)n;
    }
    if (s.size() == 8) {
        return Color(((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f,
                     (v & 0xFF) / 255.0f, ((v >> 24) & 0xFF) / 255.0f);
    }
    return Color(((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f,
                 (v & 0xFF) / 255.0f, 1.0f);
}

std::wstring ColorToHex(const Color& color, bool withAlpha) {
    auto byte = [](float v) { return (unsigned)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    wchar_t buf[16];
    if (withAlpha)
        swprintf(buf, 16, L"#%02X%02X%02X%02X", byte(color.a), byte(color.r), byte(color.g), byte(color.b));
    else
        swprintf(buf, 16, L"#%02X%02X%02X", byte(color.r), byte(color.g), byte(color.b));
    return buf;
}

static Palette MakeLight() {
    Palette p;
    p.dark = false;
    p.bg = HexColor(L"#FFFFFF");
    p.surface = HexColor(L"#F6F8FA");
    p.fg = HexColor(L"#1F2328");
    p.muted = HexColor(L"#59636E");
    p.border = HexColor(L"#D8DEE4");
    p.codeBg = HexColor(L"#F6F8FA");
    p.accent = HexColor(L"#0969DA");
    p.accentSoft = HexColor(L"#DDF4FF");
    p.hover = HexColor(L"#EEF1F4");
    p.tableHead = HexColor(L"#F6F8FA");
    p.danger = HexColor(L"#CF222E");
    p.dangerSoft = HexColor(L"#FFEBE9");
    p.quoteBar = HexColor(L"#D0D7DE");
    p.selection = HexColor(L"#0969DA", 0.22f);
    p.codeKeyword = HexColor(L"#CF222E");
    p.codeString = HexColor(L"#0A3069");
    p.codeNumber = HexColor(L"#0550AE");
    p.codeComment = HexColor(L"#6E7781");
    p.codeType = HexColor(L"#8250DF");
    p.codePunct = HexColor(L"#57606A");
    p.jsonKey = HexColor(L"#0550AE");
    p.jsonString = HexColor(L"#0A3069");
    p.jsonNumber = HexColor(L"#0550AE");
    p.jsonBool = HexColor(L"#CF222E");
    p.jsonNull = HexColor(L"#6E7781");
    return p;
}
static Palette MakeDark() {
    Palette p;
    p.dark = true;
    p.bg = HexColor(L"#0D1117");
    p.surface = HexColor(L"#161B22");
    p.fg = HexColor(L"#E6EDF3");
    p.muted = HexColor(L"#9198A1");
    p.border = HexColor(L"#30363D");
    p.codeBg = HexColor(L"#161B22");
    p.accent = HexColor(L"#4493F8");
    p.accentSoft = HexColor(L"#1C2C40");
    p.hover = HexColor(L"#1F242C");
    p.tableHead = HexColor(L"#161B22");
    p.danger = HexColor(L"#F85149");
    p.dangerSoft = HexColor(L"#2D1416");
    p.quoteBar = HexColor(L"#30363D");
    p.selection = HexColor(L"#4493F8", 0.32f);
    p.codeKeyword = HexColor(L"#FF7B72");
    p.codeString = HexColor(L"#A5D6FF");
    p.codeNumber = HexColor(L"#79C0FF");
    p.codeComment = HexColor(L"#8B949E");
    p.codeType = HexColor(L"#D2A8FF");
    p.codePunct = HexColor(L"#C9D1D9");
    p.jsonKey = HexColor(L"#79C0FF");
    p.jsonString = HexColor(L"#7EE787");
    p.jsonNumber = HexColor(L"#79C0FF");
    p.jsonBool = HexColor(L"#FF7B72");
    p.jsonNull = HexColor(L"#8B949E");
    return p;
}
static Palette MakeWarm() {
    Palette p;
    p.dark = false;
    p.bg = HexColor(L"#FBF7F0");
    p.surface = HexColor(L"#F3EDE3");
    p.fg = HexColor(L"#2B2622");
    p.muted = HexColor(L"#6B6258");
    p.border = HexColor(L"#DED3C4");
    p.codeBg = HexColor(L"#F3EDE3");
    p.accent = HexColor(L"#A65B1E");
    p.accentSoft = HexColor(L"#F5E3D2");
    p.hover = HexColor(L"#F0E7DB");
    p.tableHead = HexColor(L"#F3EDE3");
    p.danger = HexColor(L"#B3261E");
    p.dangerSoft = HexColor(L"#FBE9E7");
    p.quoteBar = HexColor(L"#D8C9B8");
    p.selection = HexColor(L"#A65B1E", 0.22f);
    p.codeKeyword = HexColor(L"#9A3412");
    p.codeString = HexColor(L"#5B4636");
    p.codeNumber = HexColor(L"#8A4B08");
    p.codeComment = HexColor(L"#8A8175");
    p.codeType = HexColor(L"#7C4A03");
    p.codePunct = HexColor(L"#6B6258");
    p.jsonKey = HexColor(L"#8A4B08");
    p.jsonString = HexColor(L"#3F5D3A");
    p.jsonNumber = HexColor(L"#8A4B08");
    p.jsonBool = HexColor(L"#B3261E");
    p.jsonNull = HexColor(L"#8A8175");
    return p;
}
static Palette MakeGreen() {
    Palette p;
    p.dark = false;
    p.bg = HexColor(L"#F2F8F2");
    p.surface = HexColor(L"#E4F0E4");
    p.fg = HexColor(L"#1E2B1E");
    p.muted = HexColor(L"#5A6B5A");
    p.border = HexColor(L"#C3D8C3");
    p.codeBg = HexColor(L"#E4F0E4");
    p.accent = HexColor(L"#2D7A46");
    p.accentSoft = HexColor(L"#CDE8D5");
    p.hover = HexColor(L"#D7E9D7");
    p.tableHead = HexColor(L"#E4F0E4");
    p.danger = HexColor(L"#B3261E");
    p.dangerSoft = HexColor(L"#F8E3E1");
    p.quoteBar = HexColor(L"#B8D2B8");
    p.selection = HexColor(L"#2D7A46", 0.22f);
    p.codeKeyword = HexColor(L"#1F6F43");
    p.codeString = HexColor(L"#3D5C3D");
    p.codeNumber = HexColor(L"#2A6B52");
    p.codeComment = HexColor(L"#708070");
    p.codeType = HexColor(L"#4A6B2D");
    p.codePunct = HexColor(L"#4F6350");
    p.jsonKey = HexColor(L"#1F6F43");
    p.jsonString = HexColor(L"#3D5C3D");
    p.jsonNumber = HexColor(L"#2A6B52");
    p.jsonBool = HexColor(L"#B3261E");
    p.jsonNull = HexColor(L"#708070");
    return p;
}
const Palette& Palette::Light() { static Palette p = MakeLight(); return p; }
const Palette& Palette::Dark() { static Palette p = MakeDark(); return p; }
const Palette& Palette::Warm() { static Palette p = MakeWarm(); return p; }
const Palette& Palette::Green() { static Palette p = MakeGreen(); return p; }
const Palette& Palette::Get(bool dark) { return dark ? Dark() : Light(); }
const Palette& Palette::Get(const std::wstring& theme) {
    std::wstring id = ToLower(Trim(theme));
    if (id == L"dark") return Dark();
    if (id == L"warm") return Warm();
    if (id == L"green") return Green();
    return Light();
}

// ---------------------------------------------------------------- doc kind
std::wstring DocKindName(DocKind kind) {
    switch (kind) {
    case DocKind::Markdown: return L"Markdown";
    case DocKind::Json: return L"JSON";
    case DocKind::Notebook: return L"Jupyter Notebook";
    case DocKind::Chat: return L"对话记录";
    case DocKind::Csv: return L"CSV 表格";
    case DocKind::Text: return L"纯文本";
    case DocKind::Docx: return L"Word 文档";
    case DocKind::Doc: return L"Word 97 文档";
    case DocKind::Xlsx: return L"Excel 工作簿";
    case DocKind::Xls: return L"Excel 97 工作簿";
    case DocKind::Pdf: return L"PDF 文档";
    case DocKind::Image: return L"图片";
    default: return L"文档";
    }
}
bool IsBinaryKind(DocKind kind) {
    return kind == DocKind::Docx || kind == DocKind::Doc || kind == DocKind::Xlsx ||
           kind == DocKind::Xls || kind == DocKind::Pdf || kind == DocKind::Image;
}
bool CanShowSource(DocKind kind) {
    switch (kind) {
    case DocKind::Markdown:
    case DocKind::Json:
    case DocKind::Notebook:
    case DocKind::Chat:
    case DocKind::Csv:
    case DocKind::Text: return true;
    case DocKind::Image: return false;
    default: return false;
    }
}

static bool LooksLikeCsv(const std::wstring& text) {
    std::wstring t = NormalizeNewlines(text);
    std::wstring first = t.substr(0, std::min<size_t>(t.size(), 4000));
    size_t lines = 0;
    for (auto& line : SplitLines(first)) {
        if (Trim(line).empty()) continue;
        lines++;
        if (lines > 4) break;
    }
    if (lines < 2) return false;
    // Count separators on the first two non-empty lines.
    std::vector<std::wstring> ls = SplitLines(first);
    std::wstring a, b;
    for (auto& l : ls) { if (!Trim(l).empty()) { if (a.empty()) a = l; else { b = l; break; } } }
    if (a.empty() || b.empty()) return false;
    auto count = [](const std::wstring& s, wchar_t c) {
        int n = 0; bool q = false;
        for (wchar_t x : s) { if (x == L'"') q = !q; else if (!q && x == c) n++; }
        return n;
    };
    for (wchar_t sep : { L',', L'\t', L';' }) {
        int ca = count(a, sep), cb = count(b, sep);
        if (ca > 0 && cb > 0) return true;
    }
    return false;
}

DocKind DetectKind(const std::wstring& path, const std::wstring& text) {
    std::wstring ext = PathExt(path);
    if (ext == L".md" || ext == L".markdown" || ext == L".mdown" || ext == L".mkd" || ext == L".mdx") {
        // Some tools export an OpenCode transcript with a .md suffix. If it is
        // actually the JSON session container, show it in chat view.
        std::wstring t = Trim(text);
        if (!t.empty() && t[0] == L'{') {
            JNode root;
            if (ParseJson(text, root)) {
                const JNode* info = root.Get(L"info");
                const JNode* msgs = root.Get(L"messages");
                if (info && info->kind == JKind::Object && msgs && msgs->kind == JKind::Array) {
                    const JNode* id = info->Get(L"id");
                    if (id && id->kind == JKind::String && StartsWithI(id->str, L"ses_")) return DocKind::Chat;
                }
            }
        }
        return DocKind::Markdown;
    }
    if (ext == L".ipynb") return DocKind::Notebook;
    if (ext == L".csv" || ext == L".tsv") return DocKind::Csv;
    if (ext == L".docx") return DocKind::Docx;
    if (ext == L".doc") return DocKind::Doc;
    if (ext == L".xlsx") return DocKind::Xlsx;
    if (ext == L".xls") return DocKind::Xls;
    if (ext == L".pdf") return DocKind::Pdf;
    if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".jfif" ||
        ext == L".gif" || ext == L".bmp" || ext == L".tif" || ext == L".tiff" ||
        ext == L".ico" || ext == L".webp")
        return DocKind::Image;
    if (ext == L".json" || ext == L".jsonc" || ext == L".geojson" || ext == L".webmanifest" || ext == L".json5") {
        JNode root;
        if (ParseJson(text, root)) {
            const JNode* info = root.Get(L"info");
            const JNode* msgs = root.Get(L"messages");
            if (info && info->kind == JKind::Object && msgs && msgs->kind == JKind::Array) {
                const JNode* id = info->Get(L"id");
                if (id && id->kind == JKind::String && StartsWithI(id->str, L"ses_")) return DocKind::Chat;
                bool hasChat = false;
                for (auto& m : msgs->items) {
                    const JNode* t = m.Get(L"type");
                    if (t && t->kind == JKind::String && (t->str == L"user" || t->str == L"assistant")) hasChat = true;
                }
                if (hasChat && info->Get(L"title")) return DocKind::Chat;
            }
        }
        return DocKind::Json;
    }
    if (ext == L".txt" || ext == L".log" || ext == L".yml" || ext == L".yaml" ||
        ext == L".xml" || ext == L".ini" || ext == L".toml") return DocKind::Text;

    std::wstring t = Trim(text);
    if (!t.empty() && (t[0] == L'{' || t[0] == L'[')) {
        JNode root;
        if (ParseJson(text, root)) {
            const JNode* info = root.Get(L"info");
            const JNode* msgs = root.Get(L"messages");
            if (info && msgs && info->kind == JKind::Object && msgs->kind == JKind::Array) {
                const JNode* id = info->Get(L"id");
                if (id && id->kind == JKind::String && StartsWithI(id->str, L"ses_")) return DocKind::Chat;
            }
            return DocKind::Json;
        }
    }
    if (StartsWithI(t, L"#") || t.find(L"\n#") != std::wstring::npos) return DocKind::Markdown;
    if (LooksLikeCsv(text)) return DocKind::Csv;
    return DocKind::Text;
}

// ---------------------------------------------------------------- settings
std::wstring Settings::ResolveConfigPath() {
    std::wstring portable = PathJoin(ExeDir(), L"config.json");
    if (FileExists(portable)) return portable;
    std::wstring dir = LocalDataDir();
    return PathJoin(dir, L"config.json");
}

static std::wstring NodeText(const JNode* n) {
    return n ? n->Text() : L"";
}

void Settings::Clamp() {
    if (!(zoom >= 0.6 && zoom <= 2.5)) zoom = 1.0;
    if (!(fontScale >= MinFontScale && fontScale <= MaxFontScale)) fontScale = 1.0;
    fontScale = std::round(fontScale * 100.0) / 100.0;
    contentWidth = std::clamp(contentWidth, MinContentWidth, MaxContentWidth);
    if (wheelStep < MinWheelStep || wheelStep > MaxWheelStep) wheelStep = 3;
    backgroundImage = Trim(backgroundImage);
    backgroundOpacity = std::clamp(backgroundOpacity, MinBackgroundOpacity, MaxBackgroundOpacity);
    backgroundFit = std::clamp(backgroundFit, MinBackgroundFit, MaxBackgroundFit);
    backgroundAnchor = std::clamp(backgroundAnchor, MinBackgroundAnchor, MaxBackgroundAnchor);
    inkToolsVisible &= 0x7F;
    canvasWidth = std::clamp(canvasWidth, MinCanvasSize, MaxCanvasSize);
    canvasHeight = std::clamp(canvasHeight, MinCanvasSize, MaxCanvasSize);
    while (uiLanguage.size() >= 2 &&
           ((uiLanguage.front() == L'"' && uiLanguage.back() == L'"') ||
            (uiLanguage.front() == L'\'' && uiLanguage.back() == L'\'')))
        uiLanguage = uiLanguage.substr(1, uiLanguage.size() - 2);
    uiLanguage = Trim(uiLanguage);
    if (uiLanguage.empty()) uiLanguage = L"zh-CN";
    theme = ToLower(Trim(theme));
    if (theme.empty() || theme == L"light") theme = L"blue";
    if (theme != L"blue" && theme != L"dark" && theme != L"warm" && theme != L"green")
        theme = L"blue";
    dark = theme == L"dark";
    for (auto& c : customColors) {
        c.r = std::clamp(c.r, 0.0f, 1.0f);
        c.g = std::clamp(c.g, 0.0f, 1.0f);
        c.b = std::clamp(c.b, 0.0f, 1.0f);
        c.a = std::clamp(c.a, 0.0f, 1.0f);
    }
    if (inkPresets.empty()) inkPresets = DefaultInkPresets();
    if (inkPresets.size() > 12) inkPresets.resize(12);
    for (auto& c : inkPresets) {
        c.r = std::clamp(c.r, 0.0f, 1.0f);
        c.g = std::clamp(c.g, 0.0f, 1.0f);
        c.b = std::clamp(c.b, 0.0f, 1.0f);
        c.a = std::clamp(c.a, 0.0f, 1.0f);
    }
    if (inkToolbarPresets.empty()) inkToolbarPresets.assign({ 0, 1, 2, 3, 4 });
    if (inkToolbarPresets.size() > 5) inkToolbarPresets.resize(5);
    std::vector<int> used;
    for (auto& index : inkToolbarPresets) {
        if (index < 0 || index >= (int)inkPresets.size() ||
            std::find(used.begin(), used.end(), index) != used.end()) {
            index = -1;
            for (int candidate = 0; candidate < (int)inkPresets.size(); candidate++) {
                if (std::find(used.begin(), used.end(), candidate) == used.end()) {
                    index = candidate;
                    break;
                }
            }
        }
        if (index < 0) index = 0;
        used.push_back(index);
    }
    if ((int)documentLanguage < (int)DocLanguage::Auto ||
        (int)documentLanguage > (int)DocLanguage::Western) documentLanguage = DocLanguage::Auto;
    while (recent.size() > MaxRecent) recent.pop_back();
    while (recentBlocked.size() > MaxRecent) recentBlocked.erase(recentBlocked.begin());
    while (workspaceHistory.size() > MaxWorkspaceHistory) workspaceHistory.pop_back();
}

Settings Settings::Load() {
    Settings s;
    s.configPath = ResolveConfigPath();
    bool loaded = false;
    bool presetsLoaded = false;
    bool hadCustomColors = false;
    bool themeLoaded = false;
    std::wstring cfgText;
    std::vector<uint8_t> bytes;
    if (FileExists(s.configPath) && ReadFileBytes(s.configPath, bytes)) {
        std::wstring enc;
        cfgText = DecodeBytes(bytes, enc);
        JNode root;
        if (ParseJson(cfgText, root) && root.kind == JKind::Object) {
            loaded = true;
            for (auto& m : root.members) {
                std::wstring key = ToLower(m.first);
                if (key == L"theme" || key == L"themeid") {
                    std::wstring value = ToLower(Trim(NodeText(&m.second)));
                    if (value == L"light") value = L"blue";
                    if (value == L"dark" || value == L"blue" || value == L"warm" || value == L"green") {
                        s.theme = value;
                        s.dark = value == L"dark";
                        themeLoaded = true;
                    }
                }
                else if (key == L"dark") {
                    std::wstring v = ToLower(NodeText(&m.second));
                    s.dark = (v == L"true" || v == L"1");
                }
                else if (key == L"zoom") s.zoom = m.second.number > 0 ? m.second.number : 1.0;
                else if (key == L"fontscale" || key == L"bodyscale")
                    s.fontScale = m.second.number > 0 ? m.second.number : 1.0;
                else if (key == L"contentwidth" || key == L"pagewidth")
                    s.contentWidth = (int)m.second.number;
                else if ((key == L"workspacehistory" || key == L"workspaces") &&
                         m.second.kind == JKind::Array) {
                    s.workspaceHistory.clear();
                    for (auto& it : m.second.items) {
                        std::wstring v = it.Text();
                        if (!v.empty()) s.workspaceHistory.push_back(v);
                    }
                }
                else if (key == L"workspaceroot") s.workspaceRoot = NodeText(&m.second);
                else if (key == L"workspacevisible") {
                    std::wstring v = ToLower(NodeText(&m.second));
                    s.workspaceVisible = (v == L"true" || v == L"1");
                }
                else if (key == L"backgroundimage" || key == L"background")
                    s.backgroundImage = NodeText(&m.second);
                else if (key == L"backgroundopacity" || key == L"backgroundalpha")
                    s.backgroundOpacity = (int)m.second.number;
                else if (key == L"backgroundfit" || key == L"backgroundmode")
                    s.backgroundFit = (int)m.second.number;
                else if (key == L"backgroundanchor" || key == L"backgroundposition")
                    s.backgroundAnchor = (int)m.second.number;
                else if (key == L"wheelstep" || key == L"wheelsteps") s.wheelStep = (int)m.second.number;
                else if (key == L"inktools" || key == L"inktoolsvisible")
                    s.inkToolsVisible = (int)m.second.number;
                else if (key == L"canvaswidth") s.canvasWidth = (int)m.second.number;
                else if (key == L"canvasheight") s.canvasHeight = (int)m.second.number;
                else if (key == L"canvasdark") {
                    std::wstring v = ToLower(NodeText(&m.second));
                    s.canvasDark = (v == L"true" || v == L"1");
                }
                else if (key == L"customcolor1" || key == L"customcolor2") {
                    int idx = key.back() == L'1' ? 0 : 1;
                    s.customColors[idx] = ParseColorText(NodeText(&m.second), s.customColors[idx]);
                    hadCustomColors = true;
                }
                else if ((key == L"inkpresets" || key == L"inkcolors") &&
                         m.second.kind == JKind::Array) {
                    s.inkPresets.clear();
                    for (auto& item : m.second.items) {
                        if (item.kind != JKind::String) continue;
                        Color color = ParseColorText(item.str, Color(0, 0, 0, 0));
                        if (color.a > 0) s.inkPresets.push_back(color);
                        if (s.inkPresets.size() >= 12) break;
                    }
                    presetsLoaded = true;
                }
                else if ((key == L"inktoolbarpresets" || key == L"inktoolbarcolors") &&
                         m.second.kind == JKind::Array) {
                    s.inkToolbarPresets.clear();
                    for (auto& item : m.second.items)
                        if (item.kind == JKind::Number) s.inkToolbarPresets.push_back((int)item.number);
                }
                else if (key == L"uilanguage" || key == L"interfacelanguage")
                    s.uiLanguage = NodeText(&m.second);
                else if (key == L"documentlanguage" || key == L"doclanguage" || key == L"language")
                    s.documentLanguage = ParseLanguage(NodeText(&m.second));
                else if (key == L"shortcuts" && m.second.kind == JKind::Object) {
                    s.shortcuts.clear();
                    for (auto& sm : m.second.members) {
                        std::wstring cmd = ToLower(Trim(sm.first));
                        if (cmd.empty()) continue;
                        std::vector<ShortcutKey> keys;
                        if (sm.second.kind == JKind::Array) {
                            for (auto& item : sm.second.items) {
                                ShortcutKey keyValue;
                                if (item.kind == JKind::String && ParseShortcut(item.Text(), keyValue))
                                    keys.push_back(keyValue);
                            }
                        } else if (sm.second.kind == JKind::String) {
                            ShortcutKey keyValue;
                            if (ParseShortcut(sm.second.Text(), keyValue)) keys.push_back(keyValue);
                        }
                        s.shortcuts[cmd] = std::move(keys);
                    }
                }
                else if (key == L"recent" && m.second.kind == JKind::Array) {
                    s.recent.clear();
                    for (auto& it : m.second.items) {
                        std::wstring v = it.Text();
                        if (!v.empty()) s.recent.push_back(v);
                    }
                }
                else if ((key == L"recentblocked" || key == L"recentdeleted") &&
                         m.second.kind == JKind::Array) {
                    s.recentBlocked.clear();
                    for (auto& it : m.second.items) {
                        std::wstring v = it.Text();
                        if (!v.empty()) s.recentBlocked.push_back(v);
                    }
                }
            }
        }
    }
    if (!loaded) {
        std::wstring legacy = PathJoin(LocalDataDir(), L"state.ini");
        std::vector<uint8_t> lb;
        if (FileExists(legacy) && ReadFileBytes(legacy, lb)) {
            std::wstring enc;
            for (auto& line : SplitLines(DecodeBytes(lb, enc))) {
                size_t eq = line.find(L'=');
                if (eq == std::wstring::npos) continue;
                std::wstring k = Trim(line.substr(0, eq));
                std::wstring v = Trim(line.substr(eq + 1));
                if (k == L"dark") s.dark = (v == L"1" || ToLower(v) == L"true");
                else if (k == L"zoom") s.zoom = wcstod(v.c_str(), nullptr);
                else if (k == L"recent" && !v.empty()) s.recent.push_back(v);
            }
        }
    }
    if (!presetsLoaded && hadCustomColors) {
        s.inkPresets = DefaultInkPresets();
        s.inkPresets.push_back(s.customColors[0]);
        s.inkPresets.push_back(s.customColors[1]);
    }
    if (!themeLoaded) s.theme = s.dark ? L"dark" : L"blue";
    s.Clamp();
    if (!FileExists(s.configPath)) s.Save();
    return s;
}

bool Settings::Save() {
    dark = theme == L"dark";
    std::wstring out = L"{\n";
    out += L"  \"theme\": \""; out += dark ? L"dark" : L"light"; out += L"\",\n";
    out += L"  \"themeId\": " + JsonEscape(theme) + L",\n";
    wchar_t z[32];
    swprintf(z, 32, L"%.2f", zoom);
    out += L"  \"zoom\": "; out += z; out += L",\n";
    swprintf(z, 32, L"%.2f", fontScale);
    out += L"  \"fontScale\": "; out += z; out += L",\n";
    out += L"  \"contentWidth\": " + std::to_wstring(contentWidth) + L",\n";
    out += L"  \"workspaceHistory\": [";
    for (size_t i = 0; i < workspaceHistory.size(); i++) {
        if (i) out += L", ";
        out += JsonEscape(workspaceHistory[i]);
    }
    out += L"],\n";
    out += L"  \"workspaceRoot\": " + JsonEscape(workspaceRoot) + L",\n";
    out += std::wstring(L"  \"workspaceVisible\": ") + (workspaceVisible ? L"true" : L"false") + L",\n";
    out += L"  \"backgroundImage\": " + JsonEscape(backgroundImage) + L",\n";
    out += L"  \"backgroundOpacity\": " + std::to_wstring(backgroundOpacity) + L",\n";
    out += L"  \"backgroundFit\": " + std::to_wstring(backgroundFit) + L",\n";
    out += L"  \"backgroundAnchor\": " + std::to_wstring(backgroundAnchor) + L",\n";
    out += L"  \"wheelStep\": " + std::to_wstring(wheelStep) + L",\n";
    out += L"  \"inkToolsVisible\": " + std::to_wstring(inkToolsVisible) + L",\n";
    out += L"  \"canvasWidth\": " + std::to_wstring(canvasWidth) + L",\n";
    out += L"  \"canvasHeight\": " + std::to_wstring(canvasHeight) + L",\n";
    out += std::wstring(L"  \"canvasDark\": ") + (canvasDark ? L"true" : L"false") + L",\n";
    out += L"  \"customColor1\": \"" + ColorToHex(customColors[0]) + L"\",\n";
    out += L"  \"customColor2\": \"" + ColorToHex(customColors[1]) + L"\",\n";
    out += L"  \"inkPresets\": [";
    for (size_t i = 0; i < inkPresets.size(); i++) {
        if (i) out += L", ";
        out += JsonEscape(ColorToHex(inkPresets[i]));
    }
    out += L"],\n";
    out += L"  \"inkToolbarPresets\": [";
    for (size_t i = 0; i < inkToolbarPresets.size(); i++) {
        if (i) out += L", ";
        out += std::to_wstring(inkToolbarPresets[i]);
    }
    out += L"],\n";
    out += L"  \"uiLanguage\": " + JsonEscape(uiLanguage) + L",\n";
    out += L"  \"documentLanguage\": \"" + LanguageConfigName(documentLanguage) + L"\",\n";
    out += L"  \"shortcutsVersion\": 1,\n";
    out += L"  \"shortcuts\": {";
    if (shortcuts.empty()) out += L"},\n";
    else {
        out += L"\n";
        size_t index = 0;
        for (auto& entry : shortcuts) {
            out += L"    " + JsonEscape(entry.first) + L": [";
            for (size_t i = 0; i < entry.second.size(); i++) {
                if (i) out += L", ";
                out += JsonEscape(ShortcutToString(entry.second[i]));
            }
            out += L"]";
            if (++index < shortcuts.size()) out += L",";
            out += L"\n";
        }
        out += L"  },\n";
    }
    out += L"  \"recent\": [";
    if (recent.empty()) out += L"],\n";
    else {
        out += L"\n";
        for (size_t i = 0; i < recent.size(); i++) {
            out += L"    " + JsonEscape(recent[i]);
            if (i + 1 < recent.size()) out += L",";
            out += L"\n";
        }
        out += L"  ],\n";
    }
    out += L"  \"recentBlocked\": [";
    if (recentBlocked.empty()) out += L"]\n";
    else {
        out += L"\n";
        for (size_t i = 0; i < recentBlocked.size(); i++) {
            out += L"    " + JsonEscape(recentBlocked[i]);
            if (i + 1 < recentBlocked.size()) out += L",";
            out += L"\n";
        }
        out += L"  ]\n";
    }
    out += L"}\n";
    std::wstring path = configPath;
    if (path.empty()) path = ResolveConfigPath();
    EnsureDir(PathDir(path));
    if (WriteTextFile(path, out)) { configPath = path; return true; }
    std::wstring fallback = PathJoin(LocalDataDir(), L"config.json");
    if (path != fallback && WriteTextFile(fallback, out)) { configPath = fallback; return true; }
    return false;
}

bool Settings::RecentBlocked(const std::wstring& path) const {
    std::wstring full = AbsolutePath(path);
    for (auto& p : recentBlocked)
        if (_wcsicmp(p.c_str(), full.c_str()) == 0) return true;
    return false;
}

void Settings::AddRecent(const std::wstring& path) {
    std::wstring full = AbsolutePath(path);
    if (RecentBlocked(full)) return;
    recent.erase(std::remove_if(recent.begin(), recent.end(), [&](const std::wstring& p) {
        return _wcsicmp(p.c_str(), full.c_str()) == 0;
    }), recent.end());
    recent.insert(recent.begin(), full);
    while (recent.size() > MaxRecent) recent.pop_back();
    Save();
}

void Settings::RemoveRecent(const std::wstring& path) {
    std::wstring full = AbsolutePath(path);
    recent.erase(std::remove_if(recent.begin(), recent.end(), [&](const std::wstring& p) {
        return _wcsicmp(p.c_str(), full.c_str()) == 0;
    }), recent.end());
    bool known = false;
    for (auto& p : recentBlocked)
        if (_wcsicmp(p.c_str(), full.c_str()) == 0) { known = true; break; }
    if (!known) recentBlocked.push_back(full);
    Save();
}

void Settings::ClearRecent() {
    recent.clear();
    recentBlocked.clear();
    Save();
}

void Settings::AddWorkspace(const std::wstring& path) {
    std::wstring full = AbsolutePath(path);
    if (full.empty()) return;
    workspaceHistory.erase(std::remove_if(workspaceHistory.begin(), workspaceHistory.end(),
        [&](const std::wstring& p) { return _wcsicmp(p.c_str(), full.c_str()) == 0; }),
        workspaceHistory.end());
    workspaceHistory.insert(workspaceHistory.begin(), full);
    while (workspaceHistory.size() > MaxWorkspaceHistory) workspaceHistory.pop_back();
}

void Settings::ClearWorkspaces() {
    workspaceHistory.clear();
    Save();
}
