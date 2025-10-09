// Layout.cpp
#include "Layout.h"
#define NOMINMAX
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
    // expects values like "18px"
    if (s.empty()) return def;
    try {
        std::wstring num;
        for (wchar_t c : s) {
            if ((c >= '0' && c <= '9')) num.push_back(c);
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
    int y = 8;
    layoutElement(static_cast<Element*>(rootNode), 8, y, viewportWidth - 16);
}

void LayoutRoot::layoutElement(Element* el, int x, int& y, int containingWidth) {
    // for each child: if element -> create block box, measure text and add to boxes
    for (auto& child : el->children) {
        if (child->type == Node::TEXT) {
            // text node under body directly -> wrap into a paragraph box
            auto tnode = static_cast<TextNode*>(child.get());
            LayoutBox box;
            box.x = x; box.y = y; box.width = containingWidth;
            box.text = tnode->text;
            box.fontSize = 14;
            // need to measure height using a temporary DC later in render; approximate height here
            box.height = 20 + (int)(tnode->text.size() / 50); // crude
            boxes.push_back(box);
            y += box.height + 8;
        }
        else if (child->type == Node::ELEMENT) {
            auto e = static_cast<Element*>(child.get());
            std::wstring tag = e->tag;
            // compute style props
            std::wstring style = getAttr(e, L"style", L"");
            std::wstring bg = L"";
            int marginTop = 6, marginBottom = 6, padding = 6;
            int fontSize = 14;
            // parse inline styles rudimentary: split by ';' and :
            std::wistringstream ss(style);
            std::wstring token;
            while (std::getline(ss, token, L';')) {
                auto pos = token.find(L':');
                if (pos == std::wstring::npos) continue;
                std::wstring k = token.substr(0, pos);
                std::wstring v = token.substr(pos + 1);
                // trim
                auto trim = [](std::wstring& t) { while (!t.empty() && iswspace(t.front())) t.erase(t.begin()); while (!t.empty() && iswspace(t.back())) t.pop_back(); };
                trim(k); trim(v);
                if (k == L"background") bg = v;
                if (k == L"background-color") bg = v;
                if (k == L"margin-top") marginTop = std::stoi(std::string(v.begin(), v.end()));
                if (k == L"margin-bottom") marginBottom = std::stoi(std::string(v.begin(), v.end()));
                if (k == L"padding") padding = std::stoi(std::string(v.begin(), v.end()));
                if (k == L"font-size") fontSize = parseFontSize(v, 14);
            }

            y += marginTop;
            // build a container box that may include combined text of children (simple approach)
            // gather all text under this element into one box
            std::wstring combined;
            for (auto& gc : e->children) {
                if (gc->type == Node::TEXT) combined += static_cast<TextNode*>(gc.get())->text + L"\n";
                else if (gc->type == Node::ELEMENT) {
                    // if child is span or inline, we try to gather
                    auto gel = static_cast<Element*>(gc.get());
                    if (gel->children.size() == 1 && gel->children[0]->type == Node::TEXT) {
                        combined += static_cast<TextNode*>(gel->children[0].get())->text + L" ";
                    }
                    else {
                        // nested block -> layout recursively
                    }
                }
            }

            LayoutBox box;
            box.x = x;
            box.y = y;
            box.width = containingWidth;
            box.background = bg;
            box.fontSize = fontSize;
            box.text = combined;
            // crude measurement: use average characters per line to estimate height
            int avgCharsPerLine = std::max(30, containingWidth / 8);
            int lines = 1;
            if (!combined.empty()) {
                int chars = (int)combined.size();
                lines = (chars / avgCharsPerLine) + 1;
            }
            box.height = padding * 2 + lines * (fontSize + 6);
            boxes.push_back(box);
            y += box.height + marginBottom;

            // layout nested block children (child elements which are block)
            for (auto& gc : e->children) {
                if (gc->type == Node::ELEMENT) {
                    auto gel = static_cast<Element*>(gc.get());
                    // if tag is block-like, layout recursively
                    if (gel->tag == L"div" || gel->tag == L"p") {
                        layoutElement(gel, x + 6, y, containingWidth - 12);
                    }
                }
            }
        }
    }
}
