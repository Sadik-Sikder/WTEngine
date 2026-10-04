#define NOMINMAX
#include "Layout.h"
#include "Renderer.h" // tryParseColor
#include "Svg.h"
#include <windows.h>
#include <string>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <set>
#include <utility>
#include <climits>

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
    if (i < v.size() && (v[i] == L'-' || v[i] == L'+')) i++; // top: -8px, margin-left: -1rem, ...
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
    if (unit == L"vw") return (int)std::lround(value * viewportWidth / 100.0);
    if (unit == L"vh" || unit == L"vmin" || unit == L"vmax") usedViewportHeight = true;
    if (unit == L"vh") return (int)std::lround(value * viewportHeight / 100.0);
    if (unit == L"vmin") return (int)std::lround(value * std::min(viewportWidth, viewportHeight) / 100.0);
    if (unit == L"vmax") return (int)std::lround(value * std::max(viewportWidth, viewportHeight) / 100.0);
    return def;
}

// height/min-height/max-height: like resolveLength, but a percentage is of
// the containing block's height - auto (`def`) when that isn't definite -
// and em/rem work too.
int LayoutRoot::resolveHeight(const std::wstring& s, int fontSize, int def) {
    std::wstring v = trimmed(s);
    double value; std::wstring unit;
    if (!parseNumberAndUnit(v, value, unit)) return def;
    if (unit == L"%") return containingHeight_ >= 0 ? (int)std::lround(value * containingHeight_ / 100.0) : def;
    if (unit == L"em" || unit == L"rem") return (int)std::lround(value * fontSize);
    return resolveLength(v, 0, def);
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

float LayoutRoot::textWidth(const std::wstring& text, int fontSize, bool bold, bool italic, const std::wstring* family) {
    if (measureText) return measureText(text, fontSize, bold, italic, family);
    return text.size() * fontSize * 0.55f; // rough fallback
}

int LayoutRoot::lineBand(const TextPaint& p, int fontSize) {
    if (p.lineHeightPx >= 0) return p.lineHeightPx;
    if (p.lineHeight >= 0) return (int)std::lround(p.lineHeight * fontSize);
    return fontSize + 8; // "normal"
}

// --- font-family --------------------------------------------------------

// Every font family installed on this machine, lowercased - read once.
static const std::set<std::wstring>& installedFonts() {
    static const std::set<std::wstring> fonts = [] {
        std::set<std::wstring> out;
        HDC dc = GetDC(nullptr);
        LOGFONTW lf{};
        lf.lfCharSet = DEFAULT_CHARSET;
        EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW* f, const TEXTMETRICW*, DWORD, LPARAM param) -> int {
            std::wstring name = f->lfFaceName;
            if (!name.empty() && name[0] != L'@') { // '@' = vertical-writing variant
                for (auto& c : name) c = (wchar_t)towlower(c);
                reinterpret_cast<std::set<std::wstring>*>(param)->insert(name);
            }
            return 1;
        }, reinterpret_cast<LPARAM>(&out), 0);
        ReleaseDC(nullptr, dc);
        return out;
    }();
    return fonts;
}

// A pointer that stays valid (and equal) for every use of the same name.
static const std::wstring* internFont(const std::wstring& face) {
    static std::set<std::wstring> pool;
    return &*pool.insert(face).first;
}

// The face to draw a CSS font-family list with: the first entry that's
// installed, with the generic families mapped to Windows' usual choices.
// Web fonts (@font-face) aren't loaded, so a page's own font falls through
// to the next entry, which is what browsers show while it downloads.
// nullptr means the default (Segoe UI) - also for "sans-serif".
static const std::wstring* resolveFontFamily(const std::wstring& list, bool& recognized) {
    static const std::pair<const wchar_t*, const wchar_t*> kGeneric[] = {
        { L"sans-serif", nullptr }, { L"system-ui", nullptr }, { L"-apple-system", nullptr },
        { L"blinkmacsystemfont", nullptr }, { L"ui-sans-serif", nullptr },
        { L"serif", L"Times New Roman" }, { L"ui-serif", L"Georgia" },
        { L"monospace", L"Consolas" }, { L"ui-monospace", L"Consolas" },
        { L"cursive", L"Comic Sans MS" }, { L"fantasy", L"Impact" },
    };
    recognized = false;
    std::wstring name;
    auto tryName = [&](std::wstring n) -> const std::wstring* {
        n = trimmed(n);
        if (n.size() >= 2 && (n[0] == L'"' || n[0] == L'\'')) n = n.substr(1, n.size() - 2);
        std::wstring lower = n;
        for (auto& c : lower) c = (wchar_t)towlower(c);
        for (const auto& [generic, face] : kGeneric) {
            if (lower == generic) { recognized = true; return face ? internFont(face) : nullptr; }
        }
        if (lower == L"segoe ui") { recognized = true; return nullptr; }
        if (installedFonts().count(lower)) { recognized = true; return internFont(n); }
        return nullptr;
    };
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(L',', start);
        if (comma == std::wstring::npos) comma = list.size();
        const std::wstring* face = tryName(list.substr(start, comma - start));
        if (recognized) return face;
        start = comma + 1;
    }
    return nullptr;
}

// Tags treated as inline-level by default: their text/content flows onto
// the same line as their surrounding siblings instead of starting a block
// of its own. Everything else defaults to block, matching the engine's
// original (pre-inline) universal-block behavior.
static bool isInlineTag(const std::wstring& tag) {
    static const std::wstring kInline[] = {
        L"a", L"span", L"b", L"strong", L"i", L"em", L"u", L"small", L"code",
        L"sub", L"sup", L"mark", L"label", L"abbr", L"cite", L"q", L"img", L"svg",
        L"del", L"ins", L"s", L"strike", L"kbd", L"samp", L"var", L"dfn", L"tt", L"time",
        L"bdi", L"bdo", L"big", L"font", L"data", L"output"
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
        bool space = false;
        while (i < text.size() && iswspace(text[i])) { i++; space = true; }
        size_t start = i;
        while (i < text.size() && !iswspace(text[i])) i++;
        if (start == i) {
            // Trailing whitespace: carried to whatever item comes next.
            if (space) {
                InlineItem marker{ L"", fontSize, href, owner, false, visuallyHidden, paint };
                marker.isSpace = true;
                out.push_back(std::move(marker));
            }
            break;
        }
        std::wstring word = text.substr(start, i - start);
        switch (paint.transform) { // text-transform changes what's drawn (and measured), not the DOM
        case TextTransform::Uppercase: for (auto& c : word) c = (wchar_t)towupper(c); break;
        case TextTransform::Lowercase: for (auto& c : word) c = (wchar_t)towlower(c); break;
        case TextTransform::Capitalize: word[0] = (wchar_t)towupper(word[0]); break;
        case TextTransform::None: break;
        }
        InlineItem item{ std::move(word), fontSize, href, owner, false, visuallyHidden, paint };
        item.spaceBefore = space; // false for the first word of text glued to what precedes it
        out.push_back(std::move(item));
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
        if (sv.position == ComputedStyle::Position::Absolute || sv.position == ComputedStyle::Position::Fixed) {
            deferOutOfFlow(e, sv, 0, 0, false); // out of the flow; no position in the line is known here
            continue;
        }

        // An image inside inline content (<a><svg>...</svg> Home</a>) sits
        // in the line like a word - never recursed into, so an <svg>'s
        // <title>/<text> can't leak in as words.
        if (e->tag == L"img" || e->tag == L"svg") {
            appendImage(e, containingWidth, sv, out);
            continue;
        }

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
void LayoutRoot::layoutInlineRun(const std::vector<InlineItem>& items, int x, int& y, int containingWidth,
                                 TextAlign align) {
    const int textInset = 4; // Engine::render draws text 4px inside its box
    const int paraGap = 6;
    const float maxWidth = (float)std::max(containingWidth - 2 * textInset, 40);

    struct Placed { InlineItem item; float offset; float width; };
    std::vector<Placed> line;
    float lineWidth = 0;

    auto emitLine = [&]() {
        if (line.empty()) return;
        // Text sits in a band as tall as the line-height of its largest font
        // (fontSize + 8 for "normal"), each word centred in it. An image
        // taller than that makes the line taller, and the text then sits at
        // the line's bottom - roughly where a browser puts an image on the
        // baseline. An image shorter than the text is centred on it, the
        // usual look of an icon next to a label.
        int textBand = 0, imageHeight = 0;
        for (auto& p : line) {
            if (p.item.image) imageHeight = std::max(imageHeight, p.item.image->height);
            else textBand = std::max(textBand, lineBand(p.item.paint, p.item.fontSize));
        }
        int lineHeight = std::max(textBand, imageHeight);
        int textTop = y + lineHeight - textBand;

        // text-align: the whole line moves by its leftover width.
        float shift = 0;
        if (align == TextAlign::Center) shift = std::max(maxWidth - lineWidth, 0.0f) / 2;
        else if (align == TextAlign::Right) shift = std::max(maxWidth - lineWidth, 0.0f);

        for (size_t i = 0; i < line.size(); i++) {
            const Placed& p = line[i];
            int left = x + (int)std::lround(p.offset + shift);
            if (p.item.image) {
                LayoutBox box = *p.item.image;
                // + textInset: offsets are measured where text is drawn, 4px
                // inside its box - without it an image hugs the word before it.
                box.x = left + textInset + p.item.marginLeft;
                box.y = textBand > box.height ? textTop + (textBand - box.height) / 2 : y + lineHeight - box.height;
                box.href = p.item.href;
                boxes.push_back(box);
                continue;
            }
            const TextPaint& paint = p.item.paint;
            int natural = p.item.fontSize + 8; // the box Engine::render draws text 4px into
            LayoutBox box;
            box.x = left;
            box.y = textTop + (textBand - natural) / 2;
            box.width = (int)std::lround(p.width);
            box.height = natural;
            box.text = p.item.word;
            box.href = p.item.href;
            box.fontSize = p.item.fontSize;
            box.color = paint.color;
            box.bold = paint.bold;
            box.italic = paint.italic;
            box.family = paint.family;
            box.underline = paint.underline;
            box.lineThrough = paint.lineThrough;
            // A decoration runs on through the space to the next word when
            // that word is decorated the same way, so a phrase gets one line.
            box.decorationWidth = box.width;
            if ((paint.underline || paint.lineThrough) && i + 1 < line.size()) {
                const Placed& next = line[i + 1];
                if (!next.item.image && next.item.paint.underline == paint.underline &&
                    next.item.paint.lineThrough == paint.lineThrough && next.item.paint.color == paint.color)
                    box.decorationWidth = (int)std::lround(next.offset - p.offset);
            }
            box.el = p.item.owner;
            box.visuallyHidden = p.item.visuallyHidden;
            boxes.push_back(box);
        }
        y += lineHeight;
        line.clear();
        lineWidth = 0;
    };

    // A run of nothing but whitespace (between two blocks, say) takes no space.
    if (std::all_of(items.begin(), items.end(), [](const InlineItem& i) { return i.isSpace; })) return;

    // Whether the HTML had whitespace before the next item (see InlineItem).
    bool pendingSpace = false;
    auto gapBefore = [&](const InlineItem& item) {
        bool gap = !line.empty() && (item.spaceBefore || pendingSpace);
        pendingSpace = false;
        return gap ? textWidth(L" ", item.fontSize, item.paint) : 0.0f;
    };

    for (const auto& raw : items) {
        if (raw.isSpace) { pendingSpace = true; continue; }
        if (raw.isBreak) { emitLine(); pendingSpace = false; continue; }

        if (raw.image) {
            // Wraps like a word; never split.
            float w = (float)(raw.marginLeft + raw.image->width + raw.marginRight);
            float spaceWidth = gapBefore(raw);
            if (!line.empty() && lineWidth + spaceWidth + w > maxWidth) emitLine();
            float offset = line.empty() ? 0 : lineWidth + spaceWidth;
            lineWidth = offset + w;
            line.push_back({ raw, offset, w });
            continue;
        }
        if (raw.word.empty()) continue;

        InlineItem item = raw;
        float wordWidth = textWidth(item.word, item.fontSize, item.paint);

        // A word wider than the line on its own gets broken by characters,
        // one fragment per line, before whatever's left of it (now short
        // enough) falls through to the normal wrapping below.
        while (wordWidth > maxWidth && item.word.size() > 1) {
            if (!line.empty()) emitLine();
            size_t n = 1;
            while (n < item.word.size() && textWidth(item.word.substr(0, n + 1), item.fontSize, item.paint) <= maxWidth) n++;
            InlineItem fragment = item;
            fragment.word = item.word.substr(0, n);
            float fragmentWidth = textWidth(fragment.word, fragment.fontSize, fragment.paint);
            line.push_back({ fragment, 0, fragmentWidth });
            lineWidth = fragmentWidth;
            emitLine();
            item.word.erase(0, n);
            wordWidth = textWidth(item.word, item.fontSize, item.paint);
        }
        if (item.word.empty()) continue;

        float spaceWidth = gapBefore(item);
        if (!line.empty() && lineWidth + spaceWidth + wordWidth > maxWidth) emitLine();

        float offset = line.empty() ? 0 : lineWidth + spaceWidth;
        lineWidth = offset + wordWidth;
        line.push_back({ item, offset, wordWidth });
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
// (Trimmed; whitespace-only text is skipped.)
static std::wstring firstText(Element* el) {
    for (auto& child : el->children) {
        if (child->type == Node::TEXT) {
            std::wstring t = trimmed(static_cast<TextNode*>(child.get())->text);
            if (!t.empty()) return t;
            continue;
        }
        std::wstring t = firstText(static_cast<Element*>(child.get()));
        if (!t.empty()) return t;
    }
    return L"";
}

// Replaces every var(--name) / var(--name, fallback) in `v` with that
// custom property's value, or else the fallback. Returns false if one has
// neither - the whole declaration is then invalid and ignored, as in CSS.
// `depth` stops a var() cycle (--a: var(--b); --b: var(--a)).
static bool substituteVars(const std::wstring& v, const CSSVars* vars, std::wstring& out, int depth = 0) {
    if (depth > 16) return false;
    out.clear();
    size_t i = 0;
    while (true) {
        size_t at = v.find(L"var(", i);
        if (at == std::wstring::npos) { out.append(v, i, std::wstring::npos); return true; }
        out.append(v, i, at - i);

        size_t p = at + 4, comma = std::wstring::npos;
        for (int level = 1; p < v.size(); p++) {
            if (v[p] == L'(') level++;
            else if (v[p] == L')' && --level == 0) break;
            else if (v[p] == L',' && level == 1 && comma == std::wstring::npos) comma = p;
        }
        if (p >= v.size()) return false; // unbalanced parentheses

        std::wstring name = trimmed(v.substr(at + 4, (comma == std::wstring::npos ? p : comma) - (at + 4)));
        for (auto& c : name) c = (wchar_t)towlower(c); // CSS::parseDeclarations lowercases the names it defines
        std::wstring piece;
        const std::wstring* value = vars ? vars->find(name) : nullptr;
        if (value) {
            if (!substituteVars(*value, vars, piece, depth + 1)) return false;
        }
        else if (comma != std::wstring::npos) {
            if (!substituteVars(trimmed(v.substr(comma + 1, p - comma - 1)), vars, piece, depth + 1)) return false;
        }
        else {
            return false;
        }
        out += piece;
        i = p + 1;
    }
}

bool LayoutRoot::parseRadius(const std::wstring& v, int fontSize, LayoutBox::CornerRadius& out) {
    size_t used = 0;
    float n;
    try { n = std::stof(v, &used); } catch (...) { return false; }
    std::wstring unit = v.substr(used);
    if (n < 0) return false;
    if (unit == L"%") out = { n, true };
    else if (unit == L"px" || (unit.empty() && n == 0)) out = { n, false };
    else if (unit == L"em" || unit == L"rem") out = { n * fontSize, false };
    else return false;
    return true;
}

void LayoutBox::cornerRadii(float out[4]) const {
    float smaller = (float)std::min(width, height);
    for (int i = 0; i < 4; i++)
        out[i] = radius[i].percent ? radius[i].value / 100.0f * smaller : radius[i].value;
    // Adjacent corners can't overlap along a side: scale all four down
    // together until they fit, as CSS does.
    float scale = 1;
    auto fit = [&](float side, float a, float b) { if (a + b > side && a + b > 0) scale = std::min(scale, side / (a + b)); };
    fit((float)width, out[0], out[1]);
    fit((float)width, out[3], out[2]);
    fit((float)height, out[0], out[3]);
    fit((float)height, out[1], out[2]);
    for (int i = 0; i < 4; i++) out[i] *= scale;
}

// --- background-image, gradients, box-shadow --------------------------------

// Splits a value on commas outside parentheses: the layers of a
// background, the shadows of a box-shadow, the arguments of a gradient.
static std::vector<std::wstring> splitTopLevelCommas(const std::wstring& v) {
    std::vector<std::wstring> out;
    std::wstring part;
    int depth = 0;
    for (wchar_t c : v) {
        if (c == L'(') depth++;
        else if (c == L')' && depth > 0) depth--;
        if (c == L',' && depth == 0) { out.push_back(trimmed(part)); part.clear(); }
        else part += c;
    }
    out.push_back(trimmed(part));
    return out;
}

// A length in pixels for shadows and gradient stops: px, em/rem (against
// `fontSize`), or a bare 0.
static bool pixelLength(const std::wstring& tok, int fontSize, float& out) {
    double n; std::wstring unit;
    if (!parseNumberAndUnit(tok, n, unit)) return false;
    if (unit == L"px" || (unit.empty() && n == 0)) out = (float)n;
    else if (unit == L"em" || unit == L"rem") out = (float)(n * fontSize);
    else return false;
    return true;
}

// A background-position/-size component: a keyword's percentage, a
// percentage, a length, or (`allowAuto`) auto.
static bool bgLength(const std::wstring& tok, int fontSize, BgLength& out, bool allowAuto) {
    if (tok == L"left" || tok == L"top") { out = { 0, true, false }; return true; }
    if (tok == L"center") { out = { 50, true, false }; return true; }
    if (tok == L"right" || tok == L"bottom") { out = { 100, true, false }; return true; }
    if (allowAuto && tok == L"auto") { out = { 0, false, true }; return true; }
    double n; std::wstring unit;
    if (parseNumberAndUnit(tok, n, unit) && unit == L"%") { out = { (float)n, true, false }; return true; }
    float px;
    if (pixelLength(tok, fontSize, px)) { out = { px, false, false }; return true; }
    return false;
}

static bool isPositionKeyword(const std::wstring& t) {
    return t == L"left" || t == L"right" || t == L"top" || t == L"bottom" || t == L"center";
}

// background-position's 1-, 2- and keyword forms ("center", "right top",
// "top", "20px 50%"). The 3/4-value offset forms ("right 10px bottom 5px")
// keep just their keywords.
static void parseBgPosition(std::vector<std::wstring> toks, int fontSize, BgLength& x, BgLength& y) {
    if (toks.size() >= 3) {
        std::vector<std::wstring> keywords;
        for (const auto& t : toks) if (isPositionKeyword(t)) keywords.push_back(t);
        toks = keywords;
    }
    if (toks.empty()) return;
    BgLength center{ 50, true, false };
    if (toks.size() == 1) {
        if (toks[0] == L"top" || toks[0] == L"bottom") { x = center; bgLength(toks[0], fontSize, y, false); }
        else if (bgLength(toks[0], fontSize, x, false)) y = center;
        return;
    }
    std::wstring a = toks[0], b = toks[1];
    if (a == L"top" || a == L"bottom" || b == L"left" || b == L"right") std::swap(a, b); // "top right" = "right top"
    BgLength px, py;
    if (bgLength(a, fontSize, px, false) && bgLength(b, fontSize, py, false)) { x = px; y = py; }
}

static void parseBgSize(const std::vector<std::wstring>& toks, int fontSize, BackgroundLayer& layer) {
    if (toks.empty()) return;
    if (toks[0] == L"cover") { layer.size = BackgroundLayer::Size::Cover; return; }
    if (toks[0] == L"contain") { layer.size = BackgroundLayer::Size::Contain; return; }
    BgLength w, h{ 0, false, true };
    if (!bgLength(toks[0], fontSize, w, true)) return;
    if (toks.size() > 1 && !bgLength(toks[1], fontSize, h, true)) return;
    layer.size = BackgroundLayer::Size::Explicit;
    layer.width = w;
    layer.height = h;
}

static bool isRepeatKeyword(const std::wstring& t) {
    return t == L"repeat" || t == L"no-repeat" || t == L"repeat-x" || t == L"repeat-y" || t == L"space" || t == L"round";
}

static void parseBgRepeat(const std::vector<std::wstring>& toks, BackgroundLayer& layer) {
    if (toks.empty()) return;
    auto repeats = [](const std::wstring& t) { return t != L"no-repeat"; }; // space/round tile like repeat here
    if (toks[0] == L"repeat-x") { layer.repeatX = true; layer.repeatY = false; }
    else if (toks[0] == L"repeat-y") { layer.repeatX = false; layer.repeatY = true; }
    else if (toks.size() >= 2) { layer.repeatX = repeats(toks[0]); layer.repeatY = repeats(toks[1]); }
    else layer.repeatX = layer.repeatY = repeats(toks[0]);
}

// A CSS angle in degrees, or false.
static bool cssAngle(const std::wstring& tok, float& degrees) {
    double n; std::wstring unit;
    if (!parseNumberAndUnit(tok, n, unit)) return false;
    if (unit == L"deg") degrees = (float)n;
    else if (unit == L"turn") degrees = (float)(n * 360);
    else if (unit == L"rad") degrees = (float)(n * 180 / 3.14159265);
    else if (unit == L"grad") degrees = (float)(n * 0.9);
    else if (unit.empty() && n == 0) degrees = 0;
    else return false;
    return true;
}

// linear-gradient(), radial-gradient(), their repeating- forms, and the
// old -webkit-/-moz- prefixed syntax (where a bare side names where the
// gradient starts, not where it goes).
static bool parseGradient(const std::wstring& raw, Gradient& g) {
    std::wstring v = lowerCase(trimmed(raw));
    bool prefixed = false;
    for (const wchar_t* prefix : { L"-webkit-", L"-moz-", L"-o-" }) {
        if (v.rfind(prefix, 0) == 0) { v.erase(0, wcslen(prefix)); prefixed = true; }
    }
    if (v.rfind(L"repeating-", 0) == 0) { g.repeating = true; v.erase(0, 10); }
    if (v.rfind(L"linear-gradient(", 0) == 0) g.radial = false;
    else if (v.rfind(L"radial-gradient(", 0) == 0) g.radial = true;
    else return false;
    size_t open = v.find(L'('), close = v.rfind(L')');
    if (close == std::wstring::npos || close <= open) return false;
    std::vector<std::wstring> args = splitTopLevelCommas(v.substr(open + 1, close - open - 1));

    size_t first = 0;
    std::vector<std::wstring> head = cssTokens(args[0]);
    if (!g.radial) {
        float deg;
        if (head.size() == 1 && cssAngle(head[0], deg)) {
            g.angle = prefixed ? 90 - deg : deg; // the old syntax measured angles from the right, counterclockwise
            first = 1;
        }
        else if (!head.empty() && (head[0] == L"to" || (prefixed && isPositionKeyword(head[0])))) {
            int sx = 0, sy = 0;
            for (const auto& t : head) {
                if (t == L"left") sx = -1; else if (t == L"right") sx = 1;
                else if (t == L"top") sy = -1; else if (t == L"bottom") sy = 1;
            }
            if (prefixed && head[0] != L"to") { sx = -sx; sy = -sy; } // "top" = starts at the top, goes down
            if (sx && sy) { g.toCorner = true; g.cornerX = sx; g.cornerY = sy; }
            else if (sx) g.angle = sx > 0 ? 90.0f : 270.0f;
            else if (sy) g.angle = sy > 0 ? 180.0f : 0.0f;
            first = 1;
        }
    }
    else {
        // [circle | ellipse] [<size keyword>] [at <position>] - only the
        // shape and centre are used; the size is always farthest-corner.
        bool shapeArg = false;
        for (size_t i = 0; i < head.size(); i++) {
            const auto& t = head[i];
            if (t == L"circle") { g.circle = true; shapeArg = true; }
            else if (t == L"ellipse" || t.find(L"-side") != std::wstring::npos ||
                     t.find(L"-corner") != std::wstring::npos) shapeArg = true;
            else if (t == L"at") {
                shapeArg = true;
                BgLength x{ 50, true, false }, y{ 50, true, false };
                parseBgPosition(std::vector<std::wstring>(head.begin() + i + 1, head.end()), 16, x, y);
                if (x.percent) g.cx = x.value / 100;
                if (y.percent) g.cy = y.value / 100;
                break;
            }
        }
        if (shapeArg) first = 1;
    }

    // Colour stops: a colour with zero, one or two positions. A lone
    // position (an interpolation hint) is skipped.
    for (size_t i = first; i < args.size(); i++) {
        Color color;
        bool hasColor = false;
        std::vector<GradientStop> positions;
        for (const auto& t : cssTokens(args[i])) {
            if (!hasColor && tryParseColor(t, color)) { hasColor = true; continue; }
            double n; std::wstring unit;
            GradientStop s;
            float px;
            if (parseNumberAndUnit(t, n, unit) && unit == L"%") { s.hasPos = true; s.pos = (float)(n / 100); }
            else if (pixelLength(t, 16, px)) { s.hasPos = true; s.px = true; s.pos = px; }
            else continue;
            positions.push_back(s);
        }
        if (!hasColor) continue;
        if (positions.empty()) positions.push_back(GradientStop{});
        for (auto& s : positions) { s.color = color; g.stops.push_back(s); }
    }
    if (g.stops.empty()) return false;
    if (g.stops.size() == 1) g.stops.push_back(g.stops[0]); // one colour: a solid fill
    return true;
}

// An image layer's image: url(...) (quotes removed) or a gradient.
static bool parseBgImage(const std::wstring& tok, BackgroundLayer& layer) {
    std::wstring lower = lowerCase(tok);
    if (lower.rfind(L"url(", 0) == 0 && tok.size() > 5 && tok.back() == L')') {
        std::wstring url = trimmed(tok.substr(4, tok.size() - 5));
        if (url.size() >= 2 && (url[0] == L'"' || url[0] == L'\'')) url = url.substr(1, url.size() - 2);
        if (url.empty()) return false;
        layer.image = url;
        return true;
    }
    if (lower.find(L"gradient(") != std::wstring::npos) {
        auto g = std::make_shared<Gradient>();
        if (!parseGradient(tok, *g)) return false;
        layer.gradient = g;
        return true;
    }
    return false;
}

// One layer of the `background` shorthand: any of an image, a position
// (with "/ size" after it), a repeat, and other keywords, in any order.
// The colour is handled separately.
static void parseBackgroundLayer(const std::wstring& text, int fontSize, BackgroundLayer& layer) {
    // "center/cover" is one token; split the slash out (outside parentheses).
    std::wstring spaced;
    int depth = 0;
    for (wchar_t c : text) {
        if (c == L'(') depth++;
        else if (c == L')' && depth > 0) depth--;
        if (c == L'/' && depth == 0) spaced += L" / ";
        else spaced += c;
    }
    std::vector<std::wstring> position, size, repeat;
    bool afterSlash = false;
    for (const auto& tok : cssTokens(spaced)) {
        std::wstring t = lowerCase(tok);
        BgLength unused;
        if (t == L"/") afterSlash = true;
        else if (parseBgImage(tok, layer)) continue;
        else if (isRepeatKeyword(t)) repeat.push_back(t);
        else if (afterSlash && (t == L"cover" || t == L"contain" || bgLength(t, fontSize, unused, true))) size.push_back(t);
        else if (bgLength(t, fontSize, unused, false)) position.push_back(t);
    }
    parseBgPosition(position, fontSize, layer.posX, layer.posY);
    parseBgSize(size, fontSize, layer);
    parseBgRepeat(repeat, layer);
}

// box-shadow: comma-separated [inset] <x> <y> [<blur> [<spread>]] [<color>],
// the colour anywhere; no colour means the text colour.
static std::vector<BoxShadow> parseBoxShadows(const std::wstring& v, int fontSize, const std::wstring& textColor) {
    std::vector<BoxShadow> out;
    if (lowerCase(trimmed(v)) == L"none") return out;
    for (const auto& part : splitTopLevelCommas(v)) {
        BoxShadow s;
        bool hasColor = false;
        std::vector<float> lengths;
        for (const auto& tok : cssTokens(part)) {
            float px;
            if (lowerCase(tok) == L"inset") s.inset = true;
            else if (pixelLength(tok, fontSize, px)) lengths.push_back(px);
            else if (tryParseColor(tok, s.color)) hasColor = true;
        }
        if (lengths.size() < 2) continue; // not a shadow
        s.x = lengths[0];
        s.y = lengths[1];
        s.blur = lengths.size() > 2 ? std::max(lengths[2], 0.0f) : 0;
        s.spread = lengths.size() > 3 ? lengths[3] : 0;
        if (!hasColor) s.color = textColor.empty() ? Color{ 0, 0, 0, 1 } : parseColor(textColor);
        out.push_back(s);
    }
    return out;
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
    const std::wstring& tag = e->tag;
    if (tag == L"a" && e->attrs.count(L"href")) { sv.paint.color = L"#0033cc"; sv.paint.underline = true; }
    if (tag == L"b" || tag == L"strong" || tag == L"th" ||
        (tag.size() == 2 && tag[0] == L'h' && tag[1] >= L'1' && tag[1] <= L'6'))
        sv.paint.bold = true;
    if (tag == L"em" || tag == L"i" || tag == L"cite" || tag == L"var" || tag == L"dfn" || tag == L"address")
        sv.paint.italic = true;
    if (tag == L"code" || tag == L"pre" || tag == L"kbd" || tag == L"samp" || tag == L"tt") {
        static const std::wstring* mono = internFont(L"Consolas");
        sv.paint.family = mono;
    }
    if (tag == L"u" || tag == L"ins") sv.paint.underline = true;
    if (tag == L"s" || tag == L"strike" || tag == L"del") sv.paint.lineThrough = true;
    if (tag == L"center" || tag == L"th") sv.paint.align = TextAlign::Center;

    // line-height as written; resolved once the cascade is done, since a
    // percentage or em is of this element's final font size.
    std::wstring lineHeightRaw;

    // Background layers and shadows, also built once the cascade is done:
    // the `background` shorthand and its longhands override each other in
    // whichever order they come, so each is kept with its place in the
    // cascade (`seq`) and the later one wins, property by property.
    // top/right/bottom/left: auto, a percentage (of the containing block,
    // applied when it's known), or a length (px, em/rem, vh/vw).
    auto parseOffset = [&](const std::wstring& raw, int fontSize) {
        Len out;
        std::wstring v = lowerCase(trimmed(raw));
        double n; std::wstring unit;
        if (v == L"auto" || !parseNumberAndUnit(v, n, unit)) return out;
        out.isAuto = false;
        if (unit == L"%") { out.percent = true; out.value = (float)n; }
        else if (unit == L"em" || unit == L"rem") out.value = (float)(n * fontSize);
        else out.value = (float)resolveLength(v, 0, 0);
        return out;
    };

    struct Recorded { std::wstring value; int seq = -1; };
    Recorded bgShorthand, bgImage, bgSize, bgPosition, bgRepeat;
    int declSeq = 0;
    std::wstring shadowRaw;

    // Sets font-family from a CSS family list; a list naming nothing
    // installed (only web fonts, say) leaves the inherited face.
    auto applyFamily = [&](const std::wstring& list) {
        bool recognized;
        const std::wstring* face = resolveFontFamily(list, recognized);
        if (recognized) sv.paint.family = face;
    };

    auto applyDecl = [&](const std::wstring& k, const std::wstring& v) {
        if (k == L"color") sv.colorSet = true;
        if (k == L"font" || k == L"font-family" || k == L"font-weight" || k == L"font-style") sv.fontSet = true;

        if (k == L"background-color") {
            Color unused;
            if (tryParseColor(v, unused)) sv.background = v;
        }
        else if (k == L"background") {
            // The color comes from the last layer ("url(a.png), #fff");
            // the layers themselves are built after the cascade (see
            // Recorded). "background: none" (or only an image) clears any
            // earlier color.
            Color unused;
            sv.background.clear();
            for (const auto& tok : cssTokens(splitTopLevelCommas(v).back())) {
                if (tryParseColor(tok, unused)) { sv.background = tok; break; }
            }
            bgShorthand = { v, ++declSeq };
        }
        else if (k == L"background-image") bgImage = { v, ++declSeq };
        else if (k == L"background-size") bgSize = { v, ++declSeq };
        else if (k == L"background-position") bgPosition = { v, ++declSeq };
        else if (k == L"background-repeat") bgRepeat = { v, ++declSeq };
        else if (k == L"box-shadow") shadowRaw = v;
        else if (k == L"color") {
            // inherit/currentcolor keep the inherited value; anything that
            // isn't a valid color is ignored, same as a real browser.
            Color unused;
            if (v == L"initial" || v == L"unset") sv.paint.color.clear();
            else if (tryParseColor(v, unused)) sv.paint.color = v;
        }
        else if (k == L"font-style") {
            if (v == L"italic" || v.rfind(L"oblique", 0) == 0) sv.paint.italic = true;
            else if (v == L"normal" || v == L"initial" || v == L"unset") sv.paint.italic = false;
        }
        else if (k == L"font-family") {
            if (v != L"inherit") applyFamily(v);
        }
        else if (k == L"text-align") {
            std::wstring a = lowerCase(trimmed(v));
            if (a == L"center" || a == L"-webkit-center") sv.paint.align = TextAlign::Center;
            else if (a == L"right" || a == L"end" || a == L"-webkit-right") sv.paint.align = TextAlign::Right;
            else if (a == L"left" || a == L"start" || a == L"justify" || a == L"-webkit-left") sv.paint.align = TextAlign::Left;
        }
        else if (k == L"line-height") lineHeightRaw = trimmed(v);
        else if (k == L"text-decoration" || k == L"text-decoration-line") {
            // Only the line part matters here; a value naming no line
            // (just a color or style) leaves it alone.
            bool any = false, underline = false, through = false;
            for (const auto& tok : cssTokens(lowerCase(v))) {
                if (tok == L"underline") { underline = true; any = true; }
                else if (tok == L"line-through") { through = true; any = true; }
                else if (tok == L"none" || tok == L"overline") any = true;
            }
            if (any) { sv.paint.underline = underline; sv.paint.lineThrough = through; }
        }
        else if (k == L"text-transform") {
            std::wstring t = lowerCase(trimmed(v));
            if (t == L"uppercase") sv.paint.transform = TextTransform::Uppercase;
            else if (t == L"lowercase") sv.paint.transform = TextTransform::Lowercase;
            else if (t == L"capitalize") sv.paint.transform = TextTransform::Capitalize;
            else if (t == L"none") sv.paint.transform = TextTransform::None;
        }
        else if (k == L"font") {
            // [style] [variant] [weight] <size>[/<line-height>] <family list>.
            // Everything not given resets to normal, as the shorthand does.
            static const wchar_t* kSizeWords[] = { L"xx-small", L"x-small", L"small", L"medium", L"large",
                                                   L"x-large", L"xx-large", L"smaller", L"larger" };
            std::wstring s = trimmed(v);
            size_t i = 0;
            bool italic = false, bold = false, found = false;
            while (i < s.size()) {
                while (i < s.size() && iswspace(s[i])) i++;
                size_t start = i;
                while (i < s.size() && !iswspace(s[i])) i++;
                std::wstring tok = s.substr(start, i - start);
                std::wstring size = tok.substr(0, tok.find(L'/'));
                double num; std::wstring unit;
                bool isSize = (parseNumberAndUnit(size, num, unit) && !unit.empty()) ||
                              std::any_of(std::begin(kSizeWords), std::end(kSizeWords), [&](const wchar_t* w) { return size == w; });
                if (isSize) {
                    found = true;
                    sv.fontSize = resolveFontSize(size, inheritedFontSize, sv.fontSize);
                    size_t slash = tok.find(L'/');
                    if (slash == std::wstring::npos) {
                        // "16px / 1.5": the line height can follow after spaces.
                        size_t j = i;
                        while (j < s.size() && iswspace(s[j])) j++;
                        if (j < s.size() && s[j] == L'/') {
                            j++;
                            while (j < s.size() && iswspace(s[j])) j++;
                            size_t lhStart = j;
                            while (j < s.size() && !iswspace(s[j])) j++;
                            lineHeightRaw = s.substr(lhStart, j - lhStart);
                            i = j;
                        }
                        else lineHeightRaw = L"normal";
                    }
                    else lineHeightRaw = tok.substr(slash + 1);
                    applyFamily(s.substr(i));
                    break;
                }
                if (tok == L"italic" || tok.rfind(L"oblique", 0) == 0) italic = true;
                else if (tok == L"bold" || tok == L"bolder") bold = true;
                else { try { if (std::stoi(tok) >= 600) bold = true; } catch (...) {} }
            }
            if (found) { sv.paint.italic = italic; sv.paint.bold = bold; }
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
            // Which of left/right is `auto`, by the 1-4 value rule (top,
            // right, bottom, left); auto resolved as 0 above.
            std::vector<std::wstring> t = cssTokens(lowerCase(v));
            if (!t.empty() && t.size() <= 4) {
                static const int kRight[4] = { 0, 1, 1, 1 }, kLeft[4] = { 0, 1, 1, 3 };
                sv.marginRightAuto = t[kRight[t.size() - 1]] == L"auto";
                sv.marginLeftAuto = t[kLeft[t.size() - 1]] == L"auto";
                if (sv.marginRightAuto) sv.marginRight = 0;
                if (sv.marginLeftAuto) sv.marginLeft = 0;
            }
        }
        else if (k == L"margin-top") sv.marginTop = resolveLength(v, containingWidth, 6);
        else if (k == L"margin-right") { sv.marginRightAuto = lowerCase(trimmed(v)) == L"auto"; sv.marginRight = resolveLength(v, containingWidth, 0); }
        else if (k == L"margin-bottom") sv.marginBottom = resolveLength(v, containingWidth, 6);
        else if (k == L"margin-left") { sv.marginLeftAuto = lowerCase(trimmed(v)) == L"auto"; sv.marginLeft = resolveLength(v, containingWidth, 0); }
        else if (k == L"padding") {
            parseBoxShorthand(v, containingWidth, 6, sv.paddingTop, sv.paddingRight, sv.paddingBottom, sv.paddingLeft);
        }
        else if (k == L"padding-top") sv.paddingTop = resolveLength(v, containingWidth, 6);
        else if (k == L"padding-right") sv.paddingRight = resolveLength(v, containingWidth, 6);
        else if (k == L"padding-bottom") sv.paddingBottom = resolveLength(v, containingWidth, 6);
        else if (k == L"padding-left") sv.paddingLeft = resolveLength(v, containingWidth, 6);
        else if (k == L"width") sv.width = resolveLength(v, containingWidth, -1);
        else if (k == L"min-width") sv.minWidth = resolveLength(v, containingWidth, -1);
        else if (k == L"max-width") sv.maxWidth = lowerCase(trimmed(v)) == L"none" ? -1 : resolveLength(v, containingWidth, -1);
        else if (k == L"height") sv.height = resolveHeight(v, sv.fontSize, -1);
        else if (k == L"min-height") sv.minHeight = resolveHeight(v, sv.fontSize, -1);
        else if (k == L"max-height") sv.maxHeight = v == L"none" ? -1 : resolveHeight(v, sv.fontSize, -1);
        else if (k == L"position") {
            std::wstring p = lowerCase(trimmed(v));
            if (p == L"relative") sv.position = ComputedStyle::Position::Relative;
            else if (p == L"absolute") sv.position = ComputedStyle::Position::Absolute;
            else if (p == L"fixed") sv.position = ComputedStyle::Position::Fixed;
            else if (p == L"sticky" || p == L"-webkit-sticky") sv.position = ComputedStyle::Position::Sticky;
            else if (p == L"static") sv.position = ComputedStyle::Position::Static;
        }
        else if (k == L"top" || k == L"right" || k == L"bottom" || k == L"left") {
            Len& side = k == L"top" ? sv.top : k == L"right" ? sv.right : k == L"bottom" ? sv.bottom : sv.left;
            side = parseOffset(v, sv.fontSize);
        }
        else if (k == L"inset") {
            // 1-4 values, like margin: top, right, bottom, left.
            std::vector<std::wstring> t = cssTokens(v);
            if (!t.empty() && t.size() <= 4) {
                static const int kPick[4][4] = { { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };
                Len* sides[4] = { &sv.top, &sv.right, &sv.bottom, &sv.left };
                for (int i = 0; i < 4; i++) *sides[i] = parseOffset(t[kPick[t.size() - 1][i]], sv.fontSize);
            }
        }
        else if (k == L"z-index") {
            if (lowerCase(trimmed(v)) == L"auto") sv.zAuto = true;
            else { try { sv.zIndex = std::stoi(v); sv.zAuto = false; } catch (...) {} }
        }
        else if (k == L"overflow" || k == L"overflow-x" || k == L"overflow-y") {
            // "hidden", or "<x> <y>" for the shorthand. Anything but visible
            // clips (see ComputedStyle::clipX).
            std::vector<std::wstring> t = cssTokens(lowerCase(v));
            if (t.empty()) return;
            auto clips = [](const std::wstring& o) { return o != L"visible"; };
            bool cx = clips(t[0]), cy = clips(t.size() > 1 ? t[1] : t[0]);
            if (k != L"overflow-y") sv.clipX = cx;
            if (k != L"overflow-x") sv.clipY = k == L"overflow" ? cy : cx;
        }
        else if (k == L"box-sizing") {
            if (v == L"border-box") sv.boxSizing = BoxSizing::BorderBox;
            else if (v == L"content-box") sv.boxSizing = BoxSizing::ContentBox;
        }
        else if (k == L"border-radius") {
            // 1-4 values: all corners; tl+br / tr+bl; tl / tr+bl / br; or
            // each of tl tr br bl. An elliptical "/ <vertical radii>" part
            // is dropped - corners are always circular here.
            std::wstring horizontal = v.substr(0, v.find(L'/'));
            std::vector<LayoutBox::CornerRadius> r;
            for (const auto& tok : cssTokens(horizontal)) {
                LayoutBox::CornerRadius c;
                if (!parseRadius(tok, sv.fontSize, c)) { r.clear(); break; }
                r.push_back(c);
            }
            static const int kPick[4][4] = { { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };
            if (!r.empty() && r.size() <= 4)
                for (int i = 0; i < 4; i++) sv.radius[i] = r[kPick[r.size() - 1][i]];
        }
        else if (k == L"border-top-left-radius" || k == L"border-top-right-radius" ||
                 k == L"border-bottom-right-radius" || k == L"border-bottom-left-radius") {
            int corner = k == L"border-top-left-radius" ? 0 : k == L"border-top-right-radius" ? 1
                       : k == L"border-bottom-right-radius" ? 2 : 3;
            auto toks = cssTokens(v);
            LayoutBox::CornerRadius c;
            if (!toks.empty() && parseRadius(toks[0], sv.fontSize, c)) sv.radius[corner] = c;
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

    // Every declaration that applies, in cascade order: matched rules (least
    // to most specific), then inline style.
    std::vector<const std::pair<std::wstring, std::wstring>*> cascade;
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
            for (const auto& decl : rule->declarations) cascade.push_back(&decl);
    }
    auto inlineDecls = CSS::parseDeclarations(getAttr(e, L"style", L""));
    for (const auto& decl : inlineDecls) cascade.push_back(&decl);

    // Custom properties first, so a var() anywhere in this element's
    // declarations sees every one defined on it, wherever in the cascade
    // either appears. A new layer is only made for values that differ from
    // what's inherited (see CSSVars).
    std::shared_ptr<CSSVars> own;
    for (const auto* decl : cascade) {
        const std::wstring& k = decl->first;
        if (k.size() < 3 || k[0] != L'-' || k[1] != L'-') continue;
        if (!own) {
            const std::wstring* inherited = inheritedPaint.vars ? inheritedPaint.vars->find(k) : nullptr;
            if (inherited && *inherited == decl->second) continue;
            own = std::make_shared<CSSVars>();
            own->parent = inheritedPaint.vars;
        }
        own->own[k] = decl->second;
    }
    if (own) {
        // Substitute var() in the values defined here; one that can't be
        // resolved (or is part of a cycle) is dropped, as in CSS.
        std::vector<std::wstring> names;
        for (const auto& [name, value] : own->own) names.push_back(name);
        std::unordered_map<std::wstring, std::wstring> resolved;
        for (const auto& name : names) {
            std::wstring out;
            if (substituteVars(own->own[name], own.get(), out)) resolved[name] = std::move(out);
        }
        own->own = std::move(resolved);
        sv.paint.vars = own;
    }

    for (const auto* decl : cascade) {
        const std::wstring& k = decl->first;
        if (k.size() >= 2 && k[0] == L'-' && k[1] == L'-') continue;
        if (decl->second.find(L"var(") == std::wstring::npos) { applyDecl(k, decl->second); continue; }
        std::wstring value;
        if (substituteVars(decl->second, sv.paint.vars.get(), value)) applyDecl(k, value);
    }

    if (!lineHeightRaw.empty()) {
        // A plain number is inherited as a multiple (each descendant applies
        // it to its own font size); px, em and % become a fixed pixel height
        // here, which is what descendants then inherit - as in CSS.
        double n; std::wstring unit;
        if (lineHeightRaw == L"normal" || lineHeightRaw == L"initial") {
            sv.paint.lineHeight = -1; sv.paint.lineHeightPx = -1;
        }
        else if (parseNumberAndUnit(lineHeightRaw, n, unit) && n >= 0) {
            if (unit.empty()) { sv.paint.lineHeight = (float)n; sv.paint.lineHeightPx = -1; }
            else if (unit == L"px") { sv.paint.lineHeightPx = (int)std::lround(n); sv.paint.lineHeight = -1; }
            else if (unit == L"em" || unit == L"rem") { sv.paint.lineHeightPx = (int)std::lround(n * sv.fontSize); sv.paint.lineHeight = -1; }
            else if (unit == L"%") { sv.paint.lineHeightPx = (int)std::lround(n * sv.fontSize / 100); sv.paint.lineHeight = -1; }
        }
    }

    // Background layers: from the shorthand or background-image, whichever
    // came later; then any size/position/repeat longhand that came after
    // the shorthand, its comma list repeated across the layers as in CSS.
    if (bgShorthand.seq >= 0 || bgImage.seq >= 0) {
        std::vector<BackgroundLayer> layers;
        std::vector<bool> hasImage;
        if (bgShorthand.seq > bgImage.seq) {
            for (const auto& part : splitTopLevelCommas(bgShorthand.value)) {
                BackgroundLayer layer;
                parseBackgroundLayer(part, sv.fontSize, layer);
                hasImage.push_back(!layer.image.empty() || layer.gradient);
                layers.push_back(std::move(layer));
            }
        }
        else {
            for (const auto& part : splitTopLevelCommas(bgImage.value)) {
                BackgroundLayer layer;
                hasImage.push_back(parseBgImage(part, layer)); // "none" holds a place in the list
                layers.push_back(std::move(layer));
            }
        }
        auto applyList = [&](const Recorded& r, auto apply) {
            if (r.seq < 0 || r.seq < bgShorthand.seq || layers.empty()) return;
            std::vector<std::wstring> items = splitTopLevelCommas(r.value);
            for (size_t i = 0; i < layers.size(); i++) apply(cssTokens(lowerCase(items[i % items.size()])), layers[i]);
        };
        applyList(bgSize, [&](const std::vector<std::wstring>& t, BackgroundLayer& l) { parseBgSize(t, sv.fontSize, l); });
        applyList(bgPosition, [&](const std::vector<std::wstring>& t, BackgroundLayer& l) { parseBgPosition(t, sv.fontSize, l.posX, l.posY); });
        applyList(bgRepeat, [&](const std::vector<std::wstring>& t, BackgroundLayer& l) { parseBgRepeat(t, l); });
        for (size_t i = 0; i < layers.size(); i++)
            if (hasImage[i]) sv.backgrounds.push_back(std::move(layers[i]));
    }
    if (!shadowRaw.empty()) sv.shadows = parseBoxShadows(shadowRaw, sv.fontSize, sv.paint.color);

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
    std::copy(std::begin(style.radius), std::end(style.radius), std::begin(box.radius));
    box.backgrounds = style.backgrounds; // gradient buttons
    box.shadows = style.shadows;
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

    // CSS width/height override those defaults (not for a checkbox, whose
    // size is its own). The width includes the control's padding and
    // border, as browsers size most controls (box-sizing: border-box).
    if (box.control != LayoutBox::Checkbox) {
        if (style.width >= 0) box.width = std::min(std::max(style.width, 16), maxWidth);
        if (style.maxWidth >= 0) box.width = std::min(box.width, std::max(style.maxWidth, 16));
        if (style.minWidth >= 0) box.width = std::min(std::max(box.width, style.minWidth), maxWidth);
        if (style.height > 0) box.height = std::max(style.height, style.fontSize + 4);
    }

    // The label's text style - only what the control's own rules set: like
    // browsers, controls don't pick up the page's text colour and font.
    if (style.colorSet) box.color = style.paint.color;
    if (style.fontSet) {
        box.bold = style.paint.bold;
        box.italic = style.paint.italic;
        box.family = style.paint.family;
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
// An <img>, or an inline <svg> (drawn the same way: its markup becomes a
// data: URI the renderer rasterizes - see Svg.h). Size, per dimension:
// CSS width/height, then the width/height attributes (an <svg>'s are part
// of its intrinsic size), then the image's natural size - a dimension that
// is set alone keeps the natural aspect ratio. Until a bitmap has loaded,
// its natural size is unknown and a 200x150 placeholder stands in; an
// SVG's comes straight from its attributes. The box comes back unplaced
// (x/y 0): layoutImage stacks it as a block, layoutInlineRun puts it in a line.
LayoutBox LayoutRoot::makeImageBox(Element* e, int containingWidth, const ComputedStyle& style) {
    const int defaultWidth = 200, defaultHeight = 150;
    bool isSvg = e->tag == L"svg";
    std::wstring src = isSvg ? svgDataUri(e, style.paint.color) : getAttr(e, L"src", L"");

    int naturalW = 0, naturalH = 0;
    bool haveNatural;
    if (isSvg) {
        float w, h;
        svgIntrinsicSize(e->attrs, w, h);
        naturalW = (int)std::lround(w);
        naturalH = (int)std::lround(h);
        haveNatural = true;
    }
    else {
        haveNatural = loadImage && !src.empty() && loadImage(src, naturalW, naturalH);
    }

    int width = style.width, height = style.height;
    if (!isSvg) {
        std::wstring wAttr = getAttr(e, L"width", L""), hAttr = getAttr(e, L"height", L"");
        if (width < 0 && !wAttr.empty()) width = parseFontSize(wAttr, defaultWidth);
        if (height < 0 && !hAttr.empty()) height = parseFontSize(hAttr, defaultHeight);
    }
    if (haveNatural && naturalW > 0 && naturalH > 0) {
        if (width < 0 && height < 0) { width = naturalW; height = naturalH; }
        else if (width < 0) width = (int)std::lround((double)height * naturalW / naturalH);
        else if (height < 0) height = (int)std::lround((double)width * naturalH / naturalW);
    }
    bool heightFollowsWidth = style.height < 0 && (isSvg || getAttr(e, L"height", L"").empty());
    if (width < 0) width = defaultWidth;
    if (height < 0) height = defaultHeight;

    // min-/max-width - img { max-width: 100% } - keeping the image's
    // proportions when its height wasn't set on its own.
    int clamped = width;
    if (style.maxWidth >= 0) clamped = std::min(clamped, style.maxWidth);
    if (style.minWidth >= 0) clamped = std::max(clamped, style.minWidth);
    if (clamped != width && width > 0) {
        if (heightFollowsWidth) height = (int)std::lround((double)height * clamped / width);
        width = clamped;
    }

    LayoutBox box;
    box.x = 0;
    box.y = 0;
    box.fontSize = style.fontSize;
    box.background = style.background;
    box.imageSrc = src;
    std::copy(std::begin(style.radius), std::end(style.radius), std::begin(box.radius)); // rounded avatars etc.
    box.backgrounds = style.backgrounds;
    box.shadows = style.shadows;
    box.width = std::min(width, std::max(containingWidth, 1));
    box.height = height;
    box.el = e;
    box.visuallyHidden = style.visuallyHidden;
    return box;
}

void LayoutRoot::appendImage(Element* e, int containingWidth, const ComputedStyle& style, std::vector<InlineItem>& out) {
    InlineItem item;
    item.fontSize = style.fontSize;
    item.href = currentHref; // an icon inside a link is part of the link
    item.owner = e;
    item.visuallyHidden = style.visuallyHidden;
    item.paint = style.paint;
    item.image = std::make_shared<LayoutBox>(makeImageBox(e, containingWidth, style));
    // Only horizontal margins mean anything inside a line. The 6px every
    // element gets by default (ComputedStyle) is for blocks, not this.
    item.marginLeft = style.marginLeft;
    item.marginRight = style.marginRight;
    item.spaceBefore = false; // a gap before it comes only from whitespace in the HTML (an isSpace marker)
    out.push_back(std::move(item));
}

// An image displayed as a block: on its own, stacked below what came before.
void LayoutRoot::layoutImage(Element* e, int x, int& y, int containingWidth, const ComputedStyle& style) {
    LayoutBox box = makeImageBox(e, containingWidth, style);
    box.x = x;
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

    // Layout starts at <body>, but it inherits from its ancestors - normally
    // just <html>, where `:root { --brand: ... }` and `html { color: ... }`
    // rules land. They're also on ancestorStack, so selectors like
    // "html .x" see them. (Font size still starts at 14px.)
    auto* root = static_cast<Element*>(rootNode);
    usedViewportHeight = false;
    stickies.clear();
    openStickies_.assign(1, {});
    // The viewport is the initial containing block: of height: % on <html>,
    // and of fixed boxes and absolute ones with no positioned ancestor.
    ContainingBlock viewport;
    positioned_.assign(1, &viewport);
    containingHeight_ = viewportHeight;

    std::vector<Element*> ancestors;
    for (Element* a = root->parent; a; a = a->parent) ancestors.insert(ancestors.begin(), a);
    TextPaint paint;
    for (Element* a : ancestors) {
        ComputedStyle s = computeStyle(a, 14, viewportWidth - 20, false, paint);
        paint = s.paint;
        containingHeight_ = s.height; // html { height: 100% } makes body { height: 100% } mean something
        ancestorStack.push_back(a);
    }

    // <body>'s own rules too - body { font-family: ...; color: ...; } is
    // where most pages set their base text style. (Its font-size applies;
    // <html>'s doesn't - the base stays 14px.)
    ComputedStyle bodyStyle = computeStyle(root, 14, viewportWidth - 20, false, paint);
    containingHeight_ = bodyStyle.height;
    if (containingHeight_ >= 0) usedViewportHeight = true; // height: % below can come from the viewport

    int y = 10;
    layoutElement(root, 10, y, viewportWidth - 20, bodyStyle.fontSize, false, bodyStyle.paint);

    // Sticky children of <body> stick until the end of the page.
    for (int i : openStickies_.back())
        stickies[i].maxShift = std::max(y - stickies[i].naturalTop - stickies[i].maxShift, 0);
    openStickies_.clear();

    containingHeight_ = viewportHeight;
    layoutOutOfFlow(viewport, 0, 0, viewportWidth, viewportHeight, true);
    positioned_.clear();

    // A sticky element inside a flex/grid item was laid out at (0, 0) and
    // then moved, so its recorded top is off; its boxes say where it ended
    // up. (maxShift is a distance, which moving doesn't change.)
    std::vector<int> stickyTop(stickies.size(), INT_MAX);
    for (const auto& b : boxes)
        if (b.sticky >= 0 && b.sticky < (int)stickies.size()) stickyTop[b.sticky] = std::min(stickyTop[b.sticky], b.y);
    for (size_t i = 0; i < stickies.size(); i++)
        if (stickyTop[i] != INT_MAX) stickies[i].naturalTop = stickyTop[i];

    // Paint order: z-index (see LayoutBox::paintKey), document order within it.
    std::stable_sort(boxes.begin(), boxes.end(),
                     [](const LayoutBox& a, const LayoutBox& b) { return a.paintKey < b.paintKey; });
}

// --- Height, positioning, overflow ---------------------------------------------

void LayoutRoot::deferOutOfFlow(Element* e, ComputedStyle style, int staticX, int staticY, bool hasStatic) {
    if (positioned_.empty()) return; // only meaningful during layout()
    if (style.display == Display::Inline) style.display = Display::Block; // positioned boxes are blockified
    ContainingBlock* cb =style.position == ComputedStyle::Position::Fixed ? positioned_.front() : positioned_.back();
    OutOfFlow item;
    item.el = e;
    item.style = std::move(style);
    item.staticX = staticX;
    item.staticY = staticY;
    item.hasStatic = hasStatic;
    item.ancestors = ancestorStack;
    item.href = currentHref;
    item.form = currentForm;
    // Flex and grid lay an item out more than once (to measure it first);
    // the last time is where it really is.
    for (auto& p : cb->pending)
        if (p.el == e) { p = std::move(item); return; }
    cb->pending.push_back(std::move(item));
}

// Lays out the absolute/fixed elements waiting on a containing block whose
// padding box (cbX, cbY, cbW, cbH) is now final, as CSS's rules for them:
// - width: its own, or the space between left and right when both are set
//   (and width is auto), or else shrink-to-fit;
// - height: its own, or the space between top and bottom when both are set;
// - x: left, else right (from the right edge), else where it was in the
//   flow; y the same with top/bottom.
// Each is laid out at (0, 0) like a flex item and moved into place; its
// boxes go at the end of the list, painting over the flow.
void LayoutRoot::layoutOutOfFlow(ContainingBlock& cb, int cbX, int cbY, int cbW, int cbH, bool isViewport) {
    if (isViewport && !cb.pending.empty()) usedViewportHeight = true; // placed against the viewport's height
    // By index: laying one out can add more (a fixed element inside it, to
    // the viewport's list).
    for (size_t i = 0; i < cb.pending.size(); i++) {
        OutOfFlow p = cb.pending[i];
        ComputedStyle& s = p.style;
        auto resolve = [](const Len& l, int base) { return l.percent ? (int)std::lround(l.value * base / 100.0) : (int)l.value; };
        int L = resolve(s.left, cbW), R = resolve(s.right, cbW), T = resolve(s.top, cbH), B = resolve(s.bottom, cbH);

        // Its context as it was where it sat in the flow.
        std::swap(ancestorStack, p.ancestors);
        std::wstring savedHref = currentHref;
        Element* savedForm = currentForm;
        currentHref = p.href;
        currentForm = p.form;
        int savedCH = containingHeight_;
        containingHeight_ = cbH;

        if (s.height < 0 && !s.top.isAuto && !s.bottom.isAuto) {
            // Stretched between top and bottom: the border box fills what's left.
            s.boxSizing = BoxSizing::BorderBox;
            s.height = std::max(cbH - T - B - s.marginTop - s.marginBottom, 0);
        }
        // The width it's laid out in: between left and right when both are
        // set (where margin: auto then centres it), else the containing
        // block for a set width, else shrink-to-fit.
        int available;
        if (!s.left.isAuto && !s.right.isAuto) available = std::max(cbW - L - R, 0);
        else if (s.width >= 0) available = cbW;
        else available = shrinkToFitWidth(p.el, s, std::max(cbW - (s.left.isAuto ? 0 : L) - (s.right.isAuto ? 0 : R), 0));
        // Its outer (margin box) width, to place it from the right.
        const int chromeX = s.boxSizing == BoxSizing::BorderBox ? 0 : s.paddingLeft + s.paddingRight + 2 * s.borderWidth;
        int widthValue = s.width >= 0 ? s.width : available - s.marginLeft - s.marginRight - chromeX;
        if (s.maxWidth >= 0) widthValue = std::min(widthValue, s.maxWidth);
        if (s.minWidth >= 0) widthValue = std::max(widthValue, s.minWidth);
        int outer = std::max(widthValue, 0) + chromeX + s.marginLeft + s.marginRight;

        int height = 0;
        std::vector<LayoutBox> placed = layoutItemDetached(p.el, available, s, height);
        int x = !s.left.isAuto ? cbX + L : !s.right.isAuto ? cbX + cbW - R - outer : (p.hasStatic ? p.staticX : cbX);
        int y = !s.top.isAuto ? cbY + T : !s.bottom.isAuto ? cbY + cbH - B - height : (p.hasStatic ? p.staticY : cbY);

        size_t start = boxes.size();
        for (auto& b : placed) {
            b.x += x;
            b.y += y;
            if (b.clipped) { b.clipX += x; b.clipY += y; }
            boxes.push_back(std::move(b));
        }
        applyPaintKey(start, s); // a control or image has no layoutBlockChild of its own to do it
        if (s.position == ComputedStyle::Position::Fixed && isViewport)
            for (size_t j = start; j < boxes.size(); j++) boxes[j].fixed = true;

        containingHeight_ = savedCH;
        currentHref = savedHref;
        currentForm = savedForm;
        std::swap(ancestorStack, p.ancestors);
    }
    cb.pending.clear();
}

void LayoutRoot::applyPaintKey(size_t from, const ComputedStyle& style) {
    if (style.position == ComputedStyle::Position::Static) return;
    int key = style.zAuto ? 1 : 2 * style.zIndex + 1;
    for (size_t i = from; i < boxes.size(); i++) {
        // Boxes still in the flow take this element's key. A positioned
        // descendant keeps its own - unless this element has an explicit
        // z-index, which makes it a stacking context its descendants can't
        // escape (simplified: they all share its key).
        if (boxes[i].paintKey == 0 || !style.zAuto) boxes[i].paintKey = key;
    }
}

void LayoutRoot::clipBoxes(size_t from, size_t to, int x, int y, int w, int h, bool alongX, bool alongY) {
    const int kFar = 1 << 28; // "no limit" along an axis that isn't clipped
    if (!alongX) { x = -kFar; w = 2 * kFar; }
    if (!alongY) { y = -kFar; h = 2 * kFar; }
    for (size_t i = from; i < to && i < boxes.size(); i++) {
        LayoutBox& b = boxes[i];
        if (!b.clipped) {
            b.clipped = true;
            b.clipX = x; b.clipY = y; b.clipW = w; b.clipH = h;
            continue;
        }
        int x1 = std::max(b.clipX, x), y1 = std::max(b.clipY, y);
        int x2 = std::min(b.clipX + b.clipW, x + w), y2 = std::min(b.clipY + b.clipH, y + h);
        b.clipX = x1; b.clipY = y1; b.clipW = std::max(x2 - x1, 0); b.clipH = std::max(y2 - y1, 0);
    }
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
            layoutInlineRun(pendingInline, x, y, containingWidth, inheritedPaint.align);
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

            // Absolute/fixed: out of the flow - laid out once its containing
            // block is (see deferOutOfFlow), from where it would have been.
            if (sv.position == ComputedStyle::Position::Absolute || sv.position == ComputedStyle::Position::Fixed) {
                deferOutOfFlow(e, sv, x, y, true);
                continue;
            }

            if (e->tag == L"input" || e->tag == L"button" || e->tag == L"select") {
                flushInline();
                layoutControl(e, x, y, containingWidth, sv);
                continue;
            }

            if (e->tag == L"img" || e->tag == L"svg") {
                // Inline by default, as in CSS: it joins the text around it
                // ("Click <img> here" stays one line). display:block puts it
                // on its own.
                if (sv.display == Display::Inline) {
                    appendImage(e, containingWidth, sv, pendingInline);
                }
                else {
                    flushInline();
                    layoutImage(e, x, y, containingWidth, sv);
                }
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
    // `width` (and min-/max-width) name the content box, or the border box
    // with box-sizing: border-box; `chromeX` converts between that and the
    // outer (border-box) width.
    const int chromeX = sv.boxSizing == BoxSizing::BorderBox ? 0 : sv.paddingLeft + sv.paddingRight + 2 * sv.borderWidth;
    int widthValue = sv.width >= 0 ? sv.width
                                   : std::max(containingWidth - sv.marginLeft - sv.marginRight - chromeX, 0); // auto: fill the container
    if (sv.maxWidth >= 0) widthValue = std::min(widthValue, sv.maxWidth);
    if (sv.minWidth >= 0) widthValue = std::max(widthValue, sv.minWidth);
    int outerWidth = widthValue + chromeX;
    int contentWidth = std::max(outerWidth - sv.paddingLeft - sv.paddingRight - 2 * sv.borderWidth, 0);

    // margin: auto - what's left of the container goes to the auto margin(s):
    // both centres the box (margin: 0 auto), one pushes it to the other side.
    int marginLeft = sv.marginLeft;
    if (sv.marginLeftAuto || sv.marginRightAuto) {
        int free = std::max(containingWidth - outerWidth - (sv.marginLeftAuto ? 0 : sv.marginLeft) -
                            (sv.marginRightAuto ? 0 : sv.marginRight), 0);
        if (sv.marginLeftAuto && sv.marginRightAuto) marginLeft = free / 2;
        else if (sv.marginLeftAuto) marginLeft = free;
    }
    int boxX = x + marginLeft;

    y += sv.marginTop;
    int contentStartY = y;
    const size_t firstBox = boxes.size(); // this element's boxes are [firstBox, end)

    // Reserve a background/border box now (before laying out children) so
    // it paints behind them, but only if this element actually declared
    // one — plain structural wrappers like <html>/<body> get no box at
    // all, they just position their children.
    size_t bgIndex = static_cast<size_t>(-1);
    if (!sv.background.empty() || sv.borderWidth > 0 || !sv.backgrounds.empty() || !sv.shadows.empty()) {
        LayoutBox box;
        box.x = boxX;
        box.y = contentStartY;
        box.width = outerWidth;
        box.height = 0; // filled in below once children are laid out
        box.background = sv.background;
        box.borderWidth = sv.borderWidth;
        box.borderColor = sv.borderColor;
        std::copy(std::begin(sv.radius), std::end(sv.radius), std::begin(box.radius));
        box.backgrounds = sv.backgrounds;
        box.shadows = sv.shadows;
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

    // An explicit height, turned into a content height: what this element's
    // own children resolve height: % against (definite), and its size
    // regardless of them. min-/max-height convert the same way.
    const int chrome = sv.paddingTop + sv.paddingBottom + 2 * sv.borderWidth;
    auto toContent = [&](int h) { return h < 0 ? -1 : std::max(sv.boxSizing == BoxSizing::BorderBox ? h - chrome : h, 0); };
    const int fixedHeight = toContent(sv.height);
    int savedCH = containingHeight_;
    containingHeight_ = fixedHeight;

    // A positioned element is the containing block of the absolute
    // elements inside it, laid out below once its own size is final.
    const bool positioned = sv.position != ComputedStyle::Position::Static;
    ContainingBlock containing;
    if (positioned) positioned_.push_back(&containing);
    openStickies_.emplace_back(); // sticky children: their limit is this element's content box

    int childX = boxX + sv.borderWidth + sv.paddingLeft;
    const int contentTop = y;
    if (sv.display == Display::Grid) layoutGrid(e, childX, y, contentWidth, sv);
    else if (sv.display == Display::Flex) layoutFlex(e, childX, y, contentWidth, sv);
    else layoutElement(e, childX, y, contentWidth, sv.fontSize, sv.visuallyHidden, sv.paint);

    currentHref = savedHref;
    currentForm = savedForm;
    containingHeight_ = savedCH;

    // The content height: its own if set, else its children's; then
    // min-/max-height. Content taller than a set height overflows - drawn
    // anyway unless overflow clips it (below) - but doesn't push what
    // follows further down.
    int contentHeight = fixedHeight >= 0 ? fixedHeight : y - contentTop;
    if (sv.minHeight >= 0) contentHeight = std::max(contentHeight, toContent(sv.minHeight));
    if (sv.maxHeight >= 0) contentHeight = std::min(contentHeight, toContent(sv.maxHeight));
    y = contentTop + contentHeight;

    for (int i : openStickies_.back()) // maxShift held the sticky element's height until now
        stickies[i].maxShift = std::max(y - stickies[i].naturalTop - stickies[i].maxShift, 0);
    openStickies_.pop_back();

    y += sv.paddingBottom + sv.borderWidth;
    const int borderHeight = y - contentStartY;
    if (bgIndex != static_cast<size_t>(-1)) boxes[bgIndex].height = borderHeight;

    // Its absolute descendants, against its padding box.
    if (positioned) {
        positioned_.pop_back();
        layoutOutOfFlow(containing, boxX + sv.borderWidth, contentStartY + sv.borderWidth,
                        outerWidth - 2 * sv.borderWidth, borderHeight - 2 * sv.borderWidth, false);
    }

    // overflow: its descendants' boxes (not its own) clipped to its padding box.
    if (sv.clipX || sv.clipY) {
        size_t from = bgIndex != static_cast<size_t>(-1) ? bgIndex + 1 : firstBox;
        clipBoxes(from, boxes.size(), boxX + sv.borderWidth, contentStartY + sv.borderWidth,
                  outerWidth - 2 * sv.borderWidth, borderHeight - 2 * sv.borderWidth, sv.clipX, sv.clipY);
    }

    // position: relative - drawn moved by its offsets, but takes up its
    // place in the flow as if it weren't. (Percentages: of the containing
    // block's width, and of its height when that's definite.)
    if (sv.position == ComputedStyle::Position::Relative) {
        auto resolve = [](const Len& l, int base) { return l.percent ? (int)std::lround(l.value * std::max(base, 0) / 100.0) : (int)l.value; };
        int dx = !sv.left.isAuto ? resolve(sv.left, containingWidth) : !sv.right.isAuto ? -resolve(sv.right, containingWidth) : 0;
        int dy = !sv.top.isAuto ? resolve(sv.top, containingHeight_) : !sv.bottom.isAuto ? -resolve(sv.bottom, containingHeight_) : 0;
        if (dx || dy) {
            for (size_t i = firstBox; i < boxes.size(); i++) {
                boxes[i].x += dx; boxes[i].y += dy;
                if (boxes[i].clipped) { boxes[i].clipX += dx; boxes[i].clipY += dy; }
            }
        }
    }

    // position: sticky - in the flow, but shifted down while scrolling so
    // its top stays `top` from the viewport's, until it meets the bottom of
    // its parent (see Engine::boxShift). Only `top` is supported.
    if (sv.position == ComputedStyle::Position::Sticky && !sv.top.isAuto) {
        Sticky st;
        st.top = sv.top.percent ? (int)std::lround(sv.top.value * viewportHeight / 100.0) : (int)sv.top.value;
        st.naturalTop = contentStartY;
        st.maxShift = borderHeight; // until the parent ends - see above
        int index = (int)stickies.size();
        stickies.push_back(st);
        if (!openStickies_.empty()) openStickies_.back().push_back(index);
        for (size_t i = firstBox; i < boxes.size(); i++)
            if (boxes[i].sticky < 0) boxes[i].sticky = index;
    }

    applyPaintKey(firstBox, sv);
    y += sv.marginBottom;
}

// Moves a box laid out somewhere else (a flex/grid item laid out at 0,0)
// into place - its overflow clip with it.
static void moveBox(LayoutBox& b, int dx, int dy) {
    b.x += dx;
    b.y += dy;
    if (b.clipped) { b.clipX += dx; b.clipY += dy; }
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
        if (cs.position == ComputedStyle::Position::Absolute || cs.position == ComputedStyle::Position::Fixed) {
            deferOutOfFlow(ce, cs, x, y, true); // not an item: out of the flow
            continue;
        }

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
        } else if (p.el->tag == L"img" || p.el->tag == L"svg") {
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
            moveBox(b, colX[p.colStart], rowY[p.rowStart]);
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
        if (cs.position == ComputedStyle::Position::Absolute || cs.position == ComputedStyle::Position::Fixed) {
            deferOutOfFlow(ce, cs, x, y, true); // not an item: out of the flow
            continue;
        }
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
                moveBox(b, x + crossOffset, cursorY);
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
    // With auto margins in play, items without a width are sized to their
    // content (as CSS always does) rather than this engine's usual implicit
    // flex-grow - otherwise they'd fill the row and leave the margins nothing.
    bool autoMarginsUsed = false;
    for (const auto& s : itemStyles) autoMarginsUsed |= s.marginLeftAuto || s.marginRightAuto;
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
        } else if (wraps || autoMarginsUsed) {
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
            // min-/max-width have the last word (the space a clamped item
            // gives up isn't handed on to the others - a simplification).
            const ComputedStyle& s = itemStyles[i];
            int extra = (s.boxSizing == BoxSizing::BorderBox ? 0 : s.paddingLeft + s.paddingRight + 2 * s.borderWidth) +
                        s.marginLeft + s.marginRight;
            if (s.maxWidth >= 0) itemWidths[i] = std::min(itemWidths[i], s.maxWidth + extra);
            if (s.minWidth >= 0) itemWidths[i] = std::max(itemWidths[i], s.minWidth + extra);
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
            // A form control doesn't fill the width it's given the way a
            // block does - it takes `width` as its own size. One with a CSS
            // width gets the flexed width that way; one without keeps its
            // natural size, as in browsers.
            bool isControl = items[i]->tag == L"input" || items[i]->tag == L"button" || items[i]->tag == L"select";
            if (isControl && itemStyles[i].width >= 0)
                real.width = std::max(itemWidths[i] - real.marginLeft - real.marginRight, 0);
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

        // Auto margins on items take the leftover space first, split evenly
        // between them - margin-left: auto pushes an item (and those after
        // it) to the end - and justify-content then has nothing left to do.
        int autoMargins = 0;
        for (int i = a; i < b; i++) autoMargins += itemStyles[i].marginLeftAuto + itemStyles[i].marginRightAuto;
        int perAutoMargin = 0;
        if (autoMargins > 0) {
            startX = x;
            extraGap = 0;
            perAutoMargin = leftover / autoMargins;
        }

        int cursorX = startX;
        for (int i = a; i < b; i++) {
            int k = i - a;
            if (itemStyles[i].marginLeftAuto) cursorX += perAutoMargin;
            int itemY = 0;
            if (style.alignItems == AlignItems::Center) itemY = (lineHeight - itemHeights[k]) / 2;
            else if (style.alignItems == AlignItems::FlexEnd) itemY = lineHeight - itemHeights[k];
            // FlexStart and Stretch both start at the line's top; stretch is
            // approximated by extending the item's own background/border box
            // (if it made one) to the line's height, rather than by
            // re-flowing its content into the extra space - a box with no
            // background/border has nothing visible to stretch anyway.
            // Only an item whose height is auto stretches, as in CSS.
            if (style.alignItems == AlignItems::Stretch && itemStyles[i].height < 0) {
                for (auto& box : itemBoxes[k]) if (box.el == items[i]) { box.height = lineHeight; break; }
            }

            for (LayoutBox box : itemBoxes[k]) {
                moveBox(box, cursorX, lineY + itemY);
                boxes.push_back(std::move(box));
            }
            cursorX += itemWidths[i] + colGap + extraGap;
            if (itemStyles[i].marginRightAuto) cursorX += perAutoMargin;
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
    else if (item->tag == L"img" || item->tag == L"svg")
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
