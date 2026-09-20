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

// Word-wraps `text` to `containingWidth`, emitting one box per line. Text has
// no background of its own: it sits on whatever its container already painted.
void LayoutRoot::layoutText(const std::wstring& text, int x, int& y, int containingWidth, int fontSize) {
    const int lineHeight = fontSize + 8; // scales with the font instead of a fixed 22px
    const int textInset = 4; // Engine::render draws text 4px inside its box
    const int paraGap = 6;
    // Keep a sane minimum so deeply nested content can't wrap per character.
    const float maxWidth = (float)std::max(containingWidth - 2 * textInset, 40);

    auto emit = [&](const std::wstring& line) {
        LayoutBox box;
        box.x = x;
        box.y = y;
        box.width = containingWidth;
        box.height = lineHeight;
        box.text = line;
        box.href = currentHref;
        box.fontSize = fontSize;
        boxes.push_back(box);
        y += lineHeight;
    };

    std::wstring line;
    size_t i = 0;
    while (i < text.size()) {
        // Next word (a run of non-space characters)
        while (i < text.size() && iswspace(text[i])) i++;
        size_t start = i;
        while (i < text.size() && !iswspace(text[i])) i++;
        if (start == i) break;
        std::wstring word = text.substr(start, i - start);

        std::wstring candidate = line.empty() ? word : line + L" " + word;
        if (textWidth(candidate, fontSize) <= maxWidth) {
            line = candidate;
            continue;
        }

        // Doesn't fit: flush the current line and start a new one.
        if (!line.empty()) { emit(line); line.clear(); }

        // A single word wider than the line gets broken by characters.
        while (textWidth(word, fontSize) > maxWidth && word.size() > 1) {
            size_t n = 1;
            while (n < word.size() && textWidth(word.substr(0, n + 1), fontSize) <= maxWidth) n++;
            emit(word.substr(0, n));
            word.erase(0, n);
        }
        line = word;
    }
    if (!line.empty()) emit(line);

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

    auto applyDecl = [&](const std::wstring& k, const std::wstring& v) {
        if (k == L"background" || k == L"background-color") sv.background = v;
        else if (k == L"margin-top") sv.marginTop = parseFontSize(v, 6);
        else if (k == L"margin-bottom") sv.marginBottom = parseFontSize(v, 6);
        else if (k == L"padding") sv.padding = parseFontSize(v, 6);
        else if (k == L"font-size") sv.fontSize = resolveFontSize(v, inheritedFontSize, sv.fontSize);
        else if (k == L"display") sv.displayNone = (v == L"none");
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

    // Loop over each child node
    for (auto& child : el->children) {

        // Case 1: Text node (simple paragraph text)
        if (child->type == Node::TEXT) {
            auto tnode = static_cast<TextNode*>(child.get());
            layoutText(tnode->text, x, y, containingWidth, inheritedFontSize);
        }

        //  Case 2: Element node (<div>, <p>, <span>, etc.)
        else if (child->type == Node::ELEMENT) {
            auto e = static_cast<Element*>(child.get());

            // Non-visual elements produce no boxes and take no space.
            if (e->tag == L"head" || e->tag == L"script" || e->tag == L"style" ||
                e->tag == L"title" || e->tag == L"meta" || e->tag == L"link" ||
                e->tag == L"base" || e->tag == L"noscript")
                continue;

            ComputedStyle sv = computeStyle(e, inheritedFontSize);
            if (sv.displayNone) continue; // this element and its subtree take no space

            if (e->tag == L"input" || e->tag == L"button") {
                layoutControl(e, x, y, containingWidth, sv);
                continue;
            }

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

    ancestorStack.pop_back();
}
