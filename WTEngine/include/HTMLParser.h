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
    void skipSpace();
    bool skipMarkup();
    bool startsWith(const std::wstring& token);
    std::wstring parseTagName();
    std::map<std::wstring, std::wstring> parseAttributes();
    std::shared_ptr<Element> parseElement();
    std::shared_ptr<Node> parseNode();
    std::wstring parseText();
};
