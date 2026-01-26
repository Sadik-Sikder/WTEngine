#pragma once

#include "Renderer.h"
#include <windows.h>  
#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include <string>

class OpenGLRenderer : public Renderer {
public:
    OpenGLRenderer() = default;
    ~OpenGLRenderer() override = default;

    void beginFrame(int width, int height, float scrollY) override;
    void endFrame() override;

    void drawRect(float x, float y, float w, float h, Color color) override;
    void drawText(float x, float y, const std::wstring& text,
        float fontSize, Color color) override;
};
