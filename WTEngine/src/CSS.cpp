// CSS.cpp
#include "CSS.h"
#include "DOM.h"
#include <algorithm>
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

// Splits `s` on top-level occurrences of `sep` - not inside (...) / [...]
// or a quoted string, so ":not(.a, .b)" and "url(data:x;base64,...)" stay
// whole. Braces are already stripped off by the time this is used.
std::vector<std::wstring> splitTop(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    int depth = 0;
    wchar_t quote = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i < s.size()) {
            wchar_t c = s[i];
            if (c == L'\\') { if (i + 1 < s.size()) i++; continue; }
            if (quote) { if (c == quote) quote = 0; continue; }
            if (c == L'"' || c == L'\'') { quote = c; continue; }
            if (c == L'(' || c == L'[') { depth++; continue; }
            if ((c == L')' || c == L']') && depth > 0) { depth--; continue; }
        }
        if (i >= s.size() || (s[i] == sep && depth == 0)) {
            parts.push_back(s.substr(start, std::min(i, s.size()) - start));
            start = i + 1;
        }
    }
    return parts;
}

// Characters that can appear in a CSS identifier (tag, class, id, attribute
// or pseudo-class name) without escaping. Non-ASCII is allowed, as in CSS.
bool isIdentChar(wchar_t c) {
    return iswalnum(c) || c == L'-' || c == L'_' || c >= 0x80;
}

// Reads an identifier starting at `i`, advancing past it. A backslash
// escapes the next character literally ("md\:flex" - Tailwind-style class
// names rely on this); hex escapes ("\31 0") aren't decoded.
std::wstring readIdent(const std::wstring& s, size_t& i) {
    std::wstring out;
    while (i < s.size()) {
        if (s[i] == L'\\' && i + 1 < s.size()) { out.push_back(s[i + 1]); i += 2; }
        else if (isIdentChar(s[i])) out.push_back(s[i++]);
        else break;
    }
    return out;
}

// Given s[open] is '(' or '[', returns the index of its matching closer,
// skipping over quoted strings and backslash escapes - npos if unterminated.
size_t matchBracket(const std::wstring& s, size_t open) {
    wchar_t o = s[open], c = (o == L'(') ? L')' : L']';
    int depth = 0;
    for (size_t j = open; j < s.size(); j++) {
        if (s[j] == L'\\') { j++; continue; }
        if (s[j] == L'"' || s[j] == L'\'') {
            wchar_t q = s[j];
            for (j++; j < s.size() && s[j] != q; j++) if (s[j] == L'\\') j++;
            continue;
        }
        if (s[j] == o) depth++;
        else if (s[j] == c && --depth == 0) return j;
    }
    return std::wstring::npos;
}

// Parses an :nth-*() argument - "odd", "even", "3", "2n+1", "-n+3", "n" -
// into (a, b). False for anything else, including the "An+B of S" form.
bool parseNth(std::wstring arg, int& a, int& b) {
    std::wstring s;
    for (wchar_t c : lower(arg)) if (!iswspace(c)) s.push_back(c);
    if (s == L"odd") { a = 2; b = 1; return true; }
    if (s == L"even") { a = 2; b = 0; return true; }
    if (s.empty()) return false;
    auto toInt = [](const std::wstring& t, int& out) {
        if (t.empty()) return false;
        size_t used = 0;
        try { out = std::stoi(t, &used); } catch (...) { return false; }
        return used == t.size();
    };
    size_t n = s.find(L'n');
    if (n == std::wstring::npos) { a = 0; return toInt(s, b); }
    std::wstring as = s.substr(0, n), bs = s.substr(n + 1);
    if (as.empty() || as == L"+") a = 1;
    else if (as == L"-") a = -1;
    else if (!toInt(as, a)) return false;
    if (bs.empty()) { b = 0; return true; }
    if (bs[0] != L'+' && bs[0] != L'-') return false;
    return toInt(bs, b);
}

bool parseCompound(const std::wstring& token, CompoundSelector& out);

// Parses the pseudo-class starting at token[i] == ':' into `out`, advancing
// `i` past it. False for a pseudo-element or an unsupported pseudo-class.
bool parsePseudo(const std::wstring& token, size_t& i, CompoundSelector& out) {
    i++; // ':'
    if (i < token.size() && token[i] == L':') return false; // ::pseudo-element
    std::wstring name = lower(readIdent(token, i));
    if (name.empty()) return false;

    std::wstring arg;
    bool hasArg = false;
    if (i < token.size() && token[i] == L'(') {
        size_t close = matchBracket(token, i);
        if (close == std::wstring::npos) return false;
        arg = trim(token.substr(i + 1, close - i - 1));
        hasArg = true;
        i = close + 1;
    }

    if (name == L"not") {
        // Each comma-separated argument must be a single compound (no
        // combinators): :not(a, b) is "neither a nor b".
        if (!hasArg) return false;
        for (const auto& part : splitTop(arg, L',')) {
            std::wstring p = trim(part);
            for (wchar_t c : p) if (iswspace(c) || c == L'>' || c == L'+' || c == L'~') return false;
            CompoundSelector inner;
            if (p.empty() || !parseCompound(p, inner)) return false;
            out.nots.push_back(std::move(inner));
        }
        return true;
    }

    PseudoClass pc{};
    static const std::pair<const wchar_t*, PseudoClass::Kind> kNthKinds[] = {
        { L"nth-child", PseudoClass::NthChild }, { L"nth-last-child", PseudoClass::NthLastChild },
        { L"nth-of-type", PseudoClass::NthOfType }, { L"nth-last-of-type", PseudoClass::NthLastOfType },
    };
    for (const auto& [n, kind] : kNthKinds) {
        if (name == n) {
            if (!hasArg || !parseNth(arg, pc.a, pc.b)) return false;
            pc.kind = kind;
            out.pseudos.push_back(pc);
            return true;
        }
    }
    if (hasArg) return false; // :is(), :where(), :has(), :lang(), ...

    static const std::pair<const wchar_t*, PseudoClass::Kind> kKinds[] = {
        { L"hover", PseudoClass::Hover },
        { L"first-child", PseudoClass::FirstChild }, { L"last-child", PseudoClass::LastChild },
        { L"only-child", PseudoClass::OnlyChild },
        { L"first-of-type", PseudoClass::FirstOfType }, { L"last-of-type", PseudoClass::LastOfType },
        { L"only-of-type", PseudoClass::OnlyOfType },
        { L"root", PseudoClass::Root }, { L"empty", PseudoClass::Empty },
        { L"link", PseudoClass::AnyLink }, { L"any-link", PseudoClass::AnyLink },
        // Real states this engine never enters (no visited history, no
        // CSS-visible focus/active/target tracking) - accepted so the rest
        // of a selector list still parses, but never true.
        { L"visited", PseudoClass::Never }, { L"active", PseudoClass::Never },
        { L"focus", PseudoClass::Never }, { L"focus-visible", PseudoClass::Never },
        { L"focus-within", PseudoClass::Never }, { L"target", PseudoClass::Never },
    };
    for (const auto& [n, kind] : kKinds) {
        if (name == n) {
            pc.kind = kind;
            out.pseudos.push_back(pc);
            return true;
        }
    }
    return false; // includes CSS2's single-colon :before/:after/:first-line/:first-letter
}

// Parses the attribute selector starting at token[i] == '[' into `out`,
// advancing `i` past its ']'.
bool parseAttr(const std::wstring& token, size_t& i, CompoundSelector& out) {
    size_t close = matchBracket(token, i);
    if (close == std::wstring::npos) return false;
    std::wstring body = token.substr(i + 1, close - i - 1);
    i = close + 1;

    size_t j = 0;
    auto skipWs = [&]() { while (j < body.size() && iswspace(body[j])) j++; };
    skipWs();
    AttrSelector as;
    as.name = lower(readIdent(body, j));
    if (as.name.empty()) return false;
    skipWs();
    if (j < body.size()) {
        if (body[j] == L'=') { as.op = L'='; j++; }
        else if (j + 1 < body.size() && body[j + 1] == L'=' &&
                 std::wstring(L"~|^$*").find(body[j]) != std::wstring::npos) { as.op = body[j]; j += 2; }
        else return false;
        skipWs();
        if (j < body.size() && (body[j] == L'"' || body[j] == L'\'')) {
            wchar_t q = body[j++];
            while (j < body.size() && body[j] != q) {
                if (body[j] == L'\\' && j + 1 < body.size()) j++;
                as.value.push_back(body[j++]);
            }
            if (j >= body.size()) return false;
            j++;
        } else {
            as.value = readIdent(body, j);
            if (as.value.empty()) return false;
        }
        skipWs();
        if (j < body.size() && (body[j] == L'i' || body[j] == L'I')) { as.caseInsensitive = true; j++; }
        else if (j < body.size() && (body[j] == L's' || body[j] == L'S')) j++;
        skipWs();
    }
    if (j != body.size()) return false;
    out.attrs.push_back(std::move(as));
    return true;
}

// Parses one compound selector token, e.g. `div.card#id[title]:hover`,
// `.card`, or `*`. Returns false for syntax we don't support.
bool parseCompound(const std::wstring& token, CompoundSelector& out) {
    size_t i = 0;
    if (i < token.size() && token[i] == L'*') {
        i++; // universal selector: leave tag empty (matches any element)
    }
    else {
        out.tag = lower(readIdent(token, i));
    }
    while (i < token.size()) {
        if (token[i] == L'.') {
            i++;
            std::wstring cls = readIdent(token, i);
            if (cls.empty()) return false;
            out.classes.push_back(cls);
        }
        else if (token[i] == L'#') {
            i++;
            std::wstring id = readIdent(token, i);
            if (id.empty()) return false;
            out.id = id;
        }
        else if (token[i] == L'[') {
            if (!parseAttr(token, i, out)) return false;
        }
        else if (token[i] == L':') {
            if (!parsePseudo(token, i, out)) return false;
        }
        else {
            return false; // a stray character
        }
    }
    return !(out.tag.empty() && i == 0); // an empty token is not a selector
}

Specificity specificityOf(const CompoundSelector& cs) {
    Specificity sp;
    if (!cs.id.empty()) sp.ids++;
    sp.classes += (int)(cs.classes.size() + cs.attrs.size() + cs.pseudos.size());
    if (!cs.tag.empty()) sp.tags++;
    // :not() adds its argument's specificity (the most specific one, for a list).
    Specificity best;
    for (const auto& n : cs.nots) {
        Specificity s = specificityOf(n);
        if (best < s) best = s;
    }
    sp.ids += best.ids; sp.classes += best.classes; sp.tags += best.tags;
    return sp;
}

Specificity specificityOf(const std::vector<CompoundSelector>& chain) {
    Specificity sp;
    for (const auto& cs : chain) {
        Specificity s = specificityOf(cs);
        sp.ids += s.ids; sp.classes += s.classes; sp.tags += s.tags;
    }
    return sp;
}

// Finds the index of the '}' matching the '{' at `openBrace`, honoring
// nested braces - npos if unterminated. Needed because real-world CSS
// nests rules inside other rules' declaration blocks (native CSS nesting,
// e.g. ".a{ .b{...} .c{...} }" - Wikipedia's stylesheets use this), so a
// plain css.find('}', ...) from the body's start would stop at the first
// nested rule's closing brace instead of the real end, desyncing every
// rule parsed after it for the rest of the file.
size_t matchBrace(const std::wstring& css, size_t openBrace) {
    int depth = 1;
    size_t j = openBrace + 1;
    while (j < css.size() && depth > 0) {
        if (css[j] == L'{') depth++;
        else if (css[j] == L'}') depth--;
        if (depth == 0) return j;
        j++;
    }
    return std::wstring::npos;
}

// Whether a @media condition (the raw text between "@media" and its "{")
// is simple enough to evaluate outright: no parenthesized feature query
// (min-width, prefers-color-scheme, hover, ...) and no comma-separated
// query list - just a bare media type, or none at all. This engine only
// ever renders on-screen, so "screen"/"all"/empty are always true and
// anything else (chiefly "print") is always false - covering the very
// common "exclude this from print" pattern real stylesheets use. A
// feature query needs the current viewport size at *match* time (layout
// re-runs it on resize), which this parser has no access to at all - left
// unevaluated (falls back to skipAtRule, same as today) rather than
// guessed at.
bool isSimpleScreenMedia(const std::wstring& conditionRaw) {
    std::wstring condition = trim(conditionRaw);
    if (condition.find(L'(') != std::wstring::npos) return false;
    if (condition.find(L',') != std::wstring::npos) return false;
    return condition.empty() || condition == L"all" || condition == L"screen";
}

// A @media condition this engine can evaluate against the viewport at
// layout time (unlike isSimpleScreenMedia's parse-time-decided case).
// `supported` false means "fall back to skipAtRule", same meaning as
// isSimpleScreenMedia returning false.
struct MediaCondition {
    bool supported = false;
    int minWidthPx = -1, maxWidthPx = -1;
};

// Parses a condition like "screen and (min-width:1120px)" - a media type
// (screen/all; print or anything else falls back to unsupported, same as
// isSimpleScreenMedia) ANDed with zero or more (min-width:Npx)/
// (max-width:Npx) features (any other feature, or a non-px unit, is
// unsupported). No comma-separated query list (a real query *list* means
// "match if ANY one applies" - supporting that would need each Rule to
// carry multiple alternative ranges instead of one, not worth it for how
// rarely a list is mixed with min-width/max-width in practice).
MediaCondition parseMediaCondition(const std::wstring& conditionRaw) {
    MediaCondition mc;
    std::wstring condition = trim(conditionRaw);
    if (condition.empty() || condition.find(L',') != std::wstring::npos) return mc;

    std::vector<std::wstring> parts;
    size_t pos = 0;
    while (true) {
        size_t andPos = condition.find(L" and ", pos);
        size_t end = (andPos == std::wstring::npos) ? condition.size() : andPos;
        parts.push_back(trim(condition.substr(pos, end - pos)));
        if (andPos == std::wstring::npos) break;
        pos = andPos + 5;
    }

    bool sawType = false;
    for (auto& part : parts) {
        if (part.empty()) return MediaCondition{};
        if (part.front() == L'(') {
            if (part.back() != L')') return MediaCondition{};
            std::wstring inner = trim(part.substr(1, part.size() - 2));
            size_t colon = inner.find(L':');
            if (colon == std::wstring::npos) return MediaCondition{};
            std::wstring feature = trim(inner.substr(0, colon));
            std::wstring value = trim(inner.substr(colon + 1));
            if (value.size() < 3 || value.compare(value.size() - 2, 2, L"px") != 0) return MediaCondition{};
            int px;
            try { px = std::stoi(value.substr(0, value.size() - 2)); }
            catch (...) { return MediaCondition{}; }
            if (feature == L"min-width") mc.minWidthPx = px;
            else if (feature == L"max-width") mc.maxWidthPx = px;
            else return MediaCondition{};
        } else {
            if (sawType) return MediaCondition{};
            sawType = true;
            if (part != L"screen" && part != L"all") return MediaCondition{}; // "print" (or anything else) included
        }
    }
    mc.supported = true;
    return mc;
}

// Skips a `@media (...) { ... }`-style at-rule (braces may nest) or a
// `@import "x.css";`-style statement. Either way its content never takes
// effect - a page's CSS only applies when unconditional.
size_t skipAtRule(const std::wstring& css, size_t i) {
    size_t stop = css.find_first_of(L"{;", i);
    if (stop == std::wstring::npos) return css.size();
    if (css[stop] == L';') return stop + 1;

    size_t close = matchBrace(css, stop);
    return close == std::wstring::npos ? css.size() : close + 1;
}

std::vector<std::wstring> classesOfUncached(const Element* el) {
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

// Cache-aware wrapper: on a hit, returns the already-split class list
// without touching the "class" attribute string again at all. On a miss
// (or no cache given), splits it - the same work classesOfUncached always
// did - storing the result for next time if there's a cache to store it in.
// Returning a reference (not a copy) matters here: this is called once per
// rule with a class selector for every element under consideration, so
// even the copy alone would have undone most of the savings.
const std::vector<std::wstring>& classesOf(const Element* el, CSS::ClassCache* cache) {
    if (!cache) {
        thread_local std::vector<std::wstring> scratch;
        scratch = classesOfUncached(el);
        return scratch;
    }
    auto [it, inserted] = cache->try_emplace(el);
    if (inserted) it->second = classesOfUncached(el);
    return it->second;
}

bool attrMatches(const AttrSelector& as, const Element* el) {
    auto it = el->attrs.find(as.name);
    if (it == el->attrs.end()) return false;
    if (as.op == 0) return true;

    std::wstring have = as.caseInsensitive ? lower(it->second) : it->second;
    std::wstring want = as.caseInsensitive ? lower(as.value) : as.value;
    switch (as.op) {
    case L'=': return have == want;
    case L'~': { // whitespace-separated list contains `want`
        if (want.empty()) return false;
        std::wistringstream ss(have);
        for (std::wstring w; ss >> w;) if (w == want) return true;
        return false;
    }
    case L'|': return have == want || have.rfind(want + L"-", 0) == 0;
    case L'^': return !want.empty() && have.rfind(want, 0) == 0;
    case L'$': return !want.empty() && have.size() >= want.size() &&
                      have.compare(have.size() - want.size(), want.size(), want) == 0;
    case L'*': return !want.empty() && have.find(want) != std::wstring::npos;
    }
    return false;
}

bool nthMatches(int a, int b, int index) {
    if (a == 0) return index == b;
    int diff = index - b;
    return diff % a == 0 && diff / a >= 0;
}

// `el`'s 1-based position among its parent's element children, counted
// from the front or back, optionally counting only same-tag siblings. An
// element with no parent (the root) counts as the only child.
int siblingIndex(const Element* el, bool fromEnd, bool sameType) {
    if (!el->parent) return 1;
    const auto& kids = el->parent->children;
    int index = 0;
    auto visit = [&](const std::shared_ptr<Node>& n) {
        if (n->type != Node::ELEMENT) return false;
        auto* s = static_cast<const Element*>(n.get());
        if (sameType && s->tag != el->tag) return false;
        index++;
        return s == el;
    };
    if (fromEnd) { for (auto it = kids.rbegin(); it != kids.rend(); ++it) if (visit(*it)) break; }
    else { for (const auto& n : kids) if (visit(n)) break; }
    return index;
}

bool pseudoMatches(const PseudoClass& pc, const Element* el, const HoverSet* hover) {
    switch (pc.kind) {
    case PseudoClass::Hover:         return hover && hover->count(el) > 0;
    case PseudoClass::FirstChild:    return siblingIndex(el, false, false) == 1;
    case PseudoClass::LastChild:     return siblingIndex(el, true, false) == 1;
    case PseudoClass::OnlyChild:     return siblingIndex(el, false, false) == 1 && siblingIndex(el, true, false) == 1;
    case PseudoClass::NthChild:      return nthMatches(pc.a, pc.b, siblingIndex(el, false, false));
    case PseudoClass::NthLastChild:  return nthMatches(pc.a, pc.b, siblingIndex(el, true, false));
    case PseudoClass::FirstOfType:   return siblingIndex(el, false, true) == 1;
    case PseudoClass::LastOfType:    return siblingIndex(el, true, true) == 1;
    case PseudoClass::OnlyOfType:    return siblingIndex(el, false, true) == 1 && siblingIndex(el, true, true) == 1;
    case PseudoClass::NthOfType:     return nthMatches(pc.a, pc.b, siblingIndex(el, false, true));
    case PseudoClass::NthLastOfType: return nthMatches(pc.a, pc.b, siblingIndex(el, true, true));
    case PseudoClass::Root:          return el->parent == nullptr;
    case PseudoClass::Empty:
        for (const auto& n : el->children) {
            if (n->type == Node::ELEMENT) return false;
            if (!static_cast<const TextNode*>(n.get())->text.empty()) return false;
        }
        return true;
    case PseudoClass::AnyLink:
        return (el->tag == L"a" || el->tag == L"area") && el->attrs.count(L"href") > 0;
    case PseudoClass::Never:         return false;
    }
    return false;
}

bool compoundMatches(const CompoundSelector& cs, const Element* el, CSS::ClassCache* cache,
                     const HoverSet* hover) {
    if (!el) return false;
    if (!cs.tag.empty() && el->tag != cs.tag) return false;

    if (!cs.id.empty()) {
        auto it = el->attrs.find(L"id");
        if (it == el->attrs.end() || it->second != cs.id) return false;
    }

    if (!cs.classes.empty()) {
        const std::vector<std::wstring>& have = classesOf(el, cache);
        for (const auto& want : cs.classes) {
            bool found = false;
            for (const auto& h : have) if (h == want) { found = true; break; }
            if (!found) return false;
        }
    }

    for (const auto& as : cs.attrs) if (!attrMatches(as, el)) return false;
    for (const auto& pc : cs.pseudos) if (!pseudoMatches(pc, el, hover)) return false;
    for (const auto& n : cs.nots) if (compoundMatches(n, el, cache, hover)) return false;
    return true;
}

// The element siblings before `el` under its parent, nearest first.
std::vector<Element*> previousSiblings(const Element* el) {
    std::vector<Element*> out;
    if (!el->parent) return out;
    for (const auto& n : el->parent->children) {
        if (n.get() == el) break;
        if (n->type == Node::ELEMENT) out.push_back(static_cast<Element*>(n.get()));
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// Whether chain[0..idx] matches with chain[idx] landing on `el`, whose
// ancestors are ancestors[0..ancCount). Tries every candidate for each
// combinator (backtracking), so "a > b c" finds a match even when the
// nearest `c`-containing `b` isn't the one whose parent is an `a`.
bool matchFrom(const std::vector<CompoundSelector>& chain, size_t idx, Element* el,
               const std::vector<Element*>& ancestors, size_t ancCount,
               CSS::ClassCache* cache, const HoverSet* hover) {
    if (!compoundMatches(chain[idx], el, cache, hover)) return false;
    if (idx == 0) return true;

    switch (chain[idx].combinator) {
    case Combinator::Child:
        return ancCount > 0 && matchFrom(chain, idx - 1, ancestors[ancCount - 1], ancestors, ancCount - 1, cache, hover);
    case Combinator::Descendant: {
        // If everything further left is also plain descendant combinators,
        // the nearest matching ancestor is as good as any farther one (it
        // has strictly more ancestors above it), so there's no need to
        // backtrack - this keeps the common "a b c" case as cheap as the
        // original greedy matcher on large pages.
        bool greedy = true;
        for (size_t j = 1; j < idx; j++) if (chain[j].combinator != Combinator::Descendant) greedy = false;
        for (size_t k = ancCount; k > 0; k--) {
            if (!compoundMatches(chain[idx - 1], ancestors[k - 1], cache, hover)) continue;
            if (matchFrom(chain, idx - 1, ancestors[k - 1], ancestors, k - 1, cache, hover)) return true;
            if (greedy) return false;
        }
        return false;
    }
    case Combinator::NextSibling: {
        auto sibs = previousSiblings(el);
        return !sibs.empty() && matchFrom(chain, idx - 1, sibs.front(), ancestors, ancCount, cache, hover);
    }
    case Combinator::SubsequentSibling:
        for (Element* s : previousSiblings(el))
            if (matchFrom(chain, idx - 1, s, ancestors, ancCount, cache, hover)) return true;
        return false;
    }
    return false;
}

} // namespace

// Parses one selector (already split out of a comma-separated group, when
// called from parseStylesheet) into a chain of compounds joined by
// combinators. False if any part of it is unsupported. The helpers it uses
// are in the anonymous namespace above, which only hides them from other
// translation units.
bool parseSelector(const std::wstring& selector, std::vector<CompoundSelector>& chain) {
    size_t i = 0;
    Combinator pending = Combinator::Descendant;
    bool haveCombinator = false; // an explicit >, + or ~ is waiting for its right-hand compound
    while (i < selector.size()) {
        while (i < selector.size() && iswspace(selector[i])) i++;
        if (i >= selector.size()) break;

        wchar_t c = selector[i];
        if (c == L'>' || c == L'+' || c == L'~') {
            if (chain.empty() || haveCombinator) return false; // leading or doubled combinator
            pending = c == L'>' ? Combinator::Child : c == L'+' ? Combinator::NextSibling
                                                                : Combinator::SubsequentSibling;
            haveCombinator = true;
            i++;
            continue;
        }
        if (!haveCombinator) pending = Combinator::Descendant;

        // The compound runs until whitespace or a combinator outside any
        // [...]/(...) - "a[title='x y']" and ":not(.a .b)" stay one token.
        size_t start = i;
        while (i < selector.size()) {
            wchar_t d = selector[i];
            if (d == L'\\') { i += 2; continue; }
            if (d == L'[' || d == L'(') {
                size_t close = matchBracket(selector, i);
                if (close == std::wstring::npos) return false;
                i = close + 1;
                continue;
            }
            if (iswspace(d) || d == L'>' || d == L'+' || d == L'~') break;
            i++;
        }
        CompoundSelector cs;
        if (!parseCompound(selector.substr(start, std::min(i, selector.size()) - start), cs)) return false;
        cs.combinator = pending;
        chain.push_back(std::move(cs));
        haveCombinator = false;
    }
    return !chain.empty() && !haveCombinator;
}

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

    // Width-conditioned @media blocks currently being descended into -
    // unlike a bare-type block (decided once, here, at parse time), a
    // width condition can't be decided until layout knows the viewport,
    // so every rule found while this is non-empty gets tagged with the
    // innermost entry's bounds instead (see parseMediaCondition's
    // comment). `endPos` is where that block's own closing '}' is
    // (from matchBrace, already computed to know where to descend to);
    // popped once the parse cursor reaches it, right before that brace is
    // consumed by the ordinary "stray closing brace" handling below -
    // works the same whether the block is directly nested or reached
    // through an intervening bare-type @media (which never touches this
    // stack itself, so the ANCESTOR width constraint still applies).
    struct MediaScope { size_t endPos; int minWidthPx, maxWidthPx; };
    std::vector<MediaScope> mediaStack;

    while (i < css.size()) {
        while (i < css.size() && iswspace(css[i])) i++;
        if (i >= css.size()) break;

        while (!mediaStack.empty() && i >= mediaStack.back().endPos) mediaStack.pop_back();

        if (css[i] == L'@') {
            // A bare-type @media (e.g. "@media screen{...}") that
            // evaluates true is descended into rather than skipped: its
            // content is parsed by this same loop as if it weren't
            // wrapped at all. Landing on the block's own closing '}' once
            // that content is exhausted is handled by the plain "stray
            // closing brace" branch just below - no explicit tracking of
            // where the block ends is needed (unlike the width-conditioned
            // case just above, this one is fully decided right here).
            if (css.compare(i + 1, 5, L"media") == 0) {
                size_t braceIdx = css.find_first_of(L"{;", i);
                if (braceIdx != std::wstring::npos && css[braceIdx] == L'{') {
                    std::wstring condition = css.substr(i + 6, braceIdx - (i + 6));
                    if (isSimpleScreenMedia(condition)) {
                        i = braceIdx + 1;
                        continue;
                    }
                    MediaCondition mc = parseMediaCondition(condition);
                    if (mc.supported) {
                        size_t blockEnd = matchBrace(css, braceIdx);
                        if (blockEnd != std::wstring::npos) {
                            int minW = mc.minWidthPx, maxW = mc.maxWidthPx;
                            if (!mediaStack.empty()) { // AND with any already-active constraint
                                int outerMin = mediaStack.back().minWidthPx;
                                int outerMax = mediaStack.back().maxWidthPx;
                                if (outerMin > minW) minW = outerMin;
                                if (outerMax >= 0 && (maxW < 0 || outerMax < maxW)) maxW = outerMax;
                            }
                            mediaStack.push_back({ blockEnd, minW, maxW });
                            i = braceIdx + 1;
                            continue;
                        }
                    }
                }
            }
            i = skipAtRule(css, i);
            continue;
        }
        if (css[i] == L'}') { i++; continue; } // stray closing brace; keep going

        size_t brace = css.find(L'{', i);
        if (brace == std::wstring::npos) break; // trailing garbage; nothing more to parse
        std::wstring selectorText = css.substr(i, brace - i);

        size_t close = matchBrace(css, brace);
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
            if (!mediaStack.empty()) {
                rule.mediaMinWidth = mediaStack.back().minWidthPx;
                rule.mediaMaxWidth = mediaStack.back().maxWidthPx;
            }
            rules.push_back(std::move(rule));
        }
        order++;
    }
    return rules;
}

bool matches(const Rule& rule, const std::vector<Element*>& ancestors, Element* el, ClassCache* cache,
             const HoverSet* hover) {
    if (rule.chain.empty() || !el) return false;
    return matchFrom(rule.chain, rule.chain.size() - 1, el, ancestors, ancestors.size(), cache, hover);
}

namespace {
bool compoundUsesHover(const CompoundSelector& cs) {
    for (const auto& pc : cs.pseudos) if (pc.kind == PseudoClass::Hover) return true;
    return false;
}
} // namespace

bool hasHoverRules(const std::vector<Rule>& rules) {
    for (const auto& rule : rules)
        for (const auto& cs : rule.chain)
            if (compoundUsesHover(cs)) return true;
    return false;
}

bool hoverCouldAffect(const std::vector<Rule>& rules, const std::vector<const Element*>& changed) {
    if (changed.empty()) return false;
    for (const auto& rule : rules) {
        for (const auto& cs : rule.chain) {
            if (!compoundUsesHover(cs)) continue;
            for (const Element* el : changed) {
                HoverSet assumeHovered{ el };
                if (compoundMatches(cs, el, nullptr, &assumeHovered)) return true;
            }
        }
    }
    return false;
}

} // namespace CSS
