#include "app.h"
#include "common.h"
#include <objbase.h>
#include <shellapi.h>
#include <string>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    EnableDarkModeForApp();
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::vector<std::wstring> files;
    bool openSettings = false;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; i++) {
            std::wstring a = argv[i] ? argv[i] : L"";
            while (!a.empty() && (a.front() == L'"' || a.front() == L'\'')) a.erase(a.begin());
            while (!a.empty() && (a.back() == L'"' || a.back() == L'\'')) a.pop_back();
            if (a.empty()) continue;
            if (_wcsicmp(a.c_str(), L"--settings") == 0) openSettings = true;
            else files.push_back(a);
        }
        LocalFree(argv);
    }

    int result = RunApp(inst, std::move(files), openSettings);

    if (SUCCEEDED(co)) CoUninitialize();
    return result;
}
