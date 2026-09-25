#define NOMINMAX
#include "Layout.h"
#include "Renderer.h" // tryParseColor
#include <windows.h>
#include <string>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <set>
#include <utility>

std::wstring LayoutRoot::getAttr(Element* el, const std::wstring& key, const std::wstring& def) {
    if (!el) return def;
    auto it = el->attrs.find(key);
    if (it == el->attrs.end()) return def;
    return it->second;
}

// Parses a plain pixel length, e.g. margin/padding values or "6" / "20px".
// A unit this doesn't understand (a decimal, "1.5em", "50%", "2pt", ...)
// falls back to `def` rather than silently truncating to a wrong number -
// this used to read "1.5em" as the digits-only prefix "1" and return 1.
int LayoutRoot::parseFontSize(const std::wstring& s, int def) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    std::wstring v = s.substr(a, b - a);
    if (v.empty()) return def;

    size_t i = 0;
    while (i < v.size() && v[i] >= L'0' && v[i] <= L'9') i++;
    if (i == 0) return def; // doesn't start with a digit at all

    std::wstring unit = v.substr(i);
    if (!unit.empty() && unit != L"px") return def; // e.g. em/%/pt - not a plain pixel count

    try { return std::stoi(v.substr(0, i)); }
    catch (...) { return def; }
}

static std::wstring trimmed(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

// Parses a leading numeric value and trailing unit out of an already-trimmed
// CSS length like "1.5em" or "50%" (value=1.5, unit=L"em"). False if it
// doesn't start with a digit (or '.') at all. Shared by resolveFontSize and
// resolveLength, which each map `unit` to a pixel value against their own base.
static bool parseNumberAndUnit(const std::wstring& v, double& value, std::wstring& unit) {
    size_t i = 0;
    bool sawDigit = false;
    while (i < v.size() && ((v[i] >= L'0' && v[i] <= L'9') || v[i] == L'.')) {
        if (v[i] != L'.') sawDigit = true;
        i++;
    }
    if (!sawDigit) return false;
    try { value = std::stod(v.substr(0, i)); }
    catch (...) { return false; }
    unit = v.substr(i);
    return true;
}

// font-size specifically also supports units relative to `baseFontSize` (the
// inherited size): em/rem multiply it, % scales it - e.g. real pages commonly
// write `h1 { font-size: 1.5em }`. Anything else unsupported (pt, vw, ...)
// falls back to `def`.
int LayoutRoot::resolveFontSize(const std::wstring& s, int baseFontSize, int def) {
    std::wstring v = trimmed(s);
    if (v.empty()) return def;

    double value; std::wstring unit;
    if (!parseNumberAndUnit(v, value, unit)) return def;

    if (unit.empty() || unit == L"px") return (int)std::lround(value);
    if (unit == L"em" || unit == L"rem") return (int)std::lround(value * baseFontSize);
    if (unit == L"%") return (int)std::lround(value * baseFontSize / 100.0);
    return def;
}

// See the declaration in Layout.h for what `base` means here.
int LayoutRoot::resolveLength(const std::wstring& s, int base, int def) {
    std::wstring v = trimmed(s);
    if (v.empty()) return def;

    double value; std::wstring unit;
    if (!parseNumberAndUnit(v, value, unit)) return def;

    if (unit.empty() || unit == L"px") return (int)std::lround(value);
    if (unit == L"%") return (int)std::lround(value * base / 100.0);
    return def;
}

// See the declaration in Layout.h for the 1/2/3/4-value expansion rule.
void LayoutRoot::parseBoxShorthand(const std::wstring& v, int containingWidth, int def,
                                    int& top, int& right, int& bottom, int& left) {
    std::vector<std::wstring> tokens;
    std::wistringstream ss(v);
    std::wstring tok;
    while (ss >> tok) tokens.push_back(tok);

    top = right = bottom = left = def;
    auto len = [&](const std::wstring& t) { return resolveLength(t, containingWidth, def); };
    if (tokens.size() == 1) {
        top = right = bottom = left = len(tokens[0]);
    } else if (tokens.size() == 2) {
        top = bottom = len(tokens[0]);
        right = left = len(tokens[1]);
    } else if (tokens.size() == 3) {
        top = len(tokens[0]);
        right = left = len(tokens[1]);
        bottom = len(tokens[2]);
    } else if (tokens.size() >= 4) {
        top = len(tokens[0]);
        right = len(tokens[1]);
        bottom = len(tokens[2]);
        left = len(tokens[3]);
    }
}

float LayoutRoot::textWidth(const std::wstring& text, int fontSize, bool bold) {
    if (measureText) return measureText(text, fontSize, bold);
    return text.size() * fontSize * 0.55f; // rough fallback
}

// Tags treated as inline-level by default: their text/content flows onto
// the same line as their surrounding siblings instead of starting a block
// of its own. Everything else defaults to block, matching the engine's
// original (pre-inline) universal-block behavior.
static bool isInlineTag(const std::wstring& tag) {
    static const std::wstring kInline[] = {
        L"a", L"span", L"b", L"strong", L"i", L"em", L"u", L"small", L"code",
        L"sub", L"sup", L"mark", L"label", L"abbr", L"cite", L"q"
    };
    for (const auto& t : kInline) if (tag == t) return true;
    return false;
}

// Splits `text` on whitespace and appends one InlineItem per word, sharing
// `fontSize`/`href`/`owner` - the same tokenization layoutInlineRun's
// predecessor (layoutText) used to wrap a single string.
void LayoutRoot::appendWords(const std::wstring& text, int fontSize, const std::wstring& href,
                              Element* owner, std::vector<InlineItem>& out, bool visuallyHidden,
                              const TextPaint& paint) {
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && iswspace(text[i])) i++;
        size_t start = i;
        while (i < text.size() && !iswspace(text[i])) i++;
        if (start == i) break;
        out.push_back({ text.substr(start, i - start), fontSize, href, owner, false, visuallyHidden, paint });
    }
}

// Flattens `el`'s inline-level content (text nodes and nested inline
// elements like <a>/<b>) into a single flat item list, so a run of mixed
// text and inline markup - e.g. "Hello <a>link</a> world" - line-breaks as
// one paragraph instead of three separate blocks. A <br> becomes a forced
// break; a block-level element found here (invalid-ish nesting, e.g.
// <a><div>) is flattened into the run rather than specially promoted, since
// that combination is rare and not worth the extra complexity.
void LayoutRoot::collectInline(Element* el, int inheritedFontSize, int containingWidth, std::vector<InlineItem>& out,
                                bool inheritedVisuallyHidden, const TextPaint& inheritedPaint) {
    ancestorStack.push_back(el);

    for (auto& child : el->children) {
        if (child->type == Node::TEXT) {
            auto tnode = static_cast<TextNode*>(child.get());
            appendWords(tnode->text, inheritedFontSize, currentHref, el, out, inheritedVisuallyHidden, inheritedPaint);
            continue;
        }

        auto e = static_cast<Element*>(child.get());
        if (e->tag == L"head" || e->tag == L"script" || e->tag == L"style" ||
            e->tag == L"title" || e->tag == L"meta" || e->tag == L"link" ||
            e->tag == L"base")
            continue;

        if (e->tag == L"br") {
            out.push_back({ L"", inheritedFontSize, L"", nullptr, true, inheritedVisuallyHidden, inheritedPaint });
            continue;
        }

        ComputedStyle sv = computeStyle(e, inheritedFontSize, containingWidth, inheritedVisuallyHidden, inheritedPaint);
        if (sv.display == Display::None) continue;

        std::wstring savedHref = currentHref;
        if (e->tag == L"a") {
            auto href = e->attrs.find(L"href");
            if (href != e->attrs.end()) currentHref = href->second;
        }
        collectInline(e, sv.fontSize, containingWidth, out, sv.visuallyHidden, sv.paint);
        currentHref = savedHref;
    }

    ancestorStack.pop_back();
}

// Word-wraps a flattened inline run (see collectInline) to `containingWidth`,
// emitting one LayoutBox per item per line - not one box per line - since
// items on the same line can differ in font size, href, or owning element
// (e.g. a link in the middle of a sentence). Generalizes what layoutText
// used to do for a single homogeneously-styled string.
void LayoutRoot::layoutInlineRun(const std::vector<InlineItem>& items, int x, int& y, int containingWidth) {
    const int textInset = 4; // Engine::render draws text 4px inside its box
    const int paraGap = 6;
    const float maxWidth = (float)std::max(containingWidth - 2 * textInset, 40);

    struct Placed { InlineItem item; float offset; };
    std::vector<Placed> line;
    float lineWidth = 0;

    auto emitLine = [&]() {
        if (line.empty()) return;
        int lineHeight = 0;
        for (auto& p : line) lineHeight = std::max(lineHeight, p.item.fontSize);
        lineHeight += 8;
        for (auto& p : line) {
            LayoutBox box;
            box.x = x + (int)std::lround(p.offset);
            box.y = y;
            box.width = (int)std::lround(textWidth(p.item.word, p.item.fontSize, p.item.paint.bold));
            box.height = lineHeight;
            box.text = p.item.word;
            box.href = p.item.href;
            box.fontSize = p.item.fontSize;
            box.color = p.item.paint.color;
            box.bold = p.item.paint.bold;
            box.el = p.item.owner;
            box.visuallyHidden = p.item.visuallyHidden;
            boxes.push_back(box);
        }
        y += lineHeight;
        line.clear();
        lineWidth = 0;
    };

    for (const auto& raw : items) {
        if (raw.isBreak) { emitLine(); continue; }
        if (raw.word.empty()) continue;

        InlineItem item = raw;
        float wordWidth = textWidth(item.word, item.fontSize, item.paint.bold);

        // A word wider than the line on its own gets broken by characters,
        // one fragment per line, before whatever's left of it (now short
        // enough) falls through to the normal wrapping below.
        while (wordWidth > maxWidth && item.word.size() > 1) {
            if (!line.empty()) emitLine();
            size_t n = 1;
            while (n < item.word.size() && textWidth(item.word.substr(0, n + 1), item.fontSize, item.paint.bold) <= maxWidth) n++;
            InlineItem fragment = item;
            fragment.word = item.word.substr(0, n);
            line.push_back({ fragment, 0 });
            emitLine();
            item.word.erase(0, n);
            wordWidth = textWidth(item.word, item.fontSize, item.paint.bold);
        }
        if (item.word.empty()) continue;

        float spaceWidth = line.empty() ? 0 : textWidth(L" ", item.fontSize, item.paint.bold);
        if (!line.empty() && lineWidth + spaceWidth + wordWidth > maxWidth) emitLine();

        float offset = line.empty() ? 0 : lineWidth + spaceWidth;
        lineWidth = offset + wordWidth;
        line.push_back({ item, offset });
    }
    emitLine();

    y += paraGap;
}

static std::wstring lowerCase(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// Splits a shorthand value into whitespace-separated tokens, keeping
// anything inside parentheses whole - "1px solid rgb(0, 0, 0)" gives
// {"1px", "solid", "rgb(0, 0, 0)"}.
static std::vector<std::wstring> cssTokens(const std::wstring& v) {
    std::vector<std::wstring> out;
    std::wstring token;
    int depth = 0;
    for (size_t i = 0; i <= v.size(); i++) {
        wchar_t c = i < v.size() ? v[i] : L' ';
        if (c == L'(') depth++;
        else if (c == L')' && depth > 0) depth--;
        if (iswspace(c) && depth == 0) {
            if (!token.empty()) out.push_back(token);
            token.clear();
        } else {
            token += c;
        }
    }
    return out;
}

// First piece of text inside an element (used for <button> labels).
static std::wstring firstText(Element* el) {
    for (auto& child : el->children) {
        if (child->type == Node::TEXT) return static_cast<TextNode*>(child.get())->text;
        std::wstring t = firstText(static_cast<Element*>(child.get()));
        if (!t.empty()) return t;
    }
    return L"";
}

// Computes the cascaded style properties layout uses, for both plain
// elements and form controls: stylesheet rules first (least to most
// specific, source order breaking ties), then inline style="" - which, per
// CSS, always wins regardless of specificity.
LayoutRoot::ComputedStyle LayoutRoot::computeStyle(Element* e, int inheritedFontSize, int containingWidth,
                                                     bool inheritedVisuallyHidden, const TextPaint& inheritedPaint) {
    ComputedStyle sv;
    sv.fontSize = inheritedFontSize; // inherited unless a rule below overrides it
    sv.display = isInlineTag(e->tag) ? Display::Inline : Display::Block;

    // color/font-weight: inherited, then the tag's own default (what a
    // browser's built-in stylesheet would give it), then author rules.
    sv.paint = inheritedPaint;
    if (e->tag == L"a" && e->attrs.count(L"href")) sv.paint.color = L"#0033cc";
    if (e->tag == L"b" || e->tag == L"strong" || e->tag == L"th" ||
        (e->tag.size() == 2 && e->tag[0] == L'h' && e->tag[1] >= L'1' && e->tag[1] <= L'6'))
        sv.paint.bold = true;

    auto applyDecl = [&](const std::wstring& k, const std::wstring& v) {
        if (k == L"background-color") {
            Color unused;
            if (tryParseColor(v, unused)) sv.background = v;
        }
        else if (k == L"background") {
            // The shorthand can carry an image, position, repeat, etc.
            // alongside the color ("#fff url(x.png) no-repeat") - keep just
            // the first token that's a color. "background: none" (or only
            // an image) clears any earlier color.
            Color unused;
            sv.background.clear();
            for (const auto& tok : cssTokens(v)) {
                if (tryParseColor(tok, unused)) { sv.background = tok; break; }
            }
        }
        else if (k == L"color") {
            // inherit/currentcolor keep the inherited value; anything that
            // isn't a valid color is ignored, same as a real browser.
            Color unused;
            if (v == L"initial" || v == L"unset") sv.paint.color.clear();
            else if (tryParseColor(v, unused)) sv.paint.color = v;
        }
        else if (k == L"font-weight") {
            if (v == L"bold" || v == L"bolder") sv.paint.bold = true;
            else if (v == L"normal" || v == L"lighter" || v == L"initial" || v == L"unset") sv.paint.bold = false;
            else {
                // Numeric weight: 600 and up is what GDI (and browsers,
                // with only a regular/bold pair of faces) render as bold.
                try { sv.paint.bold = std::stoi(v) >= 600; } catch (...) {}
            }
        }
        else if (k == L"margin") {
            parseBoxShorthand(v, containingWidth, 6, sv.marginTop, sv.marginRight, sv.marginBottom, sv.marginLeft);
        }
        else if (k == L"margin-top") sv.marginTop = resolveLength(v, containingWidth, 6);
        else if (k == L"margin-right") sv.marginRight = resolveLength(v, containingWidth, 0);
        else if (k == L"margin-bottom") sv.marginBottom = resolveLength(v, containingWidth, 6);
        else if (k == L"margin-left") sv.marginLeft = resolveLength(v, containingWidth, 0);
        else if (k == L"padding") {
            parseBoxShorthand(v, containingWidth, 6, sv.paddingTop, sv.paddingRight, sv.paddingBottom, sv.paddingLeft);
        }
        else if (k == L"padding-top") sv.paddingTop = resolveLength(v, containingWidth, 6);
        else if (k == L"padding-right") sv.paddingRight = resolveLength(v, containingWidth, 6);
        else if (k == L"padding-bottom") sv.paddingBottom = resolveLength(v, containingWidth, 6);
        else if (k == L"padding-left") sv.paddingLeft = resolveLength(v, containingWidth, 6);
        else if (k == L"width") sv.width = resolveLength(v, containingWidth, -1);
        else if (k == L"height") sv.height = resolveLength(v, containingWidth, -1);
        else if (k == L"box-sizing") {
            if (v == L"border-box") sv.boxSizing = BoxSizing::BorderBox;
            else if (v == L"content-box") sv.boxSizing = BoxSizing::ContentBox;
        }
        else if (k == L"border-color") sv.borderColor = v;
        else if (k == L"border-width") sv.borderWidth = resolveLength(v, containingWidth, 0);
        else if (k == L"border") {
            // Shorthand, e.g. "1px solid #333": scan whitespace-separated
            // tokens for a length and a color (any CSS color). The style keyword
            // (solid/dashed/...) is accepted but has nothing to key off of -
            // every border is drawn the same way, a solid-colored frame.
            Color unused;
            for (const auto& tok : cssTokens(v)) {
                if (tryParseColor(tok, unused)) sv.borderColor = tok;
                else if (tok == L"none" || tok == L"hidden") sv.borderWidth = 0;
                else {
                    int len = resolveLength(tok, containingWidth, -1);
                    if (len >= 0) sv.borderWidth = len;
                }
            }
        }
        else if (k == L"grid-template-columns") sv.gridTemplateColumns = parseGridTemplateTracks(v, containingWidth);
        else if (k == L"grid-template-rows") sv.gridTemplateRows = parseGridTemplateTracks(v, containingWidth);
        else if (k == L"grid-template-areas") sv.gridTemplateAreas = parseGridTemplateAreas(v);
        else if (k == L"grid-template") {
            std::vector<std::vector<std::wstring>> areas;
            std::vector<GridTrack> rowTracks, colTracks;
            parseGridTemplateShorthand(v, containingWidth, areas, rowTracks, colTracks);
            if (!areas.empty()) sv.gridTemplateAreas = std::move(areas);
            if (!rowTracks.empty()) sv.gridTemplateRows = std::move(rowTracks);
            if (!colTracks.empty()) sv.gridTemplateColumns = std::move(colTracks);
        }
        else if (k == L"grid-area") sv.gridArea = trimmed(v); // on an item: the area name to place into - see its ComputedStyle comment
        else if (k == L"grid-column") {
            int s, e;
            if (parseGridLinePlacement(v, s, e)) { sv.gridColumnStart = s; sv.gridColumnEnd = e; }
        }
        else if (k == L"grid-column-start") { try { sv.gridColumnStart = std::stoi(v); } catch (...) {} }
        else if (k == L"grid-column-end") { try { sv.gridColumnEnd = std::stoi(v); } catch (...) {} }
        else if (k == L"grid-row") {
            int s, e;
            if (parseGridLinePlacement(v, s, e)) { sv.gridRowStart = s; sv.gridRowEnd = e; }
        }
        else if (k == L"grid-row-start") { try { sv.gridRowStart = std::stoi(v); } catch (...) {} }
        else if (k == L"grid-row-end") { try { sv.gridRowEnd = std::stoi(v); } catch (...) {} }
        else if (k == L"row-gap") sv.rowGap = resolveLength(v, containingWidth, 0);
        else if (k == L"column-gap") sv.columnGap = resolveLength(v, containingWidth, 0);
        else if (k == L"gap" || k == L"grid-gap") {
            // "gap: <row>" sets both; "gap: <row> <column>" sets them
            // separately, in that order - same order real CSS uses.
            std::wistringstream ss(v);
            std::wstring t1, t2;
            ss >> t1;
            int g1 = resolveLength(t1, containingWidth, 0);
            if (ss >> t2) { sv.rowGap = g1; sv.columnGap = resolveLength(t2, containingWidth, 0); }
            else { sv.rowGap = sv.columnGap = g1; }
        }
        else if (k == L"font-size") sv.fontSize = resolveFontSize(v, inheritedFontSize, sv.fontSize);
        else if (k == L"opacity") { try { sv.opacity = std::stof(v); } catch (...) {} }
        else if (k == L"visibility") sv.visibilityHidden = (v == L"hidden" || v == L"collapse");
        else if (k == L"display") {
            if (v == L"none") sv.display = Display::None;
            else if (v == L"inline" || v == L"inline-block") sv.display = Display::Inline;
            else if (v == L"block") sv.display = Display::Block;
            else if (v == L"grid") sv.display = Display::Grid;
            else if (v == L"flex") sv.display = Display::Flex;
        }
        else if (k == L"flex-direction") {
            // The -reverse variants are laid out unreversed; anything
            // unrecognized falls back to row, the real default.
            sv.flexDirection = (v == L"column" || v == L"column-reverse") ? FlexDirection::Column : FlexDirection::Row;
        }
        else if (k == L"justify-content") {
            if (v == L"center") sv.justifyContent = JustifyContent::Center;
            else if (v == L"flex-end") sv.justifyContent = JustifyContent::FlexEnd;
            else if (v == L"space-between") sv.justifyContent = JustifyContent::SpaceBetween;
            else if (v == L"space-around") sv.justifyContent = JustifyContent::SpaceAround;
            else sv.justifyContent = JustifyContent::FlexStart;
        }
        else if (k == L"align-items") {
            if (v == L"flex-start") sv.alignItems = AlignItems::FlexStart;
            else if (v == L"center") sv.alignItems = AlignItems::Center;
            else if (v == L"flex-end") sv.alignItems = AlignItems::FlexEnd;
            else sv.alignItems = AlignItems::Stretch;
        }
        else if (k == L"flex-grow") {
            try { sv.flexGrow = std::stof(v); sv.flexGrowSet = true; } catch (...) {}
        }
        else if (k == L"flex-shrink") {
            try { sv.flexShrink = std::max(std::stof(v), 0.0f); } catch (...) {}
        }
        else if (k == L"flex-basis") {
            sv.flexBasis = (v == L"auto" || v == L"content") ? -1 : resolveLength(v, containingWidth, -1);
        }
        else if (k == L"flex-wrap") {
            sv.flexWrap = v == L"wrap" ? FlexWrap::Wrap : v == L"wrap-reverse" ? FlexWrap::WrapReverse : FlexWrap::NoWrap;
        }
        else if (k == L"flex-flow") {
            // "<direction> <wrap>" in either order, either part optional.
            for (const auto& tok : cssTokens(v)) {
                if (tok == L"row" || tok == L"row-reverse") sv.flexDirection = FlexDirection::Row;
                else if (tok == L"column" || tok == L"column-reverse") sv.flexDirection = FlexDirection::Column;
                else if (tok == L"wrap") sv.flexWrap = FlexWrap::Wrap;
                else if (tok == L"wrap-reverse") sv.flexWrap = FlexWrap::WrapReverse;
                else if (tok == L"nowrap") sv.flexWrap = FlexWrap::NoWrap;
            }
        }
        else if (k == L"flex") {
            // The shorthand, per spec: none = 0 0 auto; auto = 1 1 auto;
            // otherwise up to two numbers (grow, then shrink) and a basis,
            // where a unitless number sets grow/shrink and anything else is
            // the basis. A basis left out becomes 0 - so `flex: 1` makes
            // items share the whole line equally, regardless of `width`.
            if (v == L"none") { sv.flexGrow = 0; sv.flexShrink = 0; sv.flexBasis = -1; sv.flexGrowSet = true; }
            else if (v == L"auto") { sv.flexGrow = 1; sv.flexShrink = 1; sv.flexBasis = -1; sv.flexGrowSet = true; }
            else if (v != L"initial") {
                int numbers = 0;
                bool basisSet = false;
                float grow = 1, shrink = 1;
                int basis = 0;
                for (const auto& tok : cssTokens(v)) {
                    size_t used = 0;
                    float num = 0;
                    bool isNumber = false;
                    try { num = std::stof(tok, &used); isNumber = used == tok.size(); } catch (...) {}
                    if (isNumber && numbers < 2) { (numbers++ == 0 ? grow : shrink) = std::max(num, 0.0f); }
                    else if (tok == L"auto" || tok == L"content") { basisSet = true; basis = -1; }
                    else { basisSet = true; basis = resolveLength(tok, containingWidth, 0); }
                }
                sv.flexGrow = grow;
                sv.flexGrowSet = true;
                sv.flexShrink = shrink;
                sv.flexBasis = basisSet ? basis : 0;
            }
        }
    };

    if (rules) {
        std::vector<const CSS::Rule*> matched;
        auto consider = [&](const std::vector<const CSS::Rule*>& bucket) {
            for (const CSS::Rule* rule : bucket) {
                // A width-conditioned @media's rule carries the viewport
                // bound it needs (CSS::Rule's comment); checked here, against
                // the live viewport, rather than once at parse time - so a
                // resize (a full relayout, hence a fresh computeStyle pass)
                // re-evaluates it for free, no separate reactivity needed.
                if (rule->mediaMinWidth >= 0 && viewportWidth < rule->mediaMinWidth) continue;
                if (rule->mediaMaxWidth >= 0 && viewportWidth > rule->mediaMaxWidth) continue;
                if (CSS::matches(*rule, ancestorStack, e, &classCache, hover)) matched.push_back(rule);
            }
        };
        auto lookup = [&](const std::unordered_map<std::wstring, std::vector<const CSS::Rule*>>& map,
                          const std::wstring& key) {
            auto it = map.find(key);
            if (it != map.end()) consider(it->second);
        };
        // Only the buckets this element could possibly satisfy (see RuleIndex).
        consider(ruleIndex.universal);
        lookup(ruleIndex.byTag, e->tag);
        auto id = e->attrs.find(L"id");
        if (id != e->attrs.end()) lookup(ruleIndex.byId, id->second);
        if (!ruleIndex.byClass.empty() && e->attrs.count(L"class")) {
            auto [it, inserted] = classCache.try_emplace(e);
            if (inserted) {
                std::wistringstream ss(e->attrs[L"class"]);
                for (std::wstring c; ss >> c;) it->second.push_back(c);
            }
            for (const auto& c : it->second) lookup(ruleIndex.byClass, c);
        }
        std::stable_sort(matched.begin(), matched.end(),
            [](const CSS::Rule* a, const CSS::Rule* b) {
                if (a->specificity < b->specificity) return true;
                if (b->specificity < a->specificity) return false;
                return a->order < b->order;
            });
        for (const auto* rule : matched)
            for (const auto& decl : rule->declarations) applyDecl(decl.first, decl.second);
    }
    for (const auto& decl : CSS::parseDeclarations(getAttr(e, L"style", L"")))
        applyDecl(decl.first, decl.second);

    sv.visuallyHidden = inheritedVisuallyHidden || sv.opacity <= 0.0f || sv.visibilityHidden;
    return sv;
}

// See the declaration in Layout.h for what's supported. repeat(N, <track>)
// is expanded textually first (e.g. "repeat(3, 1fr)" -> "1fr 1fr 1fr"),
// then the result is just a space-separated list of px/%/fr tokens.
std::vector<LayoutRoot::GridTrack> LayoutRoot::parseGridTemplateTracks(const std::wstring& v, int containingWidth) {
    std::wstring expanded;
    size_t i = 0;
    while (i < v.size()) {
        size_t rep = v.find(L"repeat(", i);
        if (rep == std::wstring::npos) { expanded += v.substr(i); break; }
        expanded += v.substr(i, rep - i);
        size_t close = v.find(L')', rep);
        if (close == std::wstring::npos) break; // unterminated repeat(; stop, keep what we have so far

        std::wstring args = v.substr(rep + 7, close - rep - 7); // between "repeat(" and ")"
        size_t comma = args.find(L',');
        int count = 1;
        std::wstring track = trimmed(args);
        if (comma != std::wstring::npos) {
            try { count = std::max(1, std::stoi(trimmed(args.substr(0, comma)))); } catch (...) {}
            track = trimmed(args.substr(comma + 1));
        }
        for (int n = 0; n < count; n++) { expanded += track; expanded += L' '; }
        i = close + 1;
    }

    std::vector<GridTrack> tracks;
    std::wistringstream ss(expanded);
    std::wstring tok;
    while (ss >> tok) {
        if (tok.size() > 2 && tok.compare(tok.size() - 2, 2, L"fr") == 0) {
            try { tracks.push_back({ true, (float)std::stod(tok.substr(0, tok.size() - 2)) }); }
            catch (...) { tracks.push_back({ true, 1.0f }); }
            continue;
        }
        int px = resolveLength(tok, containingWidth, -1);
        // A keyword this doesn't understand (auto, minmax(...), fit-content(...),
        // ...) becomes 1fr - see the Layout.h comment on why.
        tracks.push_back(px >= 0 ? GridTrack{ false, (float)px } : GridTrack{ true, 1.0f });
    }
    return tracks;
}

// See the declaration in Layout.h for the grammar/scope. `start`/`end` are
// 1-based CSS grid line numbers, matching how they're authored.
bool LayoutRoot::parseGridLinePlacement(const std::wstring& v, int& start, int& end) {
    std::wistringstream ss(v);
    std::wstring a, slash, b;
    if (!(ss >> a)) return false;
    int startVal;
    try { startVal = std::stoi(a); } catch (...) { return false; }
    if (startVal <= 0) return false; // line numbers are 1-based; 0/negative isn't supported

    if (!(ss >> slash)) { start = startVal; end = startVal + 1; return true; } // bare "N": span 1
    if (slash != L"/") return false;
    if (!(ss >> b)) return false;

    if (b == L"span") {
        std::wstring n;
        if (!(ss >> n)) return false;
        int spanVal;
        try { spanVal = std::stoi(n); } catch (...) { return false; }
        if (spanVal <= 0) return false;
        start = startVal; end = startVal + spanVal;
        return true;
    }
    int endVal;
    try { endVal = std::stoi(b); } catch (...) { return false; }
    if (endVal <= startVal) return false; // an empty/reversed range isn't supported
    start = startVal; end = endVal;
    return true;
}

// See the declaration in Layout.h for the grammar/scope.
std::vector<std::vector<std::wstring>> LayoutRoot::parseGridTemplateAreas(const std::wstring& v) {
    std::vector<std::vector<std::wstring>> rows;
    size_t i = 0;
    while (i < v.size()) {
        size_t open = v.find(L'"', i);
        if (open == std::wstring::npos) break;
        size_t close = v.find(L'"', open + 1);
        if (close == std::wstring::npos) return {}; // unterminated string - malformed, bail on the whole value

        std::wistringstream ss(v.substr(open + 1, close - open - 1));
        std::vector<std::wstring> row;
        std::wstring tok;
        while (ss >> tok) row.push_back(tok);
        if (!rows.empty() && row.size() != rows[0].size()) return {}; // every row must name the same number of columns
        rows.push_back(std::move(row));
        i = close + 1;
    }
    return rows;
}

// See the declaration in Layout.h for the grammar/scope. Splits at the
// top-level '/' first (an area string never contains one); everything
// before it is scanned for alternating quoted area-rows and an optional
// track-size token right after each one's closing quote.
void LayoutRoot::parseGridTemplateShorthand(const std::wstring& v, int containingWidth,
                                             std::vector<std::vector<std::wstring>>& areas,
                                             std::vector<GridTrack>& rowTracks, std::vector<GridTrack>& colTracks) {
    size_t slash = v.find(L'/');
    std::wstring rowsPart = slash == std::wstring::npos ? v : v.substr(0, slash);

    size_t i = 0;
    while (i < rowsPart.size()) {
        size_t open = rowsPart.find(L'"', i);
        if (open == std::wstring::npos) break;
        size_t close = rowsPart.find(L'"', open + 1);
        if (close == std::wstring::npos) { areas.clear(); rowTracks.clear(); return; } // unterminated - bail

        std::wistringstream ss(rowsPart.substr(open + 1, close - open - 1));
        std::vector<std::wstring> row;
        std::wstring tok;
        while (ss >> tok) row.push_back(tok);
        if (!areas.empty() && row.size() != areas[0].size()) { areas.clear(); rowTracks.clear(); return; }
        areas.push_back(std::move(row));

        // An optional track size sits between this row's closing quote and
        // the next row's opening one (or the end) - e.g. the 40px in
        // `"header header" 40px "sidebar main" 1fr`. isFr:true,value:0 - a
        // share of nothing - marks "no size given", distinct from an
        // actually-parsed 0px: layoutGrid already treats any isFr track as
        // unset/auto for rows (see gridTemplateRows's own comment), so
        // this rides that same rule for free instead of needing a new one.
        size_t nextOpen = rowsPart.find(L'"', close + 1);
        std::wstring between = trimmed(rowsPart.substr(close + 1, (nextOpen == std::wstring::npos ? rowsPart.size() : nextOpen) - close - 1));
        if (!between.empty()) {
            auto parsed = parseGridTemplateTracks(between, containingWidth);
            rowTracks.push_back(!parsed.empty() ? parsed[0] : GridTrack{ true, 0.0f });
        } else {
            rowTracks.push_back({ true, 0.0f });
        }
        i = close + 1;
    }

    if (slash != std::wstring::npos) colTracks = parseGridTemplateTracks(trimmed(v.substr(slash + 1)), containingWidth);
}

// Places one <input> or <button> as a box of its own. Controls are laid out
// as blocks like everything else here, so a label and its field end up on
// separate rows.
void LayoutRoot::layoutControl(Element* e, int x, int& y, int containingWidth, const ComputedStyle& style) {
    // Control heights scale with font-size so a bigger font doesn't clip.
    const int fieldHeight = std::max(28, style.fontSize + 14);
    const int buttonHeight = std::max(30, style.fontSize + 16);
    const int checkboxSize = std::max(18, style.fontSize + 4);
    const int maxWidth = std::max(containingWidth, 60);

    std::wstring type = lowerCase(getAttr(e, L"type", L""));

    LayoutBox box;
    box.x = x;
    box.fontSize = style.fontSize;
    box.background = style.background;
    box.el = e;
    box.form = currentForm;
    box.visuallyHidden = style.visuallyHidden;

    if (e->tag == L"button") {
        box.control = LayoutBox::Button;
        box.text = firstText(e);
        if (box.text.empty()) box.text = L"Button";
    }
    else if (e->tag == L"select") {
        box.control = LayoutBox::Select;
    }
    else if (type == L"submit" || type == L"button" || type == L"reset" || type == L"image") {
        box.control = LayoutBox::Button;
        box.text = getAttr(e, L"value", L"");
        if (box.text.empty()) box.text = type == L"reset" ? L"Reset" : type == L"button" ? L"" : L"Submit";
    }
    else if (type == L"checkbox") {
        box.control = LayoutBox::Checkbox;
    }
    else if (type == L"hidden" || type == L"radio" || type == L"file" ||
             type == L"range" || type == L"color") {
        return; // hidden takes no space; the rest aren't supported
    }
    else {
        box.control = LayoutBox::TextField; // text, search, email, password, url, ...
    }

    switch (box.control) {
    case LayoutBox::TextField: {
        int chars = parseFontSize(getAttr(e, L"size", L""), 20);
        box.width = std::min(std::max(chars * 8 + 16, 60), maxWidth);
        box.height = fieldHeight;
        break;
    }
    case LayoutBox::Button:
        box.width = std::min(std::max((int)textWidth(box.text, box.fontSize) + 24, 40), maxWidth);
        box.height = buttonHeight;
        break;
    case LayoutBox::Select:
        box.width = std::min(160, maxWidth);
        box.height = fieldHeight;
        break;
    default: // Checkbox
        box.width = checkboxSize;
        box.height = checkboxSize;
        break;
    }

    y += style.marginTop;
    box.y = y;
    boxes.push_back(box);
    y += box.height + style.marginBottom;
}

// Places an <img> as a box of its own. Explicit width/height attributes
// always win (per-axis - a mismatched aspect ratio between an explicit
// width and a natural height is the page's problem, same as real browsers);
// an unset axis uses the image's natural decoded size, fetching/decoding it
// now via `loadImage` so the box gets the real size instead of a guess. A
// fixed placeholder covers the case where that fetch/decode fails.
void LayoutRoot::layoutImage(Element* e, int x, int& y, int containingWidth, const ComputedStyle& style) {
    const int defaultWidth = 200, defaultHeight = 150;
    std::wstring src = getAttr(e, L"src", L"");

    int naturalW = 0, naturalH = 0;
    bool haveNatural = loadImage && !src.empty() && loadImage(src, naturalW, naturalH);

    std::wstring wAttr = getAttr(e, L"width", L"");
    std::wstring hAttr = getAttr(e, L"height", L"");
    int width = !wAttr.empty() ? parseFontSize(wAttr, defaultWidth) : (haveNatural ? naturalW : defaultWidth);
    int height = !hAttr.empty() ? parseFontSize(hAttr, defaultHeight) : (haveNatural ? naturalH : defaultHeight);

    LayoutBox box;
    box.x = x;
    box.fontSize = style.fontSize;
    box.background = style.background;
    box.imageSrc = src;
    box.width = std::min(width, std::max(containingWidth, 1));
    box.height = height;
    box.el = e;
    box.visuallyHidden = style.visuallyHidden;

    y += style.marginTop;
    box.y = y;
    boxes.push_back(box);
    y += box.height + style.marginBottom;
}

void LayoutRoot::rebuildRuleIndex() {
    ruleIndex = RuleIndex{};
    if (!rules) return;
    for (const auto& rule : *rules) {
        if (rule.chain.empty()) continue;
        const CSS::CompoundSelector& last = rule.chain.back();
        if (!last.id.empty()) ruleIndex.byId[last.id].push_back(&rule);
        else if (!last.classes.empty()) ruleIndex.byClass[last.classes.front()].push_back(&rule);
        else if (!last.tag.empty()) ruleIndex.byTag[last.tag].push_back(&rule);
        else ruleIndex.universal.push_back(&rule);
    }
}

void LayoutRoot::layout() {
    boxes.clear();
    ancestorStack.clear();
    classCache.clear(); // safe to reuse within this pass only - see its declaration in Layout.h
    rebuildRuleIndex();
    if (!rootNode) return;

    int y = 10;
    layoutElement(static_cast<Element*>(rootNode), 10, y, viewportWidth - 20, 14, false, TextPaint{});
}

void LayoutRoot::layoutElement(Element* el, int x, int& y, int containingWidth, int inheritedFontSize,
                                bool inheritedVisuallyHidden, const TextPaint& inheritedPaint) {
    if (!el) return;
    ancestorStack.push_back(el); // `el` is an ancestor of every child laid out below

    // Consecutive text/inline-level children (e.g. "Hello <a>link</a> world")
    // are buffered here so they word-wrap together as one flowing run,
    // instead of each one starting a block of its own. A block-level child,
    // or the end of `el`'s children, flushes whatever's pending so far.
    std::vector<InlineItem> pendingInline;
    auto flushInline = [&]() {
        if (!pendingInline.empty()) {
            layoutInlineRun(pendingInline, x, y, containingWidth);
            pendingInline.clear();
        }
    };

    // Loop over each child node
    for (auto& child : el->children) {

        // Case 1: Text node (simple paragraph text)
        if (child->type == Node::TEXT) {
            auto tnode = static_cast<TextNode*>(child.get());
            appendWords(tnode->text, inheritedFontSize, currentHref, el, pendingInline, inheritedVisuallyHidden, inheritedPaint);
        }

        //  Case 2: Element node (<div>, <p>, <span>, etc.)
        else if (child->type == Node::ELEMENT) {
            auto e = static_cast<Element*>(child.get());

            // Non-visual elements produce no boxes and take no space.
            // <noscript> is deliberately not in this list: unlike the others
            // its content is meant to be shown when JS isn't available. The
            // engine does run scripts now, but it doesn't distinguish the two
            // cases, so <noscript> is laid out like a normal container below.
            if (e->tag == L"head" || e->tag == L"script" || e->tag == L"style" ||
                e->tag == L"title" || e->tag == L"meta" || e->tag == L"link" ||
                e->tag == L"base")
                continue;

            if (e->tag == L"br") {
                pendingInline.push_back({ L"", inheritedFontSize, L"", nullptr, true, inheritedVisuallyHidden, inheritedPaint });
                continue;
            }

            ComputedStyle sv = computeStyle(e, inheritedFontSize, containingWidth, inheritedVisuallyHidden, inheritedPaint);
            if (sv.display == Display::None) continue; // this element and its subtree take no space

            if (e->tag == L"input" || e->tag == L"button" || e->tag == L"select") {
                flushInline();
                layoutControl(e, x, y, containingWidth, sv);
                continue;
            }

            if (e->tag == L"img") {
                flushInline();
                layoutImage(e, x, y, containingWidth, sv);
                continue;
            }

            if (sv.display == Display::Inline) {
                // Joins the current run rather than starting a block: its
                // text (and any nested inline content) flows onto the same
                // line as its surrounding siblings.
                std::wstring savedHref = currentHref;
                if (e->tag == L"a") {
                    auto href = e->attrs.find(L"href");
                    if (href != e->attrs.end()) currentHref = href->second;
                }
                collectInline(e, sv.fontSize, containingWidth, pendingInline, sv.visuallyHidden, sv.paint);
                currentHref = savedHref;
                continue;
            }

            // Block-level (or grid - blockified the same way a real grid
            // item is): close out any pending inline run first so it
            // renders above this block, in document order.
            flushInline();
            layoutBlockChild(e, x, y, containingWidth, sv);
        }
    }

    flushInline();
    ancestorStack.pop_back();
}

// See the declaration in Layout.h. This is exactly what layoutElement's own
// loop used to do inline for a block-level child; factored out so layoutGrid
// can give a grid item identical box-model treatment without duplicating it.
void LayoutRoot::layoutBlockChild(Element* e, int x, int& y, int containingWidth, const ComputedStyle& sv) {
    // Box model: turn the cascaded style into an outer width (the painted
    // border/background edge) and a content width (what's left for children
    // after padding and border). With no explicit `width` the box just
    // fills what its container offers, as always; with one set, box-sizing
    // decides which width it names - see the BoxSizing comment in Layout.h.
    int outerWidth;
    if (sv.width >= 0) {
        outerWidth = sv.boxSizing == BoxSizing::BorderBox
            ? sv.width
            : sv.width + sv.paddingLeft + sv.paddingRight + 2 * sv.borderWidth;
    } else {
        outerWidth = std::max(containingWidth - sv.marginLeft - sv.marginRight, 0);
    }
    int contentWidth = std::max(outerWidth - sv.paddingLeft - sv.paddingRight - 2 * sv.borderWidth, 0);
    int boxX = x + sv.marginLeft;

    y += sv.marginTop;
    int contentStartY = y;

    // Reserve a background/border box now (before laying out children) so
    // it paints behind them, but only if this element actually declared
    // one — plain structural wrappers like <html>/<body> get no box at
    // all, they just position their children.
    size_t bgIndex = static_cast<size_t>(-1);
    if (!sv.background.empty() || sv.borderWidth > 0) {
        LayoutBox box;
        box.x = boxX;
        box.y = contentStartY;
        box.width = outerWidth;
        box.height = 0; // filled in below once children are laid out
        box.background = sv.background;
        box.borderWidth = sv.borderWidth;
        box.borderColor = sv.borderColor;
        box.el = e;
        box.visuallyHidden = sv.visuallyHidden;
        bgIndex = boxes.size();
        boxes.push_back(box);
    }

    y += sv.borderWidth + sv.paddingTop;

    // Recurse into nested elements/text - or, for a grid/flex container,
    // into layoutGrid/layoutFlex instead of the usual vertical flow.
    std::wstring savedHref = currentHref;
    if (e->tag == L"a") {
        auto href = e->attrs.find(L"href");
        if (href != e->attrs.end()) currentHref = href->second;
    }
    Element* savedForm = currentForm;
    if (e->tag == L"form") currentForm = e;

    int childX = boxX + sv.borderWidth + sv.paddingLeft;
    if (sv.display == Display::Grid) layoutGrid(e, childX, y, contentWidth, sv);
    else if (sv.display == Display::Flex) layoutFlex(e, childX, y, contentWidth, sv);
    else layoutElement(e, childX, y, contentWidth, sv.fontSize, sv.visuallyHidden, sv.paint);

    currentHref = savedHref;
    currentForm = savedForm;

    // Explicit height:0 collapses this box regardless of its children's
    // natural size - real CSS clips them via overflow:hidden; this engine
    // has no clipping model, so it just doesn't let them push layout past
    // here. Children's own boxes keep whatever (real, un-collapsed)
    // positions they were laid out at - harmless as long as they're also
    // invisible (opacity:0/visibility:hidden), which is the only realistic
    // reason a page pairs height:0 with content still inside it.
    if (sv.height == 0) y = contentStartY + sv.borderWidth + sv.paddingTop;

    y += sv.paddingBottom + sv.borderWidth;

    if (bgIndex != static_cast<size_t>(-1)) {
        boxes[bgIndex].height = y - contentStartY;
    }

    y += sv.marginBottom;
}

// Places `el`'s grid items (its direct element children, minus the usual
// non-visual tags) into a grid - see the declaration in Layout.h for the
// numbered summary of the algorithm. Like layoutFlex, every item is laid
// out into its own scratch box list at local (0,0) first (the same
// std::swap(boxes, scratch) technique used throughout this file) to
// discover its natural height, since a row's height - especially one a
// multi-row item spans into - depends on items this single top-to-bottom
// pass hasn't reached yet.
void LayoutRoot::layoutGrid(Element* el, int x, int& y, int containingWidth, const ComputedStyle& style) {
    ancestorStack.push_back(el); // items' descendant-selector matching includes the grid container

    // One grid item: its element, its computed style (against containingWidth
    // - re-resolved against its real span width once that's known, below,
    // the same reason layoutFlex re-resolves per item), and its placement -
    // set here if the item is explicitly positioned (both axes), left as -1
    // (auto) otherwise for the placement pass further down to fill in.
    struct ItemPlacement {
        Element* el;
        ComputedStyle style;
        int colStart = -1, colEnd = -1; // 0-based cell range [start, end)
        int rowStart = -1, rowEnd = -1;
    };
    std::vector<ItemPlacement> placements;
    for (auto& child : el->children) {
        if (child->type != Node::ELEMENT) continue;
        auto* ce = static_cast<Element*>(child.get());
        if (ce->tag == L"head" || ce->tag == L"script" || ce->tag == L"style" ||
            ce->tag == L"title" || ce->tag == L"meta" || ce->tag == L"link" || ce->tag == L"base")
            continue;
        ComputedStyle cs = computeStyle(ce, style.fontSize, containingWidth, style.visuallyHidden, style.paint);
        if (cs.display == Display::None) continue;

        ItemPlacement p;
        p.el = ce;
        p.style = cs;
        // grid-area wins if it names a cell that actually exists in the
        // template; otherwise (no grid-template-areas, or a name that
        // doesn't appear in it) fall through to line-based placement.
        bool placedByArea = false;
        if (!cs.gridArea.empty()) {
            int minRow = -1, maxRow = -1, minCol = -1, maxCol = -1;
            for (int r = 0; r < (int)style.gridTemplateAreas.size(); r++) {
                auto& row = style.gridTemplateAreas[r];
                for (int c = 0; c < (int)row.size(); c++) {
                    if (row[c] != cs.gridArea) continue;
                    if (minRow < 0) { minRow = maxRow = r; minCol = maxCol = c; }
                    else { minRow = std::min(minRow, r); maxRow = std::max(maxRow, r); minCol = std::min(minCol, c); maxCol = std::max(maxCol, c); }
                }
            }
            if (minRow >= 0) { // the name was found - real CSS requires its cells to form a rectangle; this just takes their bounding box regardless
                p.colStart = minCol; p.colEnd = maxCol + 1;
                p.rowStart = minRow; p.rowEnd = maxRow + 1;
                placedByArea = true;
            }
        }
        // Both axes must be explicit for this item to be explicitly placed
        // - see the ComputedStyle/layoutGrid comments in Layout.h on why
        // one alone isn't treated as a partial placement.
        if (!placedByArea && cs.gridColumnStart > 0 && cs.gridRowStart > 0) {
            p.colStart = cs.gridColumnStart - 1; // 1-based line -> 0-based cell index
            p.colEnd = (cs.gridColumnEnd > 0 ? cs.gridColumnEnd - 1 : p.colStart + 1);
            p.rowStart = cs.gridRowStart - 1;
            p.rowEnd = (cs.gridRowEnd > 0 ? cs.gridRowEnd - 1 : p.rowStart + 1);
        }
        placements.push_back(std::move(p));
    }
    if (placements.empty()) { ancestorStack.pop_back(); return; }

    std::vector<GridTrack> cols = style.gridTemplateColumns;
    // grid-template-areas defines the column *count* even if
    // grid-template-columns doesn't have enough tracks for it - pad the
    // missing ones with 1fr, the same fallback "no template" already uses.
    int areaCols = style.gridTemplateAreas.empty() ? 0 : (int)style.gridTemplateAreas[0].size();
    while ((int)cols.size() < areaCols) cols.push_back({ true, 1.0f });
    if (cols.empty()) cols.push_back({ false, (float)containingWidth }); // no template at all -> one full-width column
    int numCols = (int)cols.size();

    int colGap = style.columnGap;
    int fixedTotal = 0;
    float frTotal = 0;
    for (auto& t : cols) { if (t.isFr) frTotal += t.value; else fixedTotal += (int)std::lround(t.value); }
    int remaining = std::max(containingWidth - colGap * std::max(numCols - 1, 0) - fixedTotal, 0);

    std::vector<int> colWidths(numCols), colX(numCols);
    int cx = x;
    for (int i = 0; i < numCols; i++) {
        int w = cols[i].isFr
            ? (frTotal > 0 ? (int)std::lround(remaining * (cols[i].value / frTotal)) : 0)
            : (int)std::lround(cols[i].value);
        colWidths[i] = std::max(w, 0);
        colX[i] = cx;
        cx += colWidths[i] + colGap;
    }

    // Explicitly-placed items: clamp to the template's column count (a
    // line beyond it doesn't create an implicit column - see Layout.h),
    // then claim their cells. Auto-placed items (colStart still -1) fill
    // in around them next, row-major, skipping any cell already claimed.
    std::set<std::pair<int, int>> occupied; // (row, col)
    // grid-template-areas defines the row count too, even for a trailing
    // row nothing ends up placed in (e.g. one made entirely of "." cells).
    int maxRowUsed = style.gridTemplateAreas.empty() ? -1 : (int)style.gridTemplateAreas.size() - 1;
    for (auto& p : placements) {
        if (p.colStart < 0) continue;
        p.colStart = std::clamp(p.colStart, 0, numCols - 1);
        p.colEnd = std::clamp(p.colEnd, p.colStart + 1, numCols);
        for (int r = p.rowStart; r < p.rowEnd; r++)
            for (int c = p.colStart; c < p.colEnd; c++)
                occupied.insert({ r, c });
        maxRowUsed = std::max(maxRowUsed, p.rowEnd - 1);
    }
    int cursorRow = 0, cursorCol = 0;
    for (auto& p : placements) {
        if (p.colStart >= 0) continue; // already explicitly placed
        while (occupied.count({ cursorRow, cursorCol })) {
            cursorCol++;
            if (cursorCol >= numCols) { cursorCol = 0; cursorRow++; }
        }
        p.colStart = cursorCol; p.colEnd = cursorCol + 1;
        p.rowStart = cursorRow; p.rowEnd = cursorRow + 1;
        occupied.insert({ cursorRow, cursorCol });
        maxRowUsed = std::max(maxRowUsed, cursorRow);
        cursorCol++;
        if (cursorCol >= numCols) { cursorCol = 0; cursorRow++; }
    }
    int numRows = maxRowUsed + 1;

    // Lay out every item at its resolved span width to discover its
    // natural height, exactly as layoutFlex does per item.
    std::vector<std::vector<LayoutBox>> itemBoxes(placements.size());
    std::vector<int> itemHeights(placements.size());
    for (size_t idx = 0; idx < placements.size(); idx++) {
        ItemPlacement& p = placements[idx];
        int spanWidth = colGap * (p.colEnd - p.colStart - 1);
        for (int c = p.colStart; c < p.colEnd; c++) spanWidth += colWidths[c];

        // Re-resolve against the item's real span width, not the
        // container's - matters for any %-based property on the item
        // itself (its own padding/margin/width), same reason layoutFlex does.
        ComputedStyle real = computeStyle(p.el, style.fontSize, spanWidth, p.style.visuallyHidden, style.paint);

        std::vector<LayoutBox> scratch;
        std::swap(boxes, scratch); // redirect every push_back below into `scratch`
        int localY = 0;
        if (p.el->tag == L"input" || p.el->tag == L"button" || p.el->tag == L"select") {
            layoutControl(p.el, 0, localY, spanWidth, real);
        } else if (p.el->tag == L"img") {
            layoutImage(p.el, 0, localY, spanWidth, real);
        } else {
            // A grid item is always block-level, regardless of its own
            // tag's default (real CSS "blockifies" it the same way).
            if (real.display == Display::Inline) real.display = Display::Block;
            layoutBlockChild(p.el, 0, localY, spanWidth, real);
        }
        std::swap(boxes, scratch);

        itemBoxes[idx] = std::move(scratch);
        itemHeights[idx] = localY;
    }

    // Row heights: an explicit grid-template-rows track wins if set (fr
    // tracks excepted, per its ComputedStyle comment); otherwise a row is
    // as tall as the tallest single-row item placed in it.
    std::vector<GridTrack> rowTracks = style.gridTemplateRows;
    std::vector<int> rowHeights(numRows, 0);
    std::vector<bool> rowExplicit(numRows, false);
    for (int r = 0; r < numRows; r++) {
        if (r < (int)rowTracks.size() && !rowTracks[r].isFr) {
            rowHeights[r] = (int)std::lround(rowTracks[r].value);
            rowExplicit[r] = true;
        }
    }
    for (size_t idx = 0; idx < placements.size(); idx++) {
        ItemPlacement& p = placements[idx];
        if (p.rowEnd - p.rowStart == 1 && !rowExplicit[p.rowStart])
            rowHeights[p.rowStart] = std::max(rowHeights[p.rowStart], itemHeights[idx]);
    }
    // A multi-row item bumps the *last* row it spans if the rows it's
    // already in (plus the row-gaps between them) aren't tall enough for
    // it, rather than distributing the shortfall across all of them -
    // simpler, and rare enough in practice (spanning items are less
    // common than spanning columns) not to be worth more than that.
    for (size_t idx = 0; idx < placements.size(); idx++) {
        ItemPlacement& p = placements[idx];
        int span = p.rowEnd - p.rowStart;
        if (span <= 1) continue;
        int sum = style.rowGap * (span - 1);
        for (int r = p.rowStart; r < p.rowEnd; r++) sum += rowHeights[r];
        if (itemHeights[idx] > sum && !rowExplicit[p.rowEnd - 1])
            rowHeights[p.rowEnd - 1] += itemHeights[idx] - sum;
    }

    std::vector<int> rowY(numRows);
    int cy = y;
    for (int r = 0; r < numRows; r++) {
        if (r > 0) cy += style.rowGap;
        rowY[r] = cy;
        cy += rowHeights[r];
    }

    for (size_t idx = 0; idx < placements.size(); idx++) {
        ItemPlacement& p = placements[idx];
        for (LayoutBox b : itemBoxes[idx]) { // copy: translate before appending to the real list
            b.x += colX[p.colStart];
            b.y += rowY[p.rowStart];
            boxes.push_back(std::move(b));
        }
    }

    y = cy;
    ancestorStack.pop_back();
}

// Places `el`'s flex items along style.flexDirection's main axis. Row and
// column direction are different enough (which axis is "main" swaps
// entirely) that they're really two algorithms sharing one function.
//
// Row direction: see the step-by-step comment at "Row direction." below
// for flex-basis/grow/shrink and wrapping. In a nowrap row, an item with
// no width/flex-basis gets a *share* of the leftover width - 1 share by
// default, or its own flex-grow. This isn't spec-accurate (real flexbox
// sizes an unflexed item by its *content*, via min/max-content sizing
// this engine has no equivalent of anywhere - text wrapping already needs
// a width handed to it, it doesn't derive one) but it's the same tradeoff
// layoutGrid already makes for an untemplated grid ("no template -> one
// full-width column" there; here, "no width/flex-grow -> an equal share"
// instead of collapsing to zero), and gives the common display:flex
// patterns (nav bars, equal-width card rows, button groups) a reasonable
// result. A wrapping row can't use that trick (nothing would ever wrap),
// so there such an item starts at a shrink-to-fit estimate instead.
// justify-content only has a visible effect on a line with space left
// over after flex-grow - which matches real flexbox's own behavior.
//
// Column direction: items are normal block children stacked vertically -
// height is whatever content needs, exactly like ordinary block layout
// already computes, so there's no equivalent sizing problem on the main
// axis. justify-content has no effect here: distributing leftover space
// along a container's height needs a *definite* height to distribute
// within, and this engine's pages are always "auto" height (they grow to
// fit content) - the same behavior real CSS flexbox shows for an
// auto-height flex column, not a shortcut unique to this engine.
// align-items does work on the cross axis (horizontal, here): an item
// with an explicit width can be positioned via flex-start/center/flex-end
// within the container's width; one without always fills it (stretch,
// the default - and the only sensible behavior for something with no
// natural width to fall back to, same reasoning as row direction).
void LayoutRoot::layoutFlex(Element* el, int x, int& y, int containingWidth, const ComputedStyle& style) {
    ancestorStack.push_back(el); // items' descendant-selector matching includes the flex container

    std::vector<Element*> items;
    std::vector<ComputedStyle> itemStyles; // against containingWidth - re-resolved against each item's real width below
    for (auto& child : el->children) {
        if (child->type != Node::ELEMENT) continue;
        auto* ce = static_cast<Element*>(child.get());
        if (ce->tag == L"head" || ce->tag == L"script" || ce->tag == L"style" ||
            ce->tag == L"title" || ce->tag == L"meta" || ce->tag == L"link" || ce->tag == L"base")
            continue;
        ComputedStyle cs = computeStyle(ce, style.fontSize, containingWidth, style.visuallyHidden, style.paint);
        if (cs.display == Display::None) continue;
        if (cs.display == Display::Inline) cs.display = Display::Block; // a flex item is always block-level, like a grid item
        items.push_back(ce);
        itemStyles.push_back(std::move(cs));
    }
    int n = (int)items.size();
    if (n == 0) { ancestorStack.pop_back(); return; }

    // Resolves one item's explicit outer width from its own style, or -1
    // if it didn't set one - shared by both directions below.
    auto explicitWidth = [](const ComputedStyle& s) -> int {
        if (s.width < 0) return -1;
        return s.boxSizing == BoxSizing::BorderBox
            ? s.width
            : s.width + s.paddingLeft + s.paddingRight + 2 * s.borderWidth;
    };

    if (style.flexDirection == FlexDirection::Column) {
        int cursorY = y;
        for (int i = 0; i < n; i++) {
            if (i > 0) cursorY += style.rowGap;

            int itemWidth = explicitWidth(itemStyles[i]);
            if (itemWidth < 0) itemWidth = containingWidth; // stretch (default): no natural width to fall back to, so fill it
            itemWidth = std::min(itemWidth, containingWidth);
            // Re-resolve against the item's real width, not the container's
            // - matters for any %-based property on the item itself (e.g.
            // its own padding/margin), same reason layoutGrid re-resolves
            // per column width instead of reusing its first pass.
            ComputedStyle real = computeStyle(items[i], style.fontSize, itemWidth, itemStyles[i].visuallyHidden, style.paint);

            int localY;
            std::vector<LayoutBox> scratch = layoutItemDetached(items[i], itemWidth, real, localY);

            int crossOffset = 0; // cross axis = horizontal, here
            if (itemWidth < containingWidth) {
                if (style.alignItems == AlignItems::Center) crossOffset = (containingWidth - itemWidth) / 2;
                else if (style.alignItems == AlignItems::FlexEnd) crossOffset = containingWidth - itemWidth;
                // FlexStart and Stretch: crossOffset stays 0 (stretch already filled the width above)
            }
            for (LayoutBox b : scratch) {
                b.x += x + crossOffset;
                b.y += cursorY;
                boxes.push_back(std::move(b));
            }
            cursorY += localY;
        }
        y = cursorY;
        ancestorStack.pop_back();
        return;
    }

    // Row direction.
    //
    // 1. Each item gets a hypothetical outer width: its flex-basis if set,
    //    else its explicit width. An item with neither is sized one of two
    //    ways. In a nowrap row it starts at 0 and takes a share of the
    //    leftover space (flex-grow, or 1 if unset) - the engine's original
    //    behavior, kept because there's no real content-based sizing.
    //    In a wrapping row that would never wrap anything, so it starts at
    //    a shrink-to-fit estimate instead (shrinkToFitWidth) and, as in
    //    real CSS, only grows if flex-grow says so.
    // 2. Items are broken into lines (only with flex-wrap), each as many
    //    items as fit - an item wider than the container gets a line of
    //    its own.
    // 3. Per line: positive free space goes to items by flex-grow;
    //    negative free space (overflow) is taken back by flex-shrink
    //    weighted by each item's basis, the spec's "scaled shrink factor".
    //    An item never shrinks below its own padding+border. (Real CSS
    //    also won't shrink below the content's min-content width by
    //    default; with no min-content sizing here, text just wraps tighter.)
    // 4. Per line: justify-content and align-items, as before. Lines stack
    //    top to bottom with row-gap between them (bottom to top for
    //    wrap-reverse); align-content isn't supported, so lines never
    //    spread out vertically.
    int colGap = style.columnGap;
    bool wraps = style.flexWrap != FlexWrap::NoWrap;

    std::vector<int> basis(n), minWidth(n);
    std::vector<float> grow(n), shrink(n);
    for (int i = 0; i < n; i++) {
        const ComputedStyle& s = itemStyles[i];
        int chrome = s.paddingLeft + s.paddingRight + 2 * s.borderWidth;
        minWidth[i] = chrome;
        int w;
        if (s.flexBasis >= 0) w = s.boxSizing == BoxSizing::BorderBox ? s.flexBasis : s.flexBasis + chrome;
        else w = explicitWidth(s);
        if (w >= 0) {
            basis[i] = w;
            grow[i] = s.flexGrow;
        } else if (wraps) {
            basis[i] = shrinkToFitWidth(items[i], s, containingWidth);
            grow[i] = s.flexGrow;
        } else {
            basis[i] = 0;
            grow[i] = s.flexGrowSet ? s.flexGrow : 1.0f;
        }
        basis[i] = std::max(basis[i], minWidth[i]);
        shrink[i] = s.flexShrink;
    }

    // Break into lines: [start, end) index ranges.
    std::vector<std::pair<int, int>> lines;
    for (int i = 0; i < n;) {
        int start = i, used = basis[i++];
        while (wraps && i < n && used + colGap + basis[i] <= containingWidth) used += colGap + basis[i++];
        if (!wraps) i = n;
        lines.push_back({ start, i });
    }
    if (style.flexWrap == FlexWrap::WrapReverse) std::reverse(lines.begin(), lines.end());

    std::vector<int> itemWidths(basis);
    int lineY = y;
    for (size_t li = 0; li < lines.size(); li++) {
        auto [a, b] = lines[li];
        int count = b - a;
        if (li > 0) lineY += style.rowGap;

        int used = colGap * (count - 1);
        float totalGrow = 0, totalScaledShrink = 0;
        for (int i = a; i < b; i++) {
            used += basis[i];
            totalGrow += grow[i];
            totalScaledShrink += shrink[i] * basis[i];
        }
        int free = containingWidth - used;
        for (int i = a; i < b; i++) {
            if (free > 0 && totalGrow > 0)
                itemWidths[i] = basis[i] + (int)std::lround(free * (grow[i] / totalGrow));
            else if (free < 0 && totalScaledShrink > 0)
                itemWidths[i] = std::max(minWidth[i],
                    basis[i] + (int)std::lround(free * (shrink[i] * basis[i] / totalScaledShrink)));
        }

        // Lay out each item at its resolved width to discover its natural
        // height - not knowable up front the same way layoutGrid can't know
        // a row's height before placing everything in it.
        std::vector<std::vector<LayoutBox>> itemBoxes(count);
        std::vector<int> itemHeights(count);
        int lineHeight = 0;
        for (int i = a; i < b; i++) {
            ComputedStyle real = computeStyle(items[i], style.fontSize, itemWidths[i], itemStyles[i].visuallyHidden, style.paint); // see the column-direction branch's comment on why
            // The flexed width replaces the item's own `width` (which only
            // fed its basis above) - otherwise layoutBlockChild would draw
            // a grown/shrunk item at its original CSS width.
            real.width = -1;
            itemBoxes[i - a] = layoutItemDetached(items[i], itemWidths[i], real, itemHeights[i - a]);
            lineHeight = std::max(lineHeight, itemHeights[i - a]);
        }

        int usedWidth = colGap * (count - 1);
        for (int i = a; i < b; i++) usedWidth += itemWidths[i];
        int leftover = std::max(containingWidth - usedWidth, 0);

        int startX = x;
        int extraGap = 0;
        switch (style.justifyContent) {
            case JustifyContent::FlexStart: break;
            case JustifyContent::Center: startX += leftover / 2; break;
            case JustifyContent::FlexEnd: startX += leftover; break;
            case JustifyContent::SpaceBetween: if (count > 1) extraGap = leftover / (count - 1); break;
            case JustifyContent::SpaceAround: {
                int around = leftover / count;
                startX += around / 2;
                extraGap = around;
                break;
            }
        }

        int cursorX = startX;
        for (int i = a; i < b; i++) {
            int k = i - a;
            int itemY = 0;
            if (style.alignItems == AlignItems::Center) itemY = (lineHeight - itemHeights[k]) / 2;
            else if (style.alignItems == AlignItems::FlexEnd) itemY = lineHeight - itemHeights[k];
            // FlexStart and Stretch both start at the line's top; stretch is
            // approximated by extending the item's own background/border box
            // (if it made one) to the line's height, rather than by
            // re-flowing its content into the extra space - a box with no
            // background/border has nothing visible to stretch anyway.
            if (style.alignItems == AlignItems::Stretch) {
                for (auto& box : itemBoxes[k]) if (box.el == items[i]) { box.height = lineHeight; break; }
            }

            for (LayoutBox box : itemBoxes[k]) {
                box.x += cursorX;
                box.y += lineY + itemY;
                boxes.push_back(std::move(box));
            }
            cursorX += itemWidths[i] + colGap + extraGap;
        }
        lineY += lineHeight;
    }

    y = lineY;
    ancestorStack.pop_back();
}

std::vector<LayoutBox> LayoutRoot::layoutItemDetached(Element* item, int width, const ComputedStyle& style, int& height) {
    std::vector<LayoutBox> scratch;
    std::swap(boxes, scratch);
    int localY = 0;
    if (item->tag == L"input" || item->tag == L"button" || item->tag == L"select")
        layoutControl(item, 0, localY, width, style);
    else if (item->tag == L"img")
        layoutImage(item, 0, localY, width, style);
    else
        layoutBlockChild(item, 0, localY, width, style);
    std::swap(boxes, scratch);
    height = localY;
    return scratch;
}

int LayoutRoot::shrinkToFitWidth(Element* item, const ComputedStyle& style, int available) {
    int height;
    std::vector<LayoutBox> trial = layoutItemDetached(item, available, style, height);
    int right = 0;
    for (const auto& b : trial) {
        // Only content has a natural width; a background box just spans
        // whatever width it was given, so it says nothing about fit.
        bool content = !b.text.empty() || !b.imageSrc.empty() || b.control != LayoutBox::NoControl;
        if (!content) continue;
        // A text box starts at the run's content edge, but layoutInlineRun
        // wraps at the content width minus a 4px inset on *each* side.
        int textInset = b.text.empty() || b.control != LayoutBox::NoControl ? 0 : 8;
        right = std::max(right, b.x + b.width + textInset);
    }
    int width = right + style.paddingRight + style.borderWidth + style.marginRight + 1; // +1: rounding in text measurement
    return std::min(std::max(width, 0), available);
}
