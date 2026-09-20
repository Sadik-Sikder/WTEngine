#pragma once

#include <string>

struct Color {
    float r, g, b, a;
};

// Parses a CSS-style "#rrggbb" color; anything else (empty, malformed, a
// named color) falls back to light gray. Defined in Engine.cpp.
Color parseColor(const std::wstring& str);

class Renderer {
public:
    virtual ~Renderer() = default;

    virtual void beginFrame(int width, int height, float scrollY) = 0;
    virtual void endFrame() = 0;

    virtual void drawRect(float x, float y, float w, float h, Color color) = 0;
    virtual void drawText(float x, float y, const std::wstring& text,
        float fontSize, Color color) = 0;
    virtual float measureText(const std::wstring& text, float fontSize) = 0;

    // Restrict drawing to a rectangle (window coordinates, y down) until clearClip().
    virtual void setClip(float x, float y, float w, float h) = 0;
    virtual void clearClip() = 0;
};
