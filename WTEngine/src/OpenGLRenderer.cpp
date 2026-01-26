#include "OpenGLRenderer.h"


void OpenGLRenderer::beginFrame(int width, int height, float scrollY) {
    glViewport(0, 0, width, height);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width, height, 0, -1, 1); 
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

void OpenGLRenderer::endFrame() {
    // Nothing needed here for now
}

void OpenGLRenderer::drawRect(float x, float y, float w, float h, Color color) {
    glColor4f(color.r, color.g, color.b, color.a);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

void OpenGLRenderer::drawText(float x, float y, const std::wstring& text,
    float fontSize, Color color) {
    // Placeholder: OpenGL text rendering is complex (needs FreeType)
}
