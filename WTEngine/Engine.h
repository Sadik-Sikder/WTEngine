// Engine.h
#pragma once
#include <string>
#include <memory>
#include <vector>
#include <windows.h>
#include "DOM.h"
#include "Layout.h"

class Engine {
public:
    Engine(HWND hwnd);
    ~Engine();

    void loadHTML(const std::wstring& html);
    void onResize(int width, int height);
    void scroll(int delta);
    void render(HDC hdc);
    int getDocumentHeight() const;

private:
    HWND hwnd;
    int width, height, documentHeight = 0;
    int scrollY = 0;

    std::shared_ptr<Document> document;
    LayoutRoot layoutRoot;

    void parseAndBuild(const std::wstring& html);
    void doLayout();
};
