// Layout.h
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "DOM.h"
#include "CSS.h"

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
    const std::vector<CSS::Rule>* rules = nullptr; // from <style> blocks on the page

    // Returns the pixel width of `text` at `fontSize`. Used to wrap text; a
    // rough per-character estimate is used when unset.
    std::function<float(const std::wstring&, int)> measureText;

    void layout(); // compute boxes from rootNode
private:
    // `inheritedFontSize` is the size to use for `el`'s own direct text, and
    // the default for its children's font-size unless they set their own -
    // i.e. plain CSS inheritance, not reset to a hardcoded size each level.
    void layoutElement(Element* el, int x, int& y, int containingWidth, int inheritedFontSize);
    std::wstring getAttr(Element* el, const std::wstring& key, const std::wstring& def = L"");
    int parseFontSize(const std::wstring& s, int def = 14);
    int resolveFontSize(const std::wstring& s, int baseFontSize, int def);
    float textWidth(const std::wstring& text, int fontSize);
    void layoutText(const std::wstring& text, int x, int& y, int containingWidth, int fontSize);

    // The subset of an element's cascaded style that layout cares about -
    // computed once per element and shared by both plain elements and form
    // controls, so both respond to the same CSS/inline styling.
    struct ComputedStyle {
        std::wstring background;
        int marginTop = 6, marginBottom = 6, padding = 6, fontSize = 14;
        bool displayNone = false;
    };
    ComputedStyle computeStyle(Element* e, int inheritedFontSize);
    void layoutControl(Element* el, int x, int& y, int containingWidth, const ComputedStyle& style);

    // Ancestors of the element layoutElement is currently iterating the
    // children of (root first); used to match descendant selectors ("a b").
    std::vector<Element*> ancestorStack;
};
