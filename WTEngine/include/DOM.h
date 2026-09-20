// DOM.h
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <map>
#include "CSS.h"

struct Node {
    enum Type { ELEMENT, TEXT } type;
    Node(Type t) : type(t) {}
    virtual ~Node() = default;
};

struct TextNode : Node {
    std::wstring text;
    TextNode(const std::wstring& t) : Node(TEXT), text(t) {}
};

struct Element : Node {
    std::wstring tag;
    std::map<std::wstring, std::wstring> attrs;
    std::vector<std::shared_ptr<Node>> children;
    // Non-owning; set by whoever attaches this element (HTMLParser during
    // parsing, or the DOM-mutating JS bindings). Used to bubble a click
    // event up through ancestors. nullptr for an unattached element (just
    // created via createElement, or just removed via removeChild).
    Element* parent = nullptr;
    Element(const std::wstring& t) : Node(ELEMENT), tag(t) {}
};

struct Document {
    std::shared_ptr<Element> root;
    std::shared_ptr<Element> body;
    std::vector<CSS::Rule> styles; // from every <style> block on the page
};
