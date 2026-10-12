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
    // A comment (<!-- ... --> inside an element, or createComment). Its
    // `text` stays empty, so layout and textContent pass over it like an
    // empty text node; `data` is what it says.
    bool comment = false;
    std::wstring data;
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
    // <script> only. parserInserted: made by the HTML parser (the page's
    // own, which Engine runs in order, or innerHTML's, which never run) -
    // not by a script's createElement. scriptStarted: it ran (or is being
    // fetched to), so inserting it again doesn't run it again.
    bool parserInserted = false;
    bool scriptStarted = false;
    Element(const std::wstring& t) : Node(ELEMENT), tag(t) {}
};

struct Document {
    // The top-level element that contains `body` (normally <html>). Its main
    // job is ownership: body->parent points at it, and it must outlive parsing.
    std::shared_ptr<Element> root;
    std::shared_ptr<Element> body;
    std::vector<CSS::Rule> styles; // from every <style> block on the page
    // Quirks mode: no <!DOCTYPE html> (or an old transitional one). Only a
    // few layout rules differ - see LayoutRoot::quirks.
    bool quirks = true;
};
