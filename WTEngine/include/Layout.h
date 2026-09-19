// Layout.h
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "DOM.h"

// Simple layout box result
struct LayoutBox {
    int x, y, width, height;
    std::wstring background; // e.g. "#rrggbb"
    std::wstring text; // text inside (for leaf text-only boxes)
    std::wstring href; // link target if this text is inside an <a href>
    int fontSize = 14;
};

struct LayoutRoot {
    int viewportWidth = 800;
    Document* doc = nullptr;
    Node* rootNode = nullptr;
    std::vector<LayoutBox> boxes;
    std::wstring currentHref; // href of the enclosing <a>, set during layout

    // Returns the pixel width of `text` at `fontSize`. Used to wrap text; a
    // rough per-character estimate is used when unset.
    std::function<float(const std::wstring&, int)> measureText;

    void layout(); // compute boxes from rootNode
private:
    void layoutElement(Element* el, int x, int& y, int containingWidth);
    std::wstring getAttr(Element* el, const std::wstring& key, const std::wstring& def = L"");
    int parseFontSize(const std::wstring& s, int def = 14);
    float textWidth(const std::wstring& text, int fontSize);
    void layoutText(const std::wstring& text, int x, int& y, int containingWidth);
};
