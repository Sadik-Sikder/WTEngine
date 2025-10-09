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
    void render(HDC hdc);

private:
    HWND hwnd;
    int width, height;

    std::shared_ptr<Document> document;
    LayoutRoot layoutRoot;

    void parseAndBuild(const std::wstring& html);
    void doLayout();
};
