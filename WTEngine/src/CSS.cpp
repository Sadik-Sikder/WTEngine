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

Specificity specificityOf(const std::vector<CompoundSelector>& chain) {
    Specificity sp;
    for (const auto& cs : chain) {
        if (!cs.id.empty()) sp.ids++;
        sp.classes += (int)cs.classes.size();
        if (!cs.tag.empty()) sp.tags++;
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

bool compoundMatches(const CompoundSelector& cs, Element* el, CSS::ClassCache* cache) {
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
    return true;
}

} // namespace

// Parses one selector (already split out of a comma-separated group, when
// called from parseStylesheet) into a descendant chain. False if any part
// of it is unsupported. isSelectorChar/parseCompound are anonymous-
// namespace helpers above, but that only restricts them from other
// translation units - they're still visible here, in the rest of this file.
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

bool matches(const Rule& rule, const std::vector<Element*>& ancestors, Element* el, ClassCache* cache) {
    if (rule.chain.empty() || !compoundMatches(rule.chain.back(), el, cache)) return false;

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
            if (compoundMatches(rule.chain[compoundIdx], ancestors[ancestorIdx], cache)) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

} // namespace CSS
