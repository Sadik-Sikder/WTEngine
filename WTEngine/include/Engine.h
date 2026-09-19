// Engine.h
#pragma once
#include <string>
#include <memory>
#include <vector>
#include "DOM.h"
#include "Layout.h"

class Renderer;
class Engine {
public:
    Engine(int w, int h);
    ~Engine();

    void loadHTML(const std::wstring& html);
    void onResize(int width, int height);
    void setRenderer(Renderer* r); // used to measure text for wrapping
    void scroll(int delta);
    void render(Renderer& renderer);
    int getDocumentHeight() const;

    // Returns the href of the link under window-space point (x, y), or empty.
    std::wstring linkAt(int x, int y, Renderer& renderer) const;

private:
    int width, height, documentHeight = 0;
    int scrollY = 0;

    std::shared_ptr<Document> document;
    LayoutRoot layoutRoot;
    Renderer* measurer = nullptr;

    void parseAndBuild(const std::wstring& html);
    void doLayout();
};
