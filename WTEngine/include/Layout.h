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

    // A border, drawn as a hollow frame (see Engine::render) so an
    // unset `background` still shows whatever's behind the box through
    // the middle - matching a real CSS border, which never fills its box.
    int borderWidth = 0;
    std::wstring borderColor;

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
    // Same unit handling as resolveFontSize (px, or a percentage), but for
    // width/height/border/margin/padding, where a percentage resolves
    // against the containing block's width, not the font size - the same
    // base CSS itself uses for percentage margin/padding on every side,
    // including top/bottom (a well-known CSS quirk, not a bug here).
    // em/rem aren't supported for these properties (unlike font-size,
    // there's no single obviously-right base to multiply), and fall back
    // to `def` like any other unrecognized unit.
    int resolveLength(const std::wstring& s, int base, int def);
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
    void collectInline(Element* el, int inheritedFontSize, int containingWidth, std::vector<InlineItem>& out);
    void layoutInlineRun(const std::vector<InlineItem>& items, int x, int& y, int containingWidth);

    // The subset of an element's cascaded style that layout cares about -
    // computed once per element and shared by both plain elements and form
    // controls, so both respond to the same CSS/inline styling.
    enum class Display { Block, Inline, None };
    // Only meaningful with an explicit `width` set; ContentBox (the CSS
    // default) means `width` names the content box, so padding/border add
    // to it - BorderBox means `width` already includes them. See
    // layoutElement's box-model math for how each is turned into an
    // outer/content width.
    enum class BoxSizing { ContentBox, BorderBox };
    struct ComputedStyle {
        std::wstring background;
        int marginTop = 6, marginBottom = 6, marginLeft = 0, marginRight = 0;
        int paddingTop = 6, paddingRight = 6, paddingBottom = 6, paddingLeft = 6;
        int width = -1; // -1 = auto: fill the container, same as if unset (today's only behavior)
        int borderWidth = 0;
        std::wstring borderColor;
        BoxSizing boxSizing = BoxSizing::ContentBox;
        int fontSize = 14;
        Display display = Display::Block;
    };
    ComputedStyle computeStyle(Element* e, int inheritedFontSize, int containingWidth);
    // Splits a shorthand value like "4px 8px" on whitespace and expands it
    // to all four sides per CSS's 1/2/3/4-value shorthand rule (used for
    // both `margin` and `padding`). Missing tokens, or a value that fails
    // to parse, fall back to `def`.
    void parseBoxShorthand(const std::wstring& v, int containingWidth, int def,
                            int& top, int& right, int& bottom, int& left);
    void layoutControl(Element* el, int x, int& y, int containingWidth, const ComputedStyle& style);
    void layoutImage(Element* el, int x, int& y, int containingWidth, const ComputedStyle& style);

    // Ancestors of the element layoutElement is currently iterating the
    // children of (root first); used to match descendant selectors ("a b").
    std::vector<Element*> ancestorStack;
};
