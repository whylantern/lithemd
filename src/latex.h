// Small dependency-free LaTeX math parser and DWrite/D2D layout engine.
#pragma once
#include "common.h"
#include <memory>
#include <string>
#include <vector>

struct IDWriteFactory;
struct ID2D1RenderTarget;

enum class MathKind {
    Row, Text, Fraction, Sqrt, Script, Matrix, Delim, Accent, Space, Line
};

enum class MathAccent { None, Bar, Hat, Tilde, Dot, Ddot, Vec, Overline, Underline };

struct MathNode {
    MathKind kind = MathKind::Row;
    std::wstring text;
    bool bold = false;
    bool italic = false;
    bool roman = false;
    bool displayLimits = false;
    float em = 0.0f;                // spacing width in em
    MathAccent accent = MathAccent::None;
    std::shared_ptr<MathNode> a, b, c;      // fraction: a/b; script: a=base, b=sup, c=sub
    std::vector<std::shared_ptr<MathNode>> items; // row/delim
    std::vector<std::vector<std::shared_ptr<MathNode>>> cells; // matrix
    wchar_t openDelim = 0;
    wchar_t closeDelim = 0;
};

std::shared_ptr<MathNode> ParseMath(const std::wstring& latex, bool display);

struct MathBox {
    float width = 0;
    float ascent = 0;
    float descent = 0;
    float height() const { return ascent + descent; }
};

MathBox MeasureMath(const MathNode& node, IDWriteFactory* dwrite, float fontSize);
void DrawMath(const MathNode& node, ID2D1RenderTarget* target, IDWriteFactory* dwrite,
              float x, float baseline, float fontSize, const Color& color);

// Convenience for plain-text contexts.
std::wstring MathPlainText(const MathNode& node);
