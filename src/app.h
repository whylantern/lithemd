#pragma once
#include <windows.h>
#include <string>
#include <vector>

int RunApp(HINSTANCE inst, std::vector<std::wstring> initialFiles, bool openSettings);
