// HTMLParser.h
#pragma once
#include <string>
#include "DOM.h"

class HTMLParser {
public:
    std::shared_ptr<Document> parse(const std::wstring& html);
private:
    size_t pos;
    std::wstring s;
    // Tag names of the elements being parsed, outermost first: an end tag
    // naming one of them closes everything opened inside it.
    std::vector<std::wstring> open_;
    std::wstring peekTagName(size_t at);
    void skipSpace();
    bool skipMarkup();
    bool startsWith(const std::wstring& token);
    std::wstring parseTagName();
    std::map<std::wstring, std::wstring> parseAttributes();
    std::shared_ptr<Element> parseElement();
    std::shared_ptr<Node> parseNode();
    std::wstring parseText();
};
