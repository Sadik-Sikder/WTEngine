// CSS.cpp
#include "CSS.h"
#include "DOM.h"
#include <cwctype>
#include <sstream>

namespace CSS {
namespace {

std::wstring trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// Strips /* ... */ comments; an unterminated one runs to the end of input.
std::wstring stripComments(const std::wstring& css) {
    std::wstring out;
    size_t i = 0;
    while (i < css.size()) {
        if (css.compare(i, 2, L"/*") == 0) {
            size_t end = css.find(L"*/", i + 2);
            i = (end == std::wstring::npos) ? css.size() : end + 2;
        }
        else {
            out.push_back(css[i++]);
        }
    }
    return out;
}

// Splits `s` on top-level occurrences of `sep`. Selectors and declaration
// blocks are already isolated from their braces by the time this is used,
// so there's no nesting to worry about.
std::vector<std::wstring> splitTop(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == sep) {
            parts.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

bool isSelectorChar(wchar_t c) {
    return iswalnum(c) || c == L'-' || c == L'_' || c == L'.' || c == L'#' || c == L'*';
}

// Parses one compound selector token, e.g. `div.card#id`, `.card`, or `*`.
// Returns false for syntax we don't support (attributes, pseudo-classes, a
// stray combinator character, ...).
bool parseCompound(const std::wstring& token, CompoundSelector& out) {
    size_t i = 0;
    if (i < token.size() && token[i] == L'*') {
        i++; // universal selector: leave tag empty (matches any element)
    }
    else {
        std::wstring tag;
        while (i < token.size() && (iswalnum(token[i]) || token[i] == L'-')) tag.push_back(token[i++]);
        out.tag = lower(tag);
    }
    while (i < token.size()) {
        if (token[i] == L'.') {
            i++;
            std::wstring cls;
            while (i < token.size() && (iswalnum(token[i]) || token[i] == L'-' || token[i] == L'_'))
                cls.push_back(token[i++]);
            if (cls.empty()) return false;
            out.classes.push_back(cls);
        }
        else if (token[i] == L'#') {
            i++;
            std::wstring id;
            while (i < token.size() && (iswalnum(token[i]) || token[i] == L'-' || token[i] == L'_'))
                id.push_back(token[i++]);
            if (id.empty()) return false;
            out.id = id;
        }
        else {
            return false; // e.g. "[type=text]" or ":hover" leaking through
        }
    }
    return true;
}

// Parses one selector (already split out of a comma-separated group) into a
// descendant chain. False if any part of it is unsupported.
bool parseSelector(const std::wstring& selector, std::vector<CompoundSelector>& chain) {
    std::wistringstream ss(selector);
    std::wstring token;
    while (ss >> token) {
        for (wchar_t c : token) {
            if (!isSelectorChar(c)) return false; // a combinator (>,+,~) or other unsupported syntax
        }
        CompoundSelector cs;
        if (!parseCompound(token, cs)) return false;
        chain.push_back(std::move(cs));
    }
    return !chain.empty();
}

Specificity specificityOf(const std::vector<CompoundSelector>& chain) {
    Specificity sp;
    for (const auto& cs : chain) {
        if (!cs.id.empty()) sp.ids++;
        sp.classes += (int)cs.classes.size();
        if (!cs.tag.empty()) sp.tags++;
    }
    return sp;
}

// Skips a `@media (...) { ... }`-style at-rule (braces may nest) or a
// `@import "x.css";`-style statement. Either way its content never takes
// effect - a page's CSS only applies when unconditional.
size_t skipAtRule(const std::wstring& css, size_t i) {
    size_t stop = css.find_first_of(L"{;", i);
    if (stop == std::wstring::npos) return css.size();
    if (css[stop] == L';') return stop + 1;

    int depth = 1;
    size_t j = stop + 1;
    while (j < css.size() && depth > 0) {
        if (css[j] == L'{') depth++;
        else if (css[j] == L'}') depth--;
        j++;
    }
    return j;
}

std::vector<std::wstring> classesOf(const Element* el) {
    std::vector<std::wstring> out;
    auto it = el->attrs.find(L"class");
    if (it == el->attrs.end()) return out;
    std::wstring cur;
    for (wchar_t c : it->second) {
        if (iswspace(c)) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        }
        else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool compoundMatches(const CompoundSelector& cs, Element* el) {
    if (!el) return false;
    if (!cs.tag.empty() && el->tag != cs.tag) return false;

    if (!cs.id.empty()) {
        auto it = el->attrs.find(L"id");
        if (it == el->attrs.end() || it->second != cs.id) return false;
    }

    if (!cs.classes.empty()) {
        std::vector<std::wstring> have = classesOf(el);
        for (const auto& want : cs.classes) {
            bool found = false;
            for (const auto& h : have) if (h == want) { found = true; break; }
            if (!found) return false;
        }
    }
    return true;
}

} // namespace

std::vector<std::pair<std::wstring, std::wstring>> parseDeclarations(const std::wstring& block) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    for (const auto& stmt : splitTop(block, L';')) {
        size_t colon = stmt.find(L':');
        if (colon == std::wstring::npos) continue;
        std::wstring k = lower(trim(stmt.substr(0, colon)));
        std::wstring v = trim(stmt.substr(colon + 1));

        // "!important" isn't given special priority here; just drop it.
        size_t bang = v.find(L'!');
        if (bang != std::wstring::npos) v = trim(v.substr(0, bang));

        if (k.empty() || v.empty()) continue;
        out.emplace_back(std::move(k), std::move(v));
    }
    return out;
}

std::vector<Rule> parseStylesheet(const std::wstring& cssIn) {
    std::wstring css = stripComments(cssIn);
    std::vector<Rule> rules;
    int order = 0;
    size_t i = 0;

    while (i < css.size()) {
        while (i < css.size() && iswspace(css[i])) i++;
        if (i >= css.size()) break;

        if (css[i] == L'@') { i = skipAtRule(css, i); continue; }
        if (css[i] == L'}') { i++; continue; } // stray closing brace; keep going

        size_t brace = css.find(L'{', i);
        if (brace == std::wstring::npos) break; // trailing garbage; nothing more to parse
        std::wstring selectorText = css.substr(i, brace - i);

        size_t close = css.find(L'}', brace);
        size_t bodyEnd = (close == std::wstring::npos) ? css.size() : close;
        std::wstring body = css.substr(brace + 1, bodyEnd - brace - 1);
        i = (close == std::wstring::npos) ? css.size() : close + 1;

        auto declarations = parseDeclarations(body);
        if (declarations.empty()) { order++; continue; }

        for (const auto& part : splitTop(selectorText, L',')) {
            std::vector<CompoundSelector> chain;
            if (!parseSelector(trim(part), chain)) continue; // unsupported; skip just this one

            Rule rule;
            rule.chain = std::move(chain);
            rule.declarations = declarations;
            rule.specificity = specificityOf(rule.chain);
            rule.order = order;
            rules.push_back(std::move(rule));
        }
        order++;
    }
    return rules;
}

bool matches(const Rule& rule, const std::vector<Element*>& ancestors, Element* el) {
    if (rule.chain.empty() || !compoundMatches(rule.chain.back(), el)) return false;

    // Walk the remaining compounds right-to-left; each must match *some*
    // ancestor above the previous match (a descendant combinator, not
    // necessarily the immediate parent).
    size_t compoundIdx = rule.chain.size() - 1;
    size_t ancestorIdx = ancestors.size();
    while (compoundIdx > 0) {
        compoundIdx--;
        bool found = false;
        while (ancestorIdx > 0) {
            ancestorIdx--;
            if (compoundMatches(rule.chain[compoundIdx], ancestors[ancestorIdx])) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

} // namespace CSS
