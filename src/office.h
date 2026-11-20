// Readers for Word / Excel documents. OOXML formats are parsed with a ZIP+XML
// reader; legacy .doc/.xls use the OLE compound-file and binary record formats.
#pragma once
#include "docmodel.h"
#include <vector>

bool BuildDocx(Document& doc, const std::vector<uint8_t>& bytes, std::wstring* error);
bool BuildXlsx(Document& doc, const std::vector<uint8_t>& bytes, std::wstring* error);
bool BuildLegacyDoc(Document& doc, const std::vector<uint8_t>& bytes, std::wstring* error);
bool BuildLegacyXls(Document& doc, const std::vector<uint8_t>& bytes, std::wstring* error);
bool BuildCsv(Document& doc, const std::wstring& text, std::wstring* error);
