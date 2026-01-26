// HTMLParser.cpp
#include "HTMLParser.h"
#include <cwctype>

void HTMLParser::skipSpace() {
    while (pos < s.size() && iswspace(s[pos])) pos++;
}

bool HTMLParser::startsWith(const std::wstring& token) {
    return s.substr(pos, token.size()) == token;
}

std::wstring HTMLParser::parseTagName() {
    skipSpace();
    std::wstring name;
    while (pos < s.size() && iswalnum(s[pos])) { name.push_back(s[pos++]); }
    return name;
}

std::map<std::wstring, std::wstring> HTMLParser::parseAttributes() {
    std::map<std::wstring, std::wstring> attrs;
    while (true) {
        skipSpace();
        if (pos >= s.size() || s[pos] == L'>' || startsWith(L"/>")) break;
        std::wstring k;
        while (pos < s.size() && (iswalnum(s[pos]) || s[pos] == L'-')) k.push_back(s[pos++]);
        skipSpace();
        if (pos < s.size() && s[pos] == L'=') {
            pos++; skipSpace();
            if (pos < s.size() && (s[pos] == L'\'' || s[pos] == L'"')) {
                wchar_t q = s[pos++]; std::wstring val;
                while (pos < s.size() && s[pos] != q) val.push_back(s[pos++]);
                if (pos < s.size() && s[pos] == q) pos++;
                attrs[k] = val;
            }
            else {
                std::wstring val;
                while (pos < s.size() && !iswspace(s[pos]) && s[pos] != L'>') val.push_back(s[pos++]);
                attrs[k] = val;
            }
        }
        else {
            attrs[k] = L"";
        }
    }
    return attrs;
}

std::shared_ptr<Element> HTMLParser::parseElement() {
    // assumes current pos at '<'
    if (s[pos] != L'<') return nullptr;
    pos++; // consume '<'
    if (pos < s.size() && s[pos] == L'/') return nullptr;
    std::wstring name = parseTagName();
    auto elem = std::make_shared<Element>(name);
    auto attrs = parseAttributes();
    elem->attrs = attrs;
    // consume '>'
    while (pos < s.size() && s[pos] != L'>') pos++;
    if (pos < s.size() && s[pos] == L'>') pos++;

    if (s.substr(pos - 2, 2) == L"/>") return elem;

    // parse children until </name>
    while (true) {
        skipSpace();
        if (pos >= s.size()) break;
        if (startsWith(L"</")) {
            // read end tag
            pos += 2;
            auto endName = parseTagName();
            while (pos < s.size() && s[pos] != L'>') pos++;
            if (pos < s.size() && s[pos] == L'>') pos++;
            break;
        }
        if (s[pos] == L'<') {
            if (pos + 1 < s.size() && s[pos + 1] == L'/') continue;
            auto child = parseElement();
            if (child) elem->children.push_back(child);
            else { pos++; }
        }
        else {
            auto text = parseText();
            if (!text.empty()) elem->children.push_back(std::make_shared<TextNode>(text));
        }
    }
    return elem;
}

std::wstring HTMLParser::parseText() {
    std::wstring out;
    while (pos < s.size() && s[pos] != L'<') {
        out.push_back(s[pos++]);
    }
    // trim
    size_t a = 0, b = out.size();
    while (a < b && iswspace(out[a])) a++;
    while (b > a && iswspace(out[b - 1])) b--;
    return out.substr(a, b - a);
}

std::shared_ptr<Node> HTMLParser::parseNode() {
    skipSpace();
    if (pos < s.size()) {
        if (s[pos] == L'<') {
            return parseElement();
        }
        else {
            auto t = parseText();
            if (!t.empty()) return std::make_shared<TextNode>(t);
        }
    }
    return nullptr;
}

std::shared_ptr<Document> HTMLParser::parse(const std::wstring& html) {
    s = html; pos = 0;
    auto doc = std::make_shared<Document>();
    // find <body> element and set it as body; we'll create a fake root if needed
    // simple strategy: parse top-level elements and search for body
    std::vector<std::shared_ptr<Element>> top;
    while (pos < s.size()) {
        skipSpace();
        if (pos < s.size() && s[pos] == L'<') {
            if (pos + 1 < s.size() && s[pos + 1] == L'/') { // stray end
                pos++; continue;
            }
            auto el = parseElement();
            if (el) top.push_back(el);
        }
        else break;
    }
    // find body
    for (auto& e : top) {
        if (e->tag == L"body") { doc->body = e; break; }
    }
    if (!doc->body) {
        // create synthetic body merging top-level elements
        auto body = std::make_shared<Element>(L"body");
        for (auto& e : top) body->children.push_back(e);
        doc->body = body;
    }
    return doc;
}
