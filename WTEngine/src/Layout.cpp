#define NOMINMAX
#include "Layout.h"
#include <windows.h>
#include <string>
#include <sstream>
#include <algorithm>

std::wstring LayoutRoot::getAttr(Element* el, const std::wstring& key, const std::wstring& def) {
    if (!el) return def;
    auto it = el->attrs.find(key);
    if (it == el->attrs.end()) return def;
    return it->second;
}

int LayoutRoot::parseFontSize(const std::wstring& s, int def) {
    if (s.empty()) return def;
    try {
        std::wstring num;
        for (wchar_t c : s) {
            if (c >= L'0' && c <= L'9') num.push_back(c);
            else break;
        }
        if (!num.empty()) return std::stoi(num);
    }
    catch (...) {}
    return def;
}

float LayoutRoot::textWidth(const std::wstring& text, int fontSize) {
    if (measureText) return measureText(text, fontSize);
    return text.size() * fontSize * 0.55f; // rough fallback
}

// Word-wraps `text` to `containingWidth`, emitting one box per line. Text has
// no background of its own: it sits on whatever its container already painted.
void LayoutRoot::layoutText(const std::wstring& text, int x, int& y, int containingWidth) {
    const int fontSize = 14;
    const int lineHeight = 22;
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

void LayoutRoot::layout() {
    boxes.clear();
    if (!rootNode) return;

    int y = 10;
    layoutElement(static_cast<Element*>(rootNode), 10, y, viewportWidth - 20);
}

void LayoutRoot::layoutElement(Element* el, int x, int& y, int containingWidth) {
    if (!el) return;

    // Loop over each child node
    for (auto& child : el->children) {

        // Case 1: Text node (simple paragraph text)
        if (child->type == Node::TEXT) {
            auto tnode = static_cast<TextNode*>(child.get());
            layoutText(tnode->text, x, y, containingWidth);
        }

        //  Case 2: Element node (<div>, <p>, <span>, etc.)
        else if (child->type == Node::ELEMENT) {
            auto e = static_cast<Element*>(child.get());

            // Non-visual elements produce no boxes and take no space.
            if (e->tag == L"head" || e->tag == L"script" || e->tag == L"style" ||
                e->tag == L"title" || e->tag == L"meta" || e->tag == L"link" ||
                e->tag == L"base" || e->tag == L"noscript")
                continue;

            std::wstring style = getAttr(e, L"style", L"");

            // default style values
            std::wstring bg = L"";
            int marginTop = 6;
            int marginBottom = 6;
            int padding = 6;
            int fontSize = 14;

            // rudimentary inline style parser ---
            std::wistringstream ss(style);
            std::wstring token;
            while (std::getline(ss, token, L';')) {
                auto pos = token.find(L':');
                if (pos == std::wstring::npos) continue;
                std::wstring k = token.substr(0, pos);
                std::wstring v = token.substr(pos + 1);

                auto trim = [](std::wstring& t) {
                    while (!t.empty() && iswspace(t.front())) t.erase(t.begin());
                    while (!t.empty() && iswspace(t.back())) t.pop_back();
                    };
                trim(k); trim(v);

                if (k == L"background" || k == L"background-color") bg = v;
                else if (k == L"margin-top") marginTop = parseFontSize(v, 6);
                else if (k == L"margin-bottom") marginBottom = parseFontSize(v, 6);
                else if (k == L"padding") padding = parseFontSize(v, 6);
                else if (k == L"font-size") fontSize = parseFontSize(v, 14);
            }

            y += marginTop;
            int contentStartY = y;

            // Reserve a background box now (before laying out children) so it
            // paints behind them, but only if this element actually declared
            // one — plain structural wrappers like <html>/<body> get no box
            // at all, they just position their children.
            size_t bgIndex = static_cast<size_t>(-1);
            if (!bg.empty()) {
                LayoutBox box;
                box.x = x;
                box.y = contentStartY;
                box.width = containingWidth;
                box.height = 0; // filled in below once children are laid out
                box.background = bg;
                bgIndex = boxes.size();
                boxes.push_back(box);
            }

            y += padding;

            // Recurse into nested elements/text
            std::wstring savedHref = currentHref;
            if (e->tag == L"a") {
                auto href = e->attrs.find(L"href");
                if (href != e->attrs.end()) currentHref = href->second;
            }
            layoutElement(e, x + padding, y, containingWidth - 2 * padding);
            currentHref = savedHref;

            y += padding;

            if (bgIndex != static_cast<size_t>(-1)) {
                boxes[bgIndex].height = y - contentStartY;
            }

            y += marginBottom;
        }
    }
}
