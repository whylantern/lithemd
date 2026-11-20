#pragma once
#include "view.h"
#include <cmath>

constexpr UINT WM_INK_COMMIT = WM_APP + 10;
constexpr UINT WM_INK_CANCEL = WM_APP + 11;

enum : int {
    INK_PEN = 1,
    INK_ERASER,
    INK_LINE,
    INK_ARROW,
    INK_RECT,
    INK_ELLIPSE,
    INK_TEXT,
    INK_MORE,
    INK_WIDTH_DOWN,
    INK_WIDTH_UP,
    INK_UNDO,
    INK_CLEAR,
    INK_DONE,
    INK_SELECT = 60,
    INK_HIGHLIGHT,
    INK_COPY,
    INK_PASTE,
    INK_GROUP,
    INK_UNGROUP,
    INK_DELETE,
    INK_SAVE,
    INK_ROTATE,
    INK_PALETTE = 90,
    INK_COLOR = 100
};

enum : int { INK_SHAPE_LINE = 0, INK_SHAPE_ARROW, INK_SHAPE_RECT, INK_SHAPE_ELLIPSE };
enum : int { INK_TOOL_PEN = 0, INK_TOOL_ERASER, INK_TOOL_LINE, INK_TOOL_ARROW,
             INK_TOOL_RECT, INK_TOOL_ELLIPSE, INK_TOOL_TEXT, INK_TOOL_SELECT,
             INK_TOOL_HIGHLIGHT };

inline float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline COLORREF ToColorRef(const Color& c) {
    auto ch = [](float v) { return (BYTE)std::lround(Clampf(v, 0.0f, 1.0f) * 255.0f); };
    return RGB(ch(c.r), ch(c.g), ch(c.b));
}

inline const std::vector<Color>& InkColors() {
    static const std::vector<Color> colors = {
        HexColor(L"#E5484D"), HexColor(L"#F5A524"), HexColor(L"#30A46C"),
        HexColor(L"#3E63DD"), HexColor(L"#1F2328"), HexColor(L"#FFFFFF")
    };
    return colors;
}
