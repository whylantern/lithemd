#pragma once
#include <string>
#include <vector>

struct UiLanguage {
    std::wstring code;
    std::wstring name;
};

// Built-in Chinese/English plus external translations discovered in
// <config dir>/translations/*.json and <exe dir>/translations/*.json.
const std::vector<UiLanguage>& UiLanguages();
void ReloadUiLanguages();
void SetUiLanguage(const std::wstring& code);
const std::wstring& CurrentUiLanguage();
std::wstring TranslateUi(const std::wstring& text);
