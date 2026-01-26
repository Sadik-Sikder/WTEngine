// Engine.cpp
#include "Engine.h"
#include "HTMLParser.h"
#include "Renderer.h"
#include <algorithm>
#include <string>
#include <sstream> 

// Minimal color struct for OpenGL

Color parseColor(const std::wstring& str) {
    if (str.empty() || str[0] != L'#' || str.size() != 7) {
        // default light gray
        return { 0.94f, 0.94f, 0.94f, 1.0f };
    }

    int r = std::stoi(std::string(str.begin() + 1, str.begin() + 3), nullptr, 16);
    int g = std::stoi(std::string(str.begin() + 3, str.begin() + 5), nullptr, 16);
    int b = std::stoi(std::string(str.begin() + 5, str.begin() + 7), nullptr, 16);

    return { r / 255.0f, g / 255.0f, b / 255.0f, 1.0f };
}

// ------------------ Engine Implementation ------------------

Engine::Engine(int w, int h)
    : width(w), height(h) {
    layoutRoot.viewportWidth = width;
}

Engine::~Engine() {}

void Engine::loadHTML(const std::wstring& html) {
    parseAndBuild(html);
    doLayout();
}

void Engine::parseAndBuild(const std::wstring& html) {
    HTMLParser parser;
    document = parser.parse(html);

    if (document && document->body) {
        layoutRoot.rootNode = document->body.get();
    }
}

void Engine::onResize(int w, int h) {
    width = w;
    height = h;

    layoutRoot.viewportWidth = width;
    doLayout();
}

void Engine::doLayout() {
    if (!document || !document->body)
        return;

    layoutRoot.boxes.clear();
    layoutRoot.layout();

    // Calculate document height
    documentHeight = 0;
    for (const auto& b : layoutRoot.boxes) {
        int bottom = b.y + b.height;
        if (bottom > documentHeight)
            documentHeight = bottom;
    }

    // Clamp scroll
    int maxScroll = documentHeight - height;
    if (maxScroll < 0) maxScroll = 0;

    if (scrollY > maxScroll) scrollY = maxScroll;
    if (scrollY < 0) scrollY = 0;
}

int Engine::getDocumentHeight() const {
    return documentHeight;
}

void Engine::scroll(int delta) {
    scrollY += delta;

    int maxScroll = documentHeight - height;
    if (maxScroll < 0) maxScroll = 0;

    scrollY = std::max(0, std::min(scrollY, maxScroll));
}

void Engine::render(Renderer& renderer) {
    // Renderer should clear framebuffer first:
    // glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    for (const auto& b : layoutRoot.boxes) {

        int screenY = b.y - scrollY;

        // Cull boxes outside viewport
        if (screenY + b.height < 0 || screenY > height)
            continue;

        // Draw background
        if (!b.background.empty()) {
            renderer.drawRect(
                b.x,
                screenY,
                b.width,
                b.height,
                parseColor(b.background)
            );
        }

        // Draw text
        if (!b.text.empty()) {
            renderer.drawText(
                b.x + 4,
                screenY + 4,
                b.text,
                b.fontSize > 0 ? b.fontSize : 14,
                { 0, 0, 0, 1 } // black text
            );
        }
    }
}
