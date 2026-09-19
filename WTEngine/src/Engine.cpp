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
    layoutRoot.measureText = [this](const std::wstring& text, int fontSize) {
        return measurer ? measurer->measureText(text, (float)fontSize)
                        : text.size() * fontSize * 0.55f;
    };
}

Engine::~Engine() {}

void Engine::setRenderer(Renderer* r) {
    measurer = r;
    doLayout(); // re-wrap with real text metrics
}

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
    if (w == width && h == height) return; // nothing to re-wrap
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
    int maxScroll = documentHeight - viewHeight();
    if (maxScroll < 0) maxScroll = 0;

    if (scrollY > maxScroll) scrollY = maxScroll;
    if (scrollY < 0) scrollY = 0;
}

int Engine::getDocumentHeight() const {
    return documentHeight;
}

void Engine::scroll(int delta) {
    scrollY += delta;

    int maxScroll = documentHeight - viewHeight();
    if (maxScroll < 0) maxScroll = 0;

    scrollY = std::max(0, std::min(scrollY, maxScroll));
}

void Engine::render(Renderer& renderer) {
    // Renderer should clear framebuffer first:
    // glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    for (const auto& b : layoutRoot.boxes) {

        int screenY = b.y - scrollY + topInset;

        // Cull boxes outside viewport
        if (screenY + b.height < topInset || screenY > height)
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
                b.href.empty() ? Color{ 0, 0, 0, 1 } : Color{ 0.0f, 0.2f, 0.8f, 1.0f } // links are blue
            );
        }
    }
}

std::wstring Engine::linkAt(int x, int y, Renderer& renderer) const {
    if (y < topInset) return L"";
    int docY = y - topInset + scrollY;

    // Later boxes paint on top, so check them first.
    for (auto it = layoutRoot.boxes.rbegin(); it != layoutRoot.boxes.rend(); ++it) {
        const auto& b = *it;
        if (b.href.empty() || b.text.empty()) continue;

        // Text is drawn at (x + 4, y + 4) and is only as wide as its glyphs,
        // so hit-test that area rather than the whole row.
        float fontSize = b.fontSize > 0 ? b.fontSize : 14;
        int textW = static_cast<int>(renderer.measureText(b.text, fontSize));
        int left = b.x + 4, top = b.y + 4;
        if (x >= left && x < left + textW && docY >= top && docY < top + b.height)
            return b.href;
    }
    return L"";
}

void Engine::setTopInset(int px) {
    topInset = px;
    doLayout(); // re-clamp scroll for the new viewport height
}
