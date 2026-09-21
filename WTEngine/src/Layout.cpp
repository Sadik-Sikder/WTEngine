#define NOMINMAX
#include "Layout.h"
#include <windows.h>
#include <string>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cwctype>

std::wstring LayoutRoot::getAttr(Element* el, const std::wstring& key, const std::wstring& def) {
    if (!el) return def;
    auto it = el->attrs.find(key);
    if (it == el->attrs.end()) return def;
    return it->second;
}

// Parses a plain pixel length, e.g. margin/padding values or "6" / "20px".
// A unit this doesn't understand (a decimal, "1.5em", "50%", "2pt", ...)
// falls back to `def` rather than silently truncating to a wrong number -
// this used to read "1.5em" as the digits-only prefix "1" and return 1.
int LayoutRoot::parseFontSize(const std::wstring& s, int def) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    std::wstring v = s.substr(a, b - a);
    if (v.empty()) return def;

    size_t i = 0;
    while (i < v.size() && v[i] >= L'0' && v[i] <= L'9') i++;
    if (i == 0) return def; // doesn't start with a digit at all

    std::wstring unit = v.substr(i);
    if (!unit.empty() && unit != L"px") return def; // e.g. em/%/pt - not a plain pixel count

    try { return std::stoi(v.substr(0, i)); }
    catch (...) { return def; }
}

// font-size specifically also supports units relative to `baseFontSize` (the
// inherited size): em/rem multiply it, % scales it - e.g. real pages commonly
// write `h1 { font-size: 1.5em }`. Anything else unsupported (pt, vw, ...)
// falls back to `def`.
int LayoutRoot::resolveFontSize(const std::wstring& s, int baseFontSize, int def) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    std::wstring v = s.substr(a, b - a);
    if (v.empty()) return def;

    size_t i = 0;
    bool sawDigit = false;
    while (i < v.size() && ((v[i] >= L'0' && v[i] <= L'9') || v[i] == L'.')) {
        if (v[i] != L'.') sawDigit = true;
        i++;
    }
    if (!sawDigit) return def;

    double value;
    try { value = std::stod(v.substr(0, i)); }
    catch (...) { return def; }

    std::wstring unit = v.substr(i);
    if (unit.empty() || unit == L"px") return (int)std::lround(value);
    if (unit == L"em" || unit == L"rem") return (int)std::lround(value * baseFontSize);
    if (unit == L"%") return (int)std::lround(value * baseFontSize / 100.0);
    return def;
}

float LayoutRoot::textWidth(const std::wstring& text, int fontSize) {
    if (measureText) return measureText(text, fontSize);
    return text.size() * fontSize * 0.55f; // rough fallback
}

// Tags treated as inline-level by default: their text/content flows onto
// the same line as their surrounding siblings instead of starting a block
// of its own. Everything else defaults to block, matching the engine's
// original (pre-inline) universal-block behavior.
static bool isInlineTag(const std::wstring& tag) {
    static const std::wstring kInline[] = {
        L"a", L"span", L"b", L"strong", L"i", L"em", L"u", L"small", L"code",
        L"sub", L"sup", L"mark", L"label", L"abbr", L"cite", L"q"
    };
    for (const auto& t : kInline) if (tag == t) return true;
    return false;
}

// Splits `text` on whitespace and appends one InlineItem per word, sharing
// `fontSize`/`href`/`owner` - the same tokenization layoutInlineRun's
// predecessor (layoutText) used to wrap a single string.
void LayoutRoot::appendWords(const std::wstring& text, int fontSize, const std::wstring& href,
                              Element* owner, std::vector<InlineItem>& out) {
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && iswspace(text[i])) i++;
        size_t start = i;
        while (i < text.size() && !iswspace(text[i])) i++;
        if (start == i) break;
        out.push_back({ text.substr(start, i - start), fontSize, href, owner, false });
    }
}

// Flattens `el`'s inline-level content (text nodes and nested inline
// elements like <a>/<b>) into a single flat item list, so a run of mixed
// text and inline markup - e.g. "Hello <a>link</a> world" - line-breaks as
// one paragraph instead of three separate blocks. A <br> becomes a forced
// break; a block-level element found here (invalid-ish nesting, e.g.
// <a><div>) is flattened into the run rather than specially promoted, since
// that combination is rare and not worth the extra complexity.
void LayoutRoot::collectInline(Element* el, int inheritedFontSize, std::vector<InlineItem>& out) {
    ancestorStack.push_back(el);

    for (auto& child : el->children) {
        if (child->type == Node::TEXT) {
            auto tnode = static_cast<TextNode*>(child.get());
            appendWords(tnode->text, inheritedFontSize, currentHref, el, out);
            continue;
        }

        auto e = static_cast<Element*>(child.get());
        if (e->tag == L"head" || e->tag == L"script" || e->tag == L"style" ||
            e->tag == L"title" || e->tag == L"meta" || e->tag == L"link" ||
            e->tag == L"base")
            continue;

        if (e->tag == L"br") {
            out.push_back({ L"", inheritedFontSize, L"", nullptr, true });
            continue;
        }

        ComputedStyle sv = computeStyle(e, inheritedFontSize);
        if (sv.display == Display::None) continue;

        std::wstring savedHref = currentHref;
        if (e->tag == L"a") {
            auto href = e->attrs.find(L"href");
            if (href != e->attrs.end()) currentHref = href->second;
        }
        collectInline(e, sv.fontSize, out);
        currentHref = savedHref;
    }

    ancestorStack.pop_back();
}

// Word-wraps a flattened inline run (see collectInline) to `containingWidth`,
// emitting one LayoutBox per item per line - not one box per line - since
// items on the same line can differ in font size, href, or owning element
// (e.g. a link in the middle of a sentence). Generalizes what layoutText
// used to do for a single homogeneously-styled string.
void LayoutRoot::layoutInlineRun(const std::vector<InlineItem>& items, int x, int& y, int containingWidth) {
    const int textInset = 4; // Engine::render draws text 4px inside its box
    const int paraGap = 6;
    const float maxWidth = (float)std::max(containingWidth - 2 * textInset, 40);

    struct Placed { InlineItem item; float offset; };
    std::vector<Placed> line;
    float lineWidth = 0;

    auto emitLine = [&]() {
        if (line.empty()) return;
        int lineHeight = 0;
        for (auto& p : line) lineHeight = std::max(lineHeight, p.item.fontSize);
        lineHeight += 8;
        for (auto& p : line) {
            LayoutBox box;
            box.x = x + (int)std::lround(p.offset);
            box.y = y;
            box.width = (int)std::lround(textWidth(p.item.word, p.item.fontSize));
            box.height = lineHeight;
            box.text = p.item.word;
            box.href = p.item.href;
            box.fontSize = p.item.fontSize;
            box.el = p.item.owner;
            boxes.push_back(box);
        }
        y += lineHeight;
        line.clear();
        lineWidth = 0;
    };

    for (const auto& raw : items) {
        if (raw.isBreak) { emitLine(); continue; }
        if (raw.word.empty()) continue;

        InlineItem item = raw;
        float wordWidth = textWidth(item.word, item.fontSize);

        // A word wider than the line on its own gets broken by characters,
        // one fragment per line, before whatever's left of it (now short
        // enough) falls through to the normal wrapping below.
        while (wordWidth > maxWidth && item.word.size() > 1) {
            if (!line.empty()) emitLine();
            size_t n = 1;
            while (n < item.word.size() && textWidth(item.word.substr(0, n + 1), item.fontSize) <= maxWidth) n++;
            InlineItem fragment = item;
            fragment.word = item.word.substr(0, n);
            line.push_back({ fragment, 0 });
            emitLine();
            item.word.erase(0, n);
            wordWidth = textWidth(item.word, item.fontSize);
        }
        if (item.word.empty()) continue;

        float spaceWidth = line.empty() ? 0 : textWidth(L" ", item.fontSize);
        if (!line.empty() && lineWidth + spaceWidth + wordWidth > maxWidth) emitLine();

        float offset = line.empty() ? 0 : lineWidth + spaceWidth;
        lineWidth = offset + wordWidth;
        line.push_back({ item, offset });
    }
    emitLine();

    y += paraGap;
}

static std::wstring lowerCase(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// First piece of text inside an element (used for <button> labels).
static std::wstring firstText(Element* el) {
    for (auto& child : el->children) {
        if (child->type == Node::TEXT) return static_cast<TextNode*>(child.get())->text;
        std::wstring t = firstText(static_cast<Element*>(child.get()));
        if (!t.empty()) return t;
    }
    return L"";
}

// Computes the cascaded style properties layout uses, for both plain
// elements and form controls: stylesheet rules first (least to most
// specific, source order breaking ties), then inline style="" - which, per
// CSS, always wins regardless of specificity.
LayoutRoot::ComputedStyle LayoutRoot::computeStyle(Element* e, int inheritedFontSize) {
    ComputedStyle sv;
    sv.fontSize = inheritedFontSize; // inherited unless a rule below overrides it
    sv.display = isInlineTag(e->tag) ? Display::Inline : Display::Block;

    auto applyDecl = [&](const std::wstring& k, const std::wstring& v) {
        if (k == L"background" || k == L"background-color") sv.background = v;
        else if (k == L"margin-top") sv.marginTop = parseFontSize(v, 6);
        else if (k == L"margin-bottom") sv.marginBottom = parseFontSize(v, 6);
        else if (k == L"padding") sv.padding = parseFontSize(v, 6);
        else if (k == L"font-size") sv.fontSize = resolveFontSize(v, inheritedFontSize, sv.fontSize);
        else if (k == L"display") {
            if (v == L"none") sv.display = Display::None;
            else if (v == L"inline" || v == L"inline-block") sv.display = Display::Inline;
            else if (v == L"block") sv.display = Display::Block;
        }
    };

    if (rules) {
        std::vector<const CSS::Rule*> matched;
        for (const auto& rule : *rules) {
            if (CSS::matches(rule, ancestorStack, e)) matched.push_back(&rule);
        }
        std::stable_sort(matched.begin(), matched.end(),
            [](const CSS::Rule* a, const CSS::Rule* b) {
                if (a->specificity < b->specificity) return true;
                if (b->specificity < a->specificity) return false;
                return a->order < b->order;
            });
        for (const auto* rule : matched)
            for (const auto& decl : rule->declarations) applyDecl(decl.first, decl.second);
    }
    for (const auto& decl : CSS::parseDeclarations(getAttr(e, L"style", L"")))
        applyDecl(decl.first, decl.second);

    return sv;
}

// Places one <input> or <button> as a box of its own. Controls are laid out
// as blocks like everything else here, so a label and its field end up on
// separate rows.
void LayoutRoot::layoutControl(Element* e, int x, int& y, int containingWidth, const ComputedStyle& style) {
    // Control heights scale with font-size so a bigger font doesn't clip.
    const int fieldHeight = std::max(28, style.fontSize + 14);
    const int buttonHeight = std::max(30, style.fontSize + 16);
    const int checkboxSize = std::max(18, style.fontSize + 4);
    const int maxWidth = std::max(containingWidth, 60);

    std::wstring type = lowerCase(getAttr(e, L"type", L""));

    LayoutBox box;
    box.x = x;
    box.fontSize = style.fontSize;
    box.background = style.background;
    box.el = e;
    box.form = currentForm;

    if (e->tag == L"button") {
        box.control = LayoutBox::Button;
        box.text = firstText(e);
        if (box.text.empty()) box.text = L"Button";
    }
    else if (e->tag == L"select") {
        box.control = LayoutBox::Select;
    }
    else if (type == L"submit" || type == L"button" || type == L"reset" || type == L"image") {
        box.control = LayoutBox::Button;
        box.text = getAttr(e, L"value", L"");
        if (box.text.empty()) box.text = type == L"reset" ? L"Reset" : type == L"button" ? L"" : L"Submit";
    }
    else if (type == L"checkbox") {
        box.control = LayoutBox::Checkbox;
    }
    else if (type == L"hidden" || type == L"radio" || type == L"file" ||
             type == L"range" || type == L"color") {
        return; // hidden takes no space; the rest aren't supported
    }
    else {
        box.control = LayoutBox::TextField; // text, search, email, password, url, ...
    }

    switch (box.control) {
    case LayoutBox::TextField: {
        int chars = parseFontSize(getAttr(e, L"size", L""), 20);
        box.width = std::min(std::max(chars * 8 + 16, 60), maxWidth);
        box.height = fieldHeight;
        break;
    }
    case LayoutBox::Button:
        box.width = std::min(std::max((int)textWidth(box.text, box.fontSize) + 24, 40), maxWidth);
        box.height = buttonHeight;
        break;
    case LayoutBox::Select:
        box.width = std::min(160, maxWidth);
        box.height = fieldHeight;
        break;
    default: // Checkbox
        box.width = checkboxSize;
        box.height = checkboxSize;
        break;
    }

    y += style.marginTop;
    box.y = y;
    boxes.push_back(box);
    y += box.height + style.marginBottom;
}

// Places an <img> as a box of its own. Explicit width/height attributes
// always win (per-axis - a mismatched aspect ratio between an explicit
// width and a natural height is the page's problem, same as real browsers);
// an unset axis uses the image's natural decoded size, fetching/decoding it
// now via `loadImage` so the box gets the real size instead of a guess. A
// fixed placeholder covers the case where that fetch/decode fails.
void LayoutRoot::layoutImage(Element* e, int x, int& y, int containingWidth, const ComputedStyle& style) {
    const int defaultWidth = 200, defaultHeight = 150;
    std::wstring src = getAttr(e, L"src", L"");

    int naturalW = 0, naturalH = 0;
    bool haveNatural = loadImage && !src.empty() && loadImage(src, naturalW, naturalH);

    std::wstring wAttr = getAttr(e, L"width", L"");
    std::wstring hAttr = getAttr(e, L"height", L"");
    int width = !wAttr.empty() ? parseFontSize(wAttr, defaultWidth) : (haveNatural ? naturalW : defaultWidth);
    int height = !hAttr.empty() ? parseFontSize(hAttr, defaultHeight) : (haveNatural ? naturalH : defaultHeight);

    LayoutBox box;
    box.x = x;
    box.fontSize = style.fontSize;
    box.background = style.background;
    box.imageSrc = src;
    box.width = std::min(width, std::max(containingWidth, 1));
    box.height = height;
    box.el = e;

    y += style.marginTop;
    box.y = y;
    boxes.push_back(box);
    y += box.height + style.marginBottom;
}

void LayoutRoot::layout() {
    boxes.clear();
    ancestorStack.clear();
    if (!rootNode) return;

    int y = 10;
    layoutElement(static_cast<Element*>(rootNode), 10, y, viewportWidth - 20, 14);
}

void LayoutRoot::layoutElement(Element* el, int x, int& y, int containingWidth, int inheritedFontSize) {
    if (!el) return;
    ancestorStack.push_back(el); // `el` is an ancestor of every child laid out below

    // Consecutive text/inline-level children (e.g. "Hello <a>link</a> world")
    // are buffered here so they word-wrap together as one flowing run,
    // instead of each one starting a block of its own. A block-level child,
    // or the end of `el`'s children, flushes whatever's pending so far.
    std::vector<InlineItem> pendingInline;
    auto flushInline = [&]() {
        if (!pendingInline.empty()) {
            layoutInlineRun(pendingInline, x, y, containingWidth);
            pendingInline.clear();
        }
    };

    // Loop over each child node
    for (auto& child : el->children) {

        // Case 1: Text node (simple paragraph text)
        if (child->type == Node::TEXT) {
            auto tnode = static_cast<TextNode*>(child.get());
            appendWords(tnode->text, inheritedFontSize, currentHref, el, pendingInline);
        }

        //  Case 2: Element node (<div>, <p>, <span>, etc.)
        else if (child->type == Node::ELEMENT) {
            auto e = static_cast<Element*>(child.get());

            // Non-visual elements produce no boxes and take no space.
            // <noscript> is deliberately not in this list: unlike the others
            // its content is meant to be shown when JS isn't available. The
            // engine does run scripts now, but it doesn't distinguish the two
            // cases, so <noscript> is laid out like a normal container below.
            if (e->tag == L"head" || e->tag == L"script" || e->tag == L"style" ||
                e->tag == L"title" || e->tag == L"meta" || e->tag == L"link" ||
                e->tag == L"base")
                continue;

            if (e->tag == L"br") {
                pendingInline.push_back({ L"", inheritedFontSize, L"", nullptr, true });
                continue;
            }

            ComputedStyle sv = computeStyle(e, inheritedFontSize);
            if (sv.display == Display::None) continue; // this element and its subtree take no space

            if (e->tag == L"input" || e->tag == L"button" || e->tag == L"select") {
                flushInline();
                layoutControl(e, x, y, containingWidth, sv);
                continue;
            }

            if (e->tag == L"img") {
                flushInline();
                layoutImage(e, x, y, containingWidth, sv);
                continue;
            }

            if (sv.display == Display::Inline) {
                // Joins the current run rather than starting a block: its
                // text (and any nested inline content) flows onto the same
                // line as its surrounding siblings.
                std::wstring savedHref = currentHref;
                if (e->tag == L"a") {
                    auto href = e->attrs.find(L"href");
                    if (href != e->attrs.end()) currentHref = href->second;
                }
                collectInline(e, sv.fontSize, pendingInline);
                currentHref = savedHref;
                continue;
            }

            // Block-level: close out any pending inline run first so it
            // renders above this block, in document order.
            flushInline();

            y += sv.marginTop;
            int contentStartY = y;

            // Reserve a background box now (before laying out children) so it
            // paints behind them, but only if this element actually declared
            // one — plain structural wrappers like <html>/<body> get no box
            // at all, they just position their children.
            size_t bgIndex = static_cast<size_t>(-1);
            if (!sv.background.empty()) {
                LayoutBox box;
                box.x = x;
                box.y = contentStartY;
                box.width = containingWidth;
                box.height = 0; // filled in below once children are laid out
                box.background = sv.background;
                box.el = e;
                bgIndex = boxes.size();
                boxes.push_back(box);
            }

            y += sv.padding;

            // Recurse into nested elements/text
            std::wstring savedHref = currentHref;
            if (e->tag == L"a") {
                auto href = e->attrs.find(L"href");
                if (href != e->attrs.end()) currentHref = href->second;
            }
            Element* savedForm = currentForm;
            if (e->tag == L"form") currentForm = e;
            layoutElement(e, x + sv.padding, y, containingWidth - 2 * sv.padding, sv.fontSize);
            currentHref = savedHref;
            currentForm = savedForm;

            y += sv.padding;

            if (bgIndex != static_cast<size_t>(-1)) {
                boxes[bgIndex].height = y - contentStartY;
            }

            y += sv.marginBottom;
        }
    }

    flushInline();
    ancestorStack.pop_back();
}
