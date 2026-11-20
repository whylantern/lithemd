#include "path_util.h"
#include "common.h"
#include <shlobj.h>
#include <objbase.h>
#include <algorithm>
#include <cwctype>

namespace {

std::wstring Unquote(std::wstring s) {
    s = Trim(s);
    if (s.size() >= 2 &&
        ((s.front() == L'"' && s.back() == L'"') || (s.front() == L'\'' && s.back() == L'\'')))
        s = Trim(s.substr(1, s.size() - 2));
    return s;
}

std::wstring PercentDecode(const std::wstring& s) {
    std::string bytes;
    for (size_t i = 0; i < s.size(); i++) {
        wchar_t c = s[i];
        if (c == L'%' && i + 2 < s.size()) {
            auto hex = [](wchar_t x) -> int {
                if (x >= L'0' && x <= L'9') return x - L'0';
                if (x >= L'a' && x <= L'f') return x - L'a' + 10;
                if (x >= L'A' && x <= L'F') return x - L'A' + 10;
                return -1;
            };
            int a = hex(s[i + 1]), b = hex(s[i + 2]);
            if (a >= 0 && b >= 0) {
                bytes.push_back((char)((a << 4) | b));
                i += 2;
                continue;
            }
        }
        bytes += Utf8FromWide(std::wstring(1, c));
    }
    return WideFromUtf8(bytes);
}

bool IsAbsolute(const std::wstring& s) {
    if (s.size() >= 2 && s[1] == L':') return true;
    if (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\') return true;
    if (!s.empty() && (s[0] == L'\\' || s[0] == L'/')) return true;
    return false;
}

} // namespace

std::wstring ResolveUserPath(const std::wstring& input, const std::wstring& baseDir) {
    std::wstring s = Unquote(input);
    if (s.empty()) return L"";
    if (StartsWithI(s, L"file://")) {
        s = s.substr(7);
        while (!s.empty() && (s.front() == L'/' || s.front() == L'\\')) s.erase(s.begin());
        s = PercentDecode(s);
        // file:///C:/x becomes C:/x; file://server/share keeps the UNC form.
        if (s.size() >= 2 && s[1] == L':') return AbsolutePath(s);
        return AbsolutePath(L"\\\\" + s);
    }
    if (s == L"~" || StartsWithI(s, L"~/") || StartsWithI(s, L"~\\")) {
        wchar_t* home = nullptr;
        std::wstring rest = s.size() > 1 ? s.substr(2) : L"";
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &home)) && home) {
            std::wstring result = PathJoin(home, rest);
            CoTaskMemFree(home);
            return AbsolutePath(result);
        }
    }
    if (s.find(L'%') != std::wstring::npos) {
        DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
        if (n > 1) {
            std::wstring expanded(n, L'\0');
            if (ExpandEnvironmentStringsW(s.c_str(), &expanded[0], n) > 0) {
                expanded.resize(n - 1);
                s = expanded;
            }
        }
    }
    if (!IsAbsolute(s) && !baseDir.empty()) s = PathJoin(baseDir, s);
    s = AbsolutePath(s);
    // Remove a trailing separator except for drive roots and UNC roots.
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) {
        if (s.size() == 3 && s[1] == L':') break;
        s.pop_back();
    }
    return s;
}

bool IsArchivePath(const std::wstring& path) {
    std::wstring ext = PathExt(path);
    return ext == L".zip" || ext == L".cbz";
}

bool IsUrlPath(const std::wstring& path) {
    return StartsWithI(path, L"http://") || StartsWithI(path, L"https://") ||
           StartsWithI(path, L"mailto:");
}
