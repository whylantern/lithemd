#include "file_policy.h"
#include "common.h"

bool IsBlockedBinaryFile(const std::wstring& path) {
    static const wchar_t* blocked[] = {
        L".exe", L".dll", L".sys", L".drv", L".scr", L".com",
        L".msi", L".msp", L".cpl", L".ocx", L".efi",
        L".bin", L".lnk", L".class", L".pyc", L".o", L".obj", L".lib"
    };
    std::wstring ext = PathExt(path);
    if (ext.empty()) return false;
    for (auto* item : blocked)
        if (ext == item) return true;
    return false;
}
