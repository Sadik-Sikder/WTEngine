#pragma once

#include <string>

struct Color {
    float r, g, b, a;
};

class Renderer {
public:
    virtual ~Renderer() = default;

    virtual void beginFrame(int width, int height, float scrollY) = 0;
    virtual void endFrame() = 0;

    virtual void drawRect(float x, float y, float w, float h, Color color) = 0;
    virtual void drawText(float x, float y, const std::wstring& text,
        float fontSize, Color color) = 0;
    virtual float measureText(const std::wstring& text, float fontSize) = 0;
};
