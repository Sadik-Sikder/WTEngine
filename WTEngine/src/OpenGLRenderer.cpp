#include "OpenGLRenderer.h"
#include <vector>
#include <algorithm>

OpenGLRenderer::~OpenGLRenderer() {
    for (auto& [size, font] : measureFonts) DeleteObject(font);
    if (measureDC) DeleteDC(measureDC);
    for (auto& [key, tex] : textCache) {
        glDeleteTextures(1, &tex.id);
    }
}

// Rasterizes `text` into an off-screen GDI bitmap (white text on a black
// background), then converts that into an RGBA texture where each pixel's
// brightness becomes its alpha. Drawing that texture with glColor4f(color)
// then tints the (already anti-aliased) glyph shapes to any color we want,
// without baking a color into the cached texture itself.
const TextTexture& OpenGLRenderer::getOrCreateTextTexture(const std::wstring& text, float fontSize) {
    std::wstring key = text + L"@" + std::to_wstring((int)fontSize);
    auto it = textCache.find(key);
    if (it != textCache.end()) return it->second;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    HFONT font = CreateFontW(
        -(int)fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT oldFont = (HFONT)SelectObject(memDC, font);

    SIZE sz{ 1, 1 };
    GetTextExtentPoint32W(memDC, text.c_str(), (int)text.size(), &sz);
    int texW = std::max(1, (int)sz.cx);
    int texH = std::max(1, (int)sz.cy);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = texW;
    bmi.bmiHeader.biHeight = -texH; // negative = top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, bitmap);

    RECT rc{ 0, 0, texW, texH };
    FillRect(memDC, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(255, 255, 255));
    TextOutW(memDC, 0, 0, text.c_str(), (int)text.size());
    GdiFlush();

    // bits is BGRX; since the glyphs were drawn white-on-black, R==G==B==
    // coverage already, so brightness doubles as the alpha we need.
    auto* src = static_cast<unsigned char*>(bits);
    std::vector<unsigned char> rgba(static_cast<size_t>(texW) * texH * 4);
    for (int i = 0; i < texW * texH; i++) {
        unsigned char coverage = src[i * 4 + 2]; // B (== G == R)
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = coverage;
    }

    SelectObject(memDC, oldBitmap);
    SelectObject(memDC, oldFont);
    DeleteObject(bitmap);
    DeleteObject(font);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    TextTexture tex;
    tex.width = texW;
    tex.height = texH;
    glGenTextures(1, &tex.id);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texW, texH, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    auto result = textCache.emplace(key, tex);
    return result.first->second;
}

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
    if (text.empty()) return;

    const TextTexture& tex = getOrCreateTextTexture(text, fontSize);

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glColor4f(color.r, color.g, color.b, color.a);

    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(x, y);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(x + tex.width, y);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(x + tex.width, y + tex.height);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(x, y + tex.height);
    glEnd();

    glDisable(GL_TEXTURE_2D);
}

// Measures with the same font the textures are rasterized with, but without
// creating a texture (layout measures many strings that are never drawn).
float OpenGLRenderer::measureText(const std::wstring& text, float fontSize) {
    if (text.empty()) return 0;

    if (!measureDC) measureDC = CreateCompatibleDC(nullptr);

    int size = (int)fontSize;
    auto it = measureFonts.find(size);
    if (it == measureFonts.end()) {
        HFONT font = CreateFontW(
            -size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        it = measureFonts.emplace(size, font).first;
    }

    HFONT old = (HFONT)SelectObject(measureDC, it->second);
    SIZE sz{ 0, 0 };
    GetTextExtentPoint32W(measureDC, text.c_str(), (int)text.size(), &sz);
    SelectObject(measureDC, old);
    return static_cast<float>(sz.cx);
}
