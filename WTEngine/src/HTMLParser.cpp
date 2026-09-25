// HTMLParser.cpp
#include "HTMLParser.h"
#include "CSS.h"
#include <cwctype>
#include <set>

// Tags that never have children or a closing tag.
static bool isVoidTag(const std::wstring& t) {
    static const std::set<std::wstring> v = {
        L"br", L"hr", L"img", L"input", L"meta", L"link", L"area", L"base",
        L"col", L"embed", L"source", L"track", L"wbr" };
    return v.count(t) > 0;
}

// Tags whose content is raw text (not markup) up to the matching end tag.
static bool isRawTextTag(const std::wstring& t) {
    return t == L"script" || t == L"style" || t == L"title" || t == L"textarea";
}

static std::wstring decodeEntities(const std::wstring& in) {
    if (in.find(L'&') == std::wstring::npos) return in;
    static const std::map<std::wstring, std::wstring> named = {
        { L"amp", L"&" }, { L"lt", L"<" }, { L"gt", L">" }, { L"quot", L"\"" },
        { L"apos", L"'" }, { L"nbsp", L" " }, { L"copy", std::wstring(1, (wchar_t)0x00A9) },
        { L"mdash", std::wstring(1, (wchar_t)0x2014) }, { L"ndash", std::wstring(1, (wchar_t)0x2013) },
        { L"hellip", std::wstring(1, (wchar_t)0x2026) } };
    std::wstring out;
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == L'&') {
            size_t semi = in.find(L';', i);
            if (semi != std::wstring::npos && semi - i <= 10) {
                std::wstring ent = in.substr(i + 1, semi - i - 1);
                if (!ent.empty() && ent[0] == L'#') {
                    try {
                        bool hex = ent.size() > 1 && (ent[1] == L'x' || ent[1] == L'X');
                        unsigned long cp = std::stoul(ent.substr(hex ? 2 : 1), nullptr, hex ? 16 : 10);
                        if (cp > 0 && cp < 0x10000) { out.push_back((wchar_t)cp); i = semi; continue; }
                    }
                    catch (...) {}
                }
                else {
                    auto it = named.find(ent);
                    if (it != named.end()) { out += it->second; i = semi; continue; }
                }
            }
        }
        out.push_back(in[i]);
    }
    return out;
}

// Skips <!DOCTYPE ...>, <!-- comments -->, and <? ... ?> at the current
// position. Returns true if something was skipped.
bool HTMLParser::skipMarkup() {
    if (pos + 1 >= s.size() || s[pos] != L'<') return false;
    if (startsWith(L"<!--")) {
        size_t end = s.find(L"-->", pos + 4);
        pos = (end == std::wstring::npos) ? s.size() : end + 3;
        return true;
    }
    if (s[pos + 1] == L'!' || s[pos + 1] == L'?') {
        size_t end = s.find(L'>', pos);
        pos = (end == std::wstring::npos) ? s.size() : end + 1;
        return true;
    }
    return false;
}

void HTMLParser::skipSpace() {
    while (pos < s.size() && iswspace(s[pos])) pos++;
}

bool HTMLParser::startsWith(const std::wstring& token) {
    return s.substr(pos, token.size()) == token;
}

std::wstring HTMLParser::parseTagName() {
    skipSpace();
    std::wstring name;
    while (pos < s.size() && iswalnum(s[pos])) { name.push_back((wchar_t)towlower(s[pos++])); }
    return name;
}

std::map<std::wstring, std::wstring> HTMLParser::parseAttributes() {
    std::map<std::wstring, std::wstring> attrs;
    while (true) {
        skipSpace();
        if (pos >= s.size() || s[pos] == L'>' || startsWith(L"/>")) break;
        std::wstring k;
        while (pos < s.size() && (iswalnum(s[pos]) || s[pos] == L'-')) k.push_back((wchar_t)towlower(s[pos++]));
        if (k.empty()) { pos++; continue; } // stray character; avoid looping forever
        skipSpace();
        if (pos < s.size() && s[pos] == L'=') {
            pos++; skipSpace();
            if (pos < s.size() && (s[pos] == L'\'' || s[pos] == L'"')) {
                wchar_t q = s[pos++]; std::wstring val;
                while (pos < s.size() && s[pos] != q) val.push_back(s[pos++]);
                if (pos < s.size() && s[pos] == q) pos++;
                attrs[k] = decodeEntities(val);
            }
            else {
                std::wstring val;
                while (pos < s.size() && !iswspace(s[pos]) && s[pos] != L'>') val.push_back(s[pos++]);
                attrs[k] = decodeEntities(val);
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

    if (pos >= 2 && s.substr(pos - 2, 2) == L"/>") return elem;
    if (isVoidTag(name)) return elem;

    if (isRawTextTag(name)) {
        // Skip everything up to the matching (case-insensitive) end tag.
        std::wstring closer = L"</" + name;
        size_t start = pos; // raw content begins here
        size_t p = pos;
        while (p < s.size()) {
            p = s.find(L"</", p);
            if (p == std::wstring::npos) break;
            bool match = p + closer.size() <= s.size();
            for (size_t i = 0; match && i < closer.size(); i++)
                if (towlower(s[p + i]) != closer[i]) match = false;
            if (match) break;
            p += 2;
        }
        // <style> and <script> content is kept (as plain text, no entity
        // decoding - decoding would corrupt JS source, e.g. turning "&&"
        // into "&") so it can be parsed/run later. <title> is kept too,
        // entity-decoded like normal text ("Tom &amp; Jerry"), for the
        // window title and document.title. <textarea> content is dropped,
        // since the engine doesn't render textareas yet.
        if ((name == L"style" || name == L"script") && p != std::wstring::npos && p > start) {
            elem->children.push_back(std::make_shared<TextNode>(s.substr(start, p - start)));
        }
        if (name == L"title") {
            size_t end = (p == std::wstring::npos) ? s.size() : p;
            if (end > start) elem->children.push_back(std::make_shared<TextNode>(decodeEntities(s.substr(start, end - start))));
        }
        if (p == std::wstring::npos) { pos = s.size(); return elem; }
        size_t gt = s.find(L'>', p);
        pos = (gt == std::wstring::npos) ? s.size() : gt + 1;
        return elem;
    }

    // parse children until </name>
    while (true) {
        skipSpace();
        if (pos >= s.size()) break;
        if (skipMarkup()) continue;
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
            if (child) { child->parent = elem.get(); elem->children.push_back(child); }
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
    return decodeEntities(out.substr(a, b - a));
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

// Collects the text of every <style> element under `el` (including `el`
// itself), in document order, for the caller to parse as one stylesheet.
static void collectStyleText(const std::shared_ptr<Element>& el, std::wstring& out) {
    if (!el) return;
    if (el->tag == L"style") {
        for (auto& child : el->children) {
            if (child->type == Node::TEXT) out += static_cast<TextNode*>(child.get())->text;
        }
        out += L"\n";
        return;
    }
    for (auto& child : el->children) {
        if (child->type == Node::ELEMENT) collectStyleText(std::static_pointer_cast<Element>(child), out);
    }
}

// Searches an element and all its descendants (not just direct children)
// for a <body> tag, since <body> is normally nested inside <html>.
static std::shared_ptr<Element> findBody(const std::shared_ptr<Element>& el) {
    if (!el) return nullptr;
    if (el->tag == L"body") return el;
    for (auto& child : el->children) {
        if (child->type == Node::ELEMENT) {
            auto found = findBody(std::static_pointer_cast<Element>(child));
            if (found) return found;
        }
    }
    return nullptr;
}

std::shared_ptr<Document> HTMLParser::parse(const std::wstring& html) {
    s = html; pos = 0;
    auto doc = std::make_shared<Document>();
    // find <body> element and set it as body; we'll create a fake root if needed
    // simple strategy: parse top-level nodes and search for body. Bare text
    // alongside top-level elements is rare in a full document (real markup
    // wraps everything in <html>/<body>) but matters for a fragment parse
    // (innerHTML=) with no wrapper at all - kept as a top-level TextNode
    // rather than dropped, so e.g. `el.innerHTML = "plain text"` works.
    std::vector<std::shared_ptr<Node>> top;
    while (pos < s.size()) {
        skipSpace();
        if (skipMarkup()) continue;
        if (pos < s.size() && s[pos] == L'<') {
            if (pos + 1 < s.size() && s[pos + 1] == L'/') { // stray end
                pos++; continue;
            }
            auto el = parseElement();
            if (el) top.push_back(el);
        }
        else {
            auto text = parseText();
            if (!text.empty()) top.push_back(std::make_shared<TextNode>(text));
        }
    }
    // find body anywhere in the parsed tree (it's usually nested inside <html>)
    for (auto& n : top) {
        if (n->type != Node::ELEMENT) continue;
        auto found = findBody(std::static_pointer_cast<Element>(n));
        if (found) {
            doc->body = found;
            // <body>'s `parent` points at the element that contains it (normally
            // <html>), which is owned only by `top` and would be freed when this
            // function returns - leaving click bubbling (which walks `parent`
            // up past <body>) reading freed memory. Keep it alive on the Document.
            doc->root = std::static_pointer_cast<Element>(n);
            break;
        }
    }
    if (!doc->body) {
        // create synthetic body merging top-level nodes
        auto body = std::make_shared<Element>(L"body");
        for (auto& n : top) {
            if (n->type == Node::ELEMENT) static_cast<Element*>(n.get())->parent = body.get();
            body->children.push_back(n);
        }
        doc->body = body;
    }

    std::wstring cssText;
    for (auto& n : top) {
        if (n->type == Node::ELEMENT) collectStyleText(std::static_pointer_cast<Element>(n), cssText);
    }
    doc->styles = CSS::parseStylesheet(cssText);

    return doc;
}
