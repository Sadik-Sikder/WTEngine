// Layout.h
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "DOM.h"

// Simple layout box result
struct LayoutBox {
    // Interactive form controls get a box of their own; the Engine draws and
    // handles them using `el` (their DOM element).
    enum Control { NoControl, TextField, Button, Checkbox };

    int x, y, width, height;
    std::wstring background; // e.g. "#rrggbb"
    std::wstring text; // text inside (for leaf text-only boxes); a Button's label
    std::wstring href; // link target if this text is inside an <a href>
    int fontSize = 14;

    Control control = NoControl;
    Element* el = nullptr;   // the control's element (NoControl: unused)
    Element* form = nullptr; // the enclosing <form>, if any
};

struct LayoutRoot {
    int viewportWidth = 800;
    Document* doc = nullptr;
    Node* rootNode = nullptr;
    std::vector<LayoutBox> boxes;
    std::wstring currentHref; // href of the enclosing <a>, set during layout
    Element* currentForm = nullptr; // the enclosing <form>, set during layout

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
    void layoutControl(Element* el, int x, int& y, int containingWidth);
};
