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
            LayoutBox box;
            box.x = x;
            box.y = y;
            box.width = containingWidth;
            box.text = tnode->text;
            box.fontSize = 14;
            box.height = 22;
            box.background = L"#ffffff"; // white background for text nodes
            boxes.push_back(box);
            y += box.height + 6;
        }

        //  Case 2: Element node (<div>, <p>, <span>, etc.)
        else if (child->type == Node::ELEMENT) {
            auto e = static_cast<Element*>(child.get());
            std::wstring tag = e->tag;
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
                else if (k == L"margin-top") marginTop = std::stoi(std::string(v.begin(), v.end()));
                else if (k == L"margin-bottom") marginBottom = std::stoi(std::string(v.begin(), v.end()));
                else if (k == L"padding") padding = std::stoi(std::string(v.begin(), v.end()));
                else if (k == L"font-size") fontSize = parseFontSize(v, 14);
            }

            y += marginTop;

            // Combine all direct text children into one string
            std::wstring combined;
            for (auto& gc : e->children) {
                if (gc->type == Node::TEXT)
                    combined += static_cast<TextNode*>(gc.get())->text + L" ";
                else if (gc->type == Node::ELEMENT) {
                    auto gel = static_cast<Element*>(gc.get());
                    // Inline elements like <span>
                    if (gel->tag == L"span" && !gel->children.empty() && gel->children[0]->type == Node::TEXT)
                        combined += static_cast<TextNode*>(gel->children[0].get())->text + L" ";
                }
            }

            // Create box for this element
            LayoutBox box;
            box.x = x;
            box.y = y;
            box.width = containingWidth;
            box.fontSize = fontSize;
            box.background = bg.empty() ? L"#e0e0e0" : bg;
            box.text = combined.empty() ? (L"[" + tag + L"]") : combined;

            // Estimate height (roughly 1 line per 30 chars)
            int lines = std::max(1, (int)(box.text.size() / 30));
            box.height = padding * 2 + lines * (fontSize + 6);

            boxes.push_back(box);
            y += box.height + marginBottom;

            // Recurse into nested elements
            layoutElement(e, x + padding, y, containingWidth - 2 * padding);
        }
    }
}
