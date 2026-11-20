// User-entered path normalisation shared by the path bar, workspace and CLI.
#pragma once
#include <string>

// Accepts quoted paths, file:// URLs, %ENV% variables, ~ and relative paths.
// Relative paths are resolved against baseDir (usually the active document).
std::wstring ResolveUserPath(const std::wstring& input, const std::wstring& baseDir);

bool IsArchivePath(const std::wstring& path);
bool IsUrlPath(const std::wstring& path);
