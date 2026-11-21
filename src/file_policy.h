// File-type policy for formats LitheMD must never read or decode.
#pragma once
#include <string>

// True for executable/system binary formats that are not documents.
bool IsBlockedBinaryFile(const std::wstring& path);
