#pragma once

#define NOMINMAX
#include "Renderer.h"
#include <windows.h>
#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include <string>
#include <map>

// A text string rasterized once (via GDI) and uploaded as an alpha-only
// OpenGL texture, cached so repeated draws of the same string/size don't
// re-rasterize every frame.
struct TextTexture {
    GLuint id = 0;
    int width = 0;
    int height = 0;
};

class OpenGLRenderer : public Renderer {
public:
    OpenGLRenderer() = default;
    ~OpenGLRenderer() override;

    void beginFrame(int width, int height, float scrollY) override;
    void endFrame() override;

    void drawRect(float x, float y, float w, float h, Color color) override;
    void drawText(float x, float y, const std::wstring& text,
        float fontSize, Color color) override;
    float measureText(const std::wstring& text, float fontSize) override;

private:
    std::map<std::wstring, TextTexture> textCache;
    HDC measureDC = nullptr;
    std::map<int, HFONT> measureFonts; // font size -> font, for measureText
    const TextTexture& getOrCreateTextTexture(const std::wstring& text, float fontSize);
};
