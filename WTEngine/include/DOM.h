// DOM.h
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <map>

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
    Element(const std::wstring& t) : Node(ELEMENT), tag(t) {}
};

struct Document {
    std::shared_ptr<Element> root;
    std::shared_ptr<Element> body;
};
#pragma once
