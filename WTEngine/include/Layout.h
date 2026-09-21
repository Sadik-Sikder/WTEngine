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
    // Interactive form controls get a box of their own; the Engine draws
    // and handles them using `el`.
    enum Control { NoControl, TextField, Button, Checkbox, Select };

    int x, y, width, height;
    std::wstring background; // e.g. "#rrggbb"
    std::wstring text; // text inside (for leaf text-only boxes); a Button's label
    std::wstring href; // link target if this text is inside an <a href>
    std::wstring imageSrc; // <img>'s raw (unresolved) src attribute; empty for non-image boxes
    int fontSize = 14;

    Control control = NoControl;
    // The element this box was generated from: a control's own element,
    // the element a background/image box belongs to, or (for a text box)
    // the direct parent element of that text. Used for click hit-testing
    // (Engine::dispatchClick) so addEventListener('click', ...) can find
    // which element - and its ancestors, for bubbling - a click landed on.
    Element* el = nullptr;
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

    // Resolves an <img>'s raw `src`, fetching/decoding (and caching) it if
    // not already cached, and returns its natural pixel size via (outW,
    // outH). Used to size a box that doesn't give explicit width/height.
    // Returns false if unset or the fetch/decode fails.
    std::function<bool(const std::wstring& src, int& outW, int& outH)> loadImage;

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

    // One word of flowing inline content (from a text node, or from an
    // inline-level element like <a>/<b> flattened into its container's
    // run - see collectInline), tagged with the style it should render
    // with. isBreak marks a <br>: a forced line break with no text of its
    // own.
    struct InlineItem {
        std::wstring word;
        int fontSize = 14;
        std::wstring href;
        Element* owner = nullptr;
        bool isBreak = false;
    };
    // Splits `text` on whitespace, appending one InlineItem per word.
    void appendWords(const std::wstring& text, int fontSize, const std::wstring& href,
                      Element* owner, std::vector<InlineItem>& out);
    void collectInline(Element* el, int inheritedFontSize, std::vector<InlineItem>& out);
    void layoutInlineRun(const std::vector<InlineItem>& items, int x, int& y, int containingWidth);

    // The subset of an element's cascaded style that layout cares about -
    // computed once per element and shared by both plain elements and form
    // controls, so both respond to the same CSS/inline styling.
    enum class Display { Block, Inline, None };
    struct ComputedStyle {
        std::wstring background;
        int marginTop = 6, marginBottom = 6, padding = 6, fontSize = 14;
        Display display = Display::Block;
    };
    ComputedStyle computeStyle(Element* e, int inheritedFontSize);
    void layoutControl(Element* el, int x, int& y, int containingWidth, const ComputedStyle& style);
    void layoutImage(Element* el, int x, int& y, int containingWidth, const ComputedStyle& style);

    // Ancestors of the element layoutElement is currently iterating the
    // children of (root first); used to match descendant selectors ("a b").
    std::vector<Element*> ancestorStack;
};
