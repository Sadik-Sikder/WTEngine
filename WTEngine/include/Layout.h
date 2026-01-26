// Layout.h
#pragma once
#include <string>
#include <vector>
#include <memory>
#include "DOM.h"

// Simple layout box result
struct LayoutBox {
    int x, y, width, height;
    std::wstring background; // e.g. "#rrggbb"
    std::wstring text; // text inside (for leaf text-only boxes)
    int fontSize = 14;
};

struct LayoutRoot {
    int viewportWidth = 800;
    Document* doc = nullptr;
    Node* rootNode = nullptr;
    std::vector<LayoutBox> boxes;

    void layout(); // compute boxes from rootNode
private:
    void layoutElement(Element* el, int x, int& y, int containingWidth);
    std::wstring getAttr(Element* el, const std::wstring& key, const std::wstring& def = L"");
    int parseFontSize(const std::wstring& s, int def = 14);
};
