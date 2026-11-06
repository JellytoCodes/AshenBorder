#pragma once

#include "Types/Math.h"

#include <string>
#include <vector>

namespace Engine
{
    enum class UiTextAlignment
    {
        Left,
        Center,
        Right
    };

    // Top-left origin. Positions/sizes are viewport fractions; text height is
    // a font line height relative to viewport height. Colors are straight-alpha values in [0, 1].
    struct UiRect
    {
        Vector2 position = {0.0f, 0.0f};
        Vector2 size = {0.0f, 0.0f};
        Vector4 color = {1.0f, 1.0f, 1.0f, 1.0f};
    };

    struct UiText
    {
        Vector2 position = {0.0f, 0.0f};
        float height = 0.025f;
        Vector4 color = {1.0f, 1.0f, 1.0f, 1.0f};
        // Strict UTF-8. The Windows renderer supports BMP glyphs and LF line
        // breaks; unsupported/missing glyphs fail rather than becoming '?'.
        std::string text;
        // position.x is the alignment anchor. A positive maxWidth fits the
        // complete line uniformly inside that width; zero keeps the original size.
        UiTextAlignment alignment = UiTextAlignment::Left;
        float maxWidth = 0.0f;
    };

    // Solid triangles are drawn after rectangles and before text in the same UI batch.
    struct UiTriangle
    {
        Vector2 a = {0.0f, 0.0f};
        Vector2 b = {0.0f, 0.0f};
        Vector2 c = {0.0f, 0.0f};
        Vector4 color = {1.0f, 1.0f, 1.0f, 1.0f};
    };

    struct UiFrame
    {
        std::vector<UiRect> rects;
        std::vector<UiText> texts;
        std::vector<UiTriangle> triangles;
    };
}
