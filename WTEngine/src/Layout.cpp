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
    if (unit == L"em") return (int)std::lround(value * emBase_);
    if (unit == L"rem") return (int)std::lround(value * 14); // the root font size - see layout()
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
    if (!measureText) return text.size() * fontSize * 0.55f; // rough fallback
    auto& widths = textCache_[FontKey{ family, fontSize, bold, italic }];
    auto it = widths.find(text);
    if (it != widths.end()) return it->second;
    float w = measureText(text, fontSize, bold, italic, family);
    // Bounded: a page with endless distinct text just starts over.
    if (++textCacheSize_ > 200000) { textCache_.clear(); textCacheSize_ = 1; }
    textCache_[FontKey{ family, fontSize, bold, italic }].emplace(text, w);
    return w;
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
        if (sv.floatSide != ComputedStyle::Float::None) {
            out.push_back(makeFloatItem(e, sv));
            continue;
        }

        // A block-level element (<a><div>...</div></a>) or a form control
        // inside inline content: the run is split around it, which lays it
        // out on lines of its own (InlineItem::blockItem) - the same a
        // control directly in a block gets.
        const bool control = e->tag == L"input" || e->tag == L"button" || e->tag == L"select";
        const bool image = e->tag == L"img" || e->tag == L"svg";
        if (control || (sv.display != Display::Inline && !image)) {
            out.push_back(makeFloatItem(e, sv, true));
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
    // While measuring a table cell's min-content width, a line can be as
    // narrow as one word (see measuring_).
    const float fullWidth = (float)std::max(containingWidth - 2 * textInset, measuring_ ? 0 : 40);

    struct Placed { InlineItem item; float offset; float width; };
    std::vector<Placed> line;
    float lineWidth = 0;

    // The current line's left edge and width - the block's, unless floats
    // beside it take some (openLine). text-align aligns within it.
    int lineX = x;
    float maxWidth = fullWidth;
    float alignWidth = (float)std::max(containingWidth - 2 * textInset, 0);
    // Floats met mid-line, placed below it once it's done.
    std::vector<std::shared_ptr<FloatItem>> deferredFloats;

    // Starts a line at `y` whose first item is `firstWidth` wide and about
    // `height` tall: beside the floats there, or - if it doesn't fit there -
    // lower down, past them.
    auto openLine = [&](float firstWidth, int height) {
        lineX = x;
        maxWidth = fullWidth;
        alignWidth = (float)std::max(containingWidth - 2 * textInset, 0);
        if (!floatCtx_ || floatCtx_->floats.empty()) return;
        for (;;) {
            int left, right, next;
            if (!spaceBeside(y, height, x, containingWidth, left, right, next)) return; // no float here
            float avail = (float)std::max(right - left - 2 * textInset, 0);
            if (avail >= firstWidth || next <= y) {
                lineX = left;
                maxWidth = alignWidth = avail;
                return;
            }
            y = next;
        }
    };

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

        // text-align: the whole line moves by its leftover width - of the
        // real width, not the 40px lines wrap at in narrower ones (a narrow
        // table column's right-aligned text would otherwise end past it).
        float shift = 0;
        if (measuring_) {} // measured from the left edge
        else if (align == TextAlign::Center) shift = std::max(alignWidth - lineWidth, 0.0f) / 2;
        else if (align == TextAlign::Right) shift = std::max(alignWidth - lineWidth, 0.0f);

        for (size_t i = 0; i < line.size(); i++) {
            const Placed& p = line[i];
            int left = lineX + (int)std::lround(p.offset + shift);
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
        for (const auto& f : deferredFloats) placeFloat(*f, x, containingWidth, y);
        deferredFloats.clear();
    };

    // A run of nothing but whitespace (between two blocks, say) takes no
    // space - though floats in it are still placed, where it is.
    if (std::none_of(items.begin(), items.end(), [](const InlineItem& i) { return !i.isSpace && !i.floatItem; })) {
        for (const auto& i : items)
            if (i.floatItem) placeFloat(*i.floatItem, x, containingWidth, y);
        return;
    }

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
        if (raw.floatItem) {
            if (line.empty()) placeFloat(*raw.floatItem, x, containingWidth, y);
            else deferredFloats.push_back(raw.floatItem);
            continue;
        }
        if (raw.blockItem) {
            // The line so far ends here; the block takes the lines below it.
            emitLine();
            pendingSpace = false;
            layoutInlineBlock(*raw.blockItem, x, y, containingWidth);
            continue;
        }

        if (raw.image) {
            // Wraps like a word; never split.
            float w = (float)(raw.marginLeft + raw.image->width + raw.marginRight);
            float spaceWidth = gapBefore(raw);
            if (!line.empty() && lineWidth + spaceWidth + w > maxWidth) emitLine();
            if (line.empty()) openLine(w, raw.image->height);
            float offset = line.empty() ? 0 : lineWidth + spaceWidth;
            lineWidth = offset + w;
            line.push_back({ raw, offset, w });
            continue;
        }
        if (raw.word.empty()) continue;

        InlineItem item = raw;
        float wordWidth = textWidth(item.word, item.fontSize, item.paint);

        // A word wider than the whole block gets broken by characters, one
        // fragment per line (below any floats), before whatever's left of
        // it (now short enough) falls through to the normal wrapping below.
        const int band = lineBand(item.paint, item.fontSize);
        while (!measuring_ && wordWidth > fullWidth && item.word.size() > 1) {
            if (!line.empty()) emitLine();
            openLine(wordWidth, band);
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
        if (line.empty()) openLine(wordWidth, band);

        float offset = line.empty() ? 0 : lineWidth + spaceWidth;
        lineWidth = offset + wordWidth;
        line.push_back({ item, offset, wordWidth });
    }
    emitLine();
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
// The CSS that a table part's old presentational attributes stand for, as a
// browser maps them: width/height, bgcolor, align (text-align - centring
// nested blocks too, as <center> does - or for the table itself, centring
// it), valign, and on cells, the enclosing table's cellpadding and border
// (any border on the table gives each cell a 1px one). Also a <div>'s
// align, and the attributes that float or clear: <img>/<table>
// align=left|right, <br clear>. Empty for every other element.
static std::vector<std::pair<std::wstring, std::wstring>> tableHints(Element* e) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    const std::wstring& tag = e->tag;
    const bool cell = tag == L"td" || tag == L"th";
    if (tag == L"img" || tag == L"br") {
        // <img align=left|right> floats it; <br clear=all|left|right>
        // clears floats.
        auto a = e->attrs.find(tag == L"img" ? L"align" : L"clear");
        if (a == e->attrs.end()) return out;
        std::wstring v = lowerCase(trimmed(a->second));
        if (tag == L"img" && (v == L"left" || v == L"right")) out.push_back({ L"float", v });
        if (tag == L"br" && (v == L"left" || v == L"right" || v == L"all" || v == L"both")) out.push_back({ L"clear", v });
        return out;
    }
    if (tag == L"div") {
        auto a = e->attrs.find(L"align");
        if (a != e->attrs.end()) {
            std::wstring v = lowerCase(trimmed(a->second));
            if (v == L"center") out.push_back({ L"text-align", L"-webkit-center" });
            else if (v == L"left" || v == L"right") out.push_back({ L"text-align", v });
        }
        return out;
    }
    if (!cell && tag != L"table" && tag != L"tr" && tag != L"thead" && tag != L"tbody" && tag != L"tfoot" &&
        tag != L"col" && tag != L"colgroup")
        return out;
    auto attr = [&](Element* el, const wchar_t* name) -> const std::wstring* {
        auto it = el->attrs.find(name);
        return it != el->attrs.end() ? &it->second : nullptr;
    };
    // border="" and border="1" both mean 1px; border="0" means none.
    auto borderWidth = [&](Element* el) {
        const std::wstring* b = attr(el, L"border");
        if (!b) return -1;
        try { return b->empty() ? 1 : std::max(std::stoi(*b), 0); } catch (...) { return 1; }
    };

    if (auto* w = attr(e, L"width")) if (tag != L"tr" && !w->empty()) out.push_back({ L"width", *w });
    if (auto* h = attr(e, L"height")) if (tag != L"col" && tag != L"colgroup" && !h->empty()) out.push_back({ L"height", *h });
    if (auto* bg = attr(e, L"bgcolor")) if (!bg->empty()) out.push_back({ L"background-color", *bg });
    if (auto* a = attr(e, L"align")) {
        std::wstring v = lowerCase(trimmed(*a));
        if (tag == L"table") {
            if (v == L"center") { out.push_back({ L"margin-left", L"auto" }); out.push_back({ L"margin-right", L"auto" }); }
            else if (v == L"left" || v == L"right") out.push_back({ L"float", v });
        }
        else if (v == L"center") out.push_back({ L"text-align", L"-webkit-center" });
        else if (v == L"left" || v == L"right") out.push_back({ L"text-align", v });
    }
    if (auto* va = attr(e, L"valign")) if (!va->empty()) out.push_back({ L"vertical-align", lowerCase(trimmed(*va)) });

    if (tag == L"table") {
        int bw = borderWidth(e);
        if (bw > 0) out.push_back({ L"border", std::to_wstring(bw) + L"px solid #808080" });
        if (auto* cs = attr(e, L"cellspacing")) if (!cs->empty()) out.push_back({ L"border-spacing", *cs + L"px" });
    }
    if (cell) {
        Element* table = e->parent;
        while (table && table->tag != L"table") table = table->parent;
        if (table) {
            if (auto* cp = attr(table, L"cellpadding")) if (!cp->empty()) out.push_back({ L"padding", *cp + L"px" });
            if (borderWidth(table) > 0) out.push_back({ L"border", L"1px solid #808080" });
        }
    }
    return out;
}

LayoutRoot::ComputedStyle LayoutRoot::computeStyle(Element* e, int inheritedFontSize, int containingWidth,
                                                     bool inheritedVisuallyHidden, const TextPaint& inheritedPaint) {
    ComputedStyle sv;
    sv.fontSize = inheritedFontSize; // inherited unless a rule below overrides it
    sv.display = isInlineTag(e->tag) ? Display::Inline : Display::Block;
    const std::wstring& tag = e->tag;

    // Table parts: their display, and browser-like spacing - cells get 1px
    // of padding (the table's border-spacing separates them), rows and row
    // groups take none, as margins and padding don't apply to them.
    if (tag == L"table") { sv.display = Display::Table; sv.paddingTop = sv.paddingRight = sv.paddingBottom = sv.paddingLeft = 0; }
    else if (tag == L"td" || tag == L"th" || tag == L"tr" || tag == L"tbody" || tag == L"thead" ||
             tag == L"tfoot" || tag == L"caption") {
        sv.display = tag == L"td" || tag == L"th" ? Display::TableCell : tag == L"tr" ? Display::TableRow
                   : tag == L"thead" ? Display::TableHeaderGroup : tag == L"tfoot" ? Display::TableFooterGroup
                   : tag == L"tbody" ? Display::TableRowGroup : Display::TableCaption;
        int pad = tag == L"td" || tag == L"th" ? 1 : tag == L"caption" ? 2 : 0;
        sv.marginTop = sv.marginBottom = 0;
        sv.paddingTop = sv.paddingRight = sv.paddingBottom = sv.paddingLeft = pad;
    }
    else if (tag == L"col" || tag == L"colgroup") sv.display = Display::None; // read by buildTable for widths only

    // The rest of a browser's built-in spacing and sizes. Vertical margins
    // are in em of the element's final font size, so they're resolved once
    // the cascade is done - unless an author rule set them (uaMarginEm).
    // A list inside a list has none, as in browsers.
    const bool heading = tag.size() == 2 && tag[0] == L'h' && tag[1] >= L'1' && tag[1] <= L'6';
    const bool list = tag == L"ul" || tag == L"ol" || tag == L"menu" || tag == L"dir";
    float uaMarginEm = 0;
    if (tag == L"p" || tag == L"dl" || tag == L"pre" || tag == L"blockquote" || tag == L"figure") uaMarginEm = 1;
    else if (tag == L"hr") uaMarginEm = 0.5f;
    else if (list) {
        uaMarginEm = 1;
        for (Element* a = e->parent; a; a = a->parent)
            if (a->tag == L"ul" || a->tag == L"ol" || a->tag == L"menu" || a->tag == L"dir") { uaMarginEm = 0; break; }
    }
    if (heading) {
        static const float kSize[6] = { 2, 1.5f, 1.17f, 1, 0.83f, 0.67f };
        static const float kMargin[6] = { 0.67f, 0.83f, 1, 1.33f, 1.67f, 2.33f };
        int level = tag[1] - L'1';
        sv.fontSize = (int)std::lround(inheritedFontSize * kSize[level]);
        uaMarginEm = kMargin[level];
    }
    if (list) sv.paddingLeft = 40;
    if (tag == L"dd") sv.marginLeft = 40;
    if (tag == L"blockquote" || tag == L"figure") sv.marginLeft = sv.marginRight = 40;
    if (tag == L"hr") { sv.borderWidth = 1; sv.borderColor = L"#9a9a9a"; }
    bool marginTopSet = false, marginBottomSet = false; // by an author rule, over uaMarginEm

    // color/font-weight: inherited, then the tag's own default (what a
    // browser's built-in stylesheet would give it), then author rules.
    sv.paint = inheritedPaint;
    sv.centeredByParent = inheritedPaint.centerBlocks;
    if (tag == L"table" && quirks) {
        // Quirks mode: a table starts its text styling afresh.
        sv.fontSize = 14;
        sv.paint.align = TextAlign::Left;
        sv.paint.centerBlocks = false;
        sv.paint.bold = sv.paint.italic = false;
    }
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
    if (tag == L"center") sv.paint.centerBlocks = true;
    else if (tag == L"th") sv.paint.centerBlocks = false;

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
        // The property, identified once - then compared as a number down the
        // chain below rather than as a string. Most pages also declare plenty
        // this engine doesn't support (transition, cursor, ...): those stop
        // at the lookup.
        enum Prop {
            P_align_items, P_background, P_background_color, P_background_image, P_background_position,
            P_background_repeat, P_background_size, P_border, P_border_bottom_left_radius,
            P_border_bottom_right_radius, P_border_collapse, P_border_color, P_border_radius,
            P_border_spacing, P_border_top_left_radius, P_border_top_right_radius, P_border_width,
            P_bottom, P_box_shadow, P_box_sizing, P_clear, P_color, P_column_gap, P_display, P_flex,
            P_flex_basis, P_flex_direction, P_flex_flow, P_flex_grow, P_flex_shrink, P_flex_wrap, P_float,
            P_font, P_font_family, P_font_size, P_font_style, P_font_weight, P_gap, P_grid_area,
            P_grid_column, P_grid_column_end, P_grid_column_start, P_grid_gap, P_grid_row, P_grid_row_end,
            P_grid_row_start, P_grid_template, P_grid_template_areas, P_grid_template_columns,
            P_grid_template_rows, P_height, P_inset, P_justify_content, P_left, P_line_height, P_margin,
            P_margin_bottom, P_margin_left, P_margin_right, P_margin_top, P_max_height, P_max_width,
            P_min_height, P_min_width, P_opacity, P_overflow, P_overflow_x, P_overflow_y, P_padding,
            P_padding_bottom, P_padding_left, P_padding_right, P_padding_top, P_position, P_right,
            P_row_gap, P_text_align, P_text_decoration, P_text_decoration_line, P_text_transform, P_top,
            P_vertical_align, P_visibility, P_width, P_z_index,
        };
        static const std::unordered_map<std::wstring, int> kProps = {
            { L"align-items", P_align_items }, { L"background", P_background },
            { L"background-color", P_background_color }, { L"background-image", P_background_image },
            { L"background-position", P_background_position }, { L"background-repeat", P_background_repeat },
            { L"background-size", P_background_size }, { L"border", P_border },
            { L"border-bottom-left-radius", P_border_bottom_left_radius },
            { L"border-bottom-right-radius", P_border_bottom_right_radius },
            { L"border-collapse", P_border_collapse }, { L"border-color", P_border_color },
            { L"border-radius", P_border_radius }, { L"border-spacing", P_border_spacing },
            { L"border-top-left-radius", P_border_top_left_radius },
            { L"border-top-right-radius", P_border_top_right_radius }, { L"border-width", P_border_width },
            { L"bottom", P_bottom }, { L"box-shadow", P_box_shadow }, { L"box-sizing", P_box_sizing },
            { L"clear", P_clear }, { L"color", P_color }, { L"column-gap", P_column_gap },
            { L"display", P_display }, { L"flex", P_flex }, { L"flex-basis", P_flex_basis },
            { L"flex-direction", P_flex_direction }, { L"flex-flow", P_flex_flow },
            { L"flex-grow", P_flex_grow }, { L"flex-shrink", P_flex_shrink }, { L"flex-wrap", P_flex_wrap },
            { L"float", P_float }, { L"font", P_font }, { L"font-family", P_font_family },
            { L"font-size", P_font_size }, { L"font-style", P_font_style }, { L"font-weight", P_font_weight },
            { L"gap", P_gap }, { L"grid-area", P_grid_area }, { L"grid-column", P_grid_column },
            { L"grid-column-end", P_grid_column_end }, { L"grid-column-start", P_grid_column_start },
            { L"grid-gap", P_grid_gap }, { L"grid-row", P_grid_row }, { L"grid-row-end", P_grid_row_end },
            { L"grid-row-start", P_grid_row_start }, { L"grid-template", P_grid_template },
            { L"grid-template-areas", P_grid_template_areas },
            { L"grid-template-columns", P_grid_template_columns },
            { L"grid-template-rows", P_grid_template_rows }, { L"height", P_height }, { L"inset", P_inset },
            { L"justify-content", P_justify_content }, { L"left", P_left }, { L"line-height", P_line_height },
            { L"margin", P_margin }, { L"margin-bottom", P_margin_bottom }, { L"margin-left", P_margin_left },
            { L"margin-right", P_margin_right }, { L"margin-top", P_margin_top },
            { L"max-height", P_max_height }, { L"max-width", P_max_width }, { L"min-height", P_min_height },
            { L"min-width", P_min_width }, { L"opacity", P_opacity }, { L"overflow", P_overflow },
            { L"overflow-x", P_overflow_x }, { L"overflow-y", P_overflow_y }, { L"padding", P_padding },
            { L"padding-bottom", P_padding_bottom }, { L"padding-left", P_padding_left },
            { L"padding-right", P_padding_right }, { L"padding-top", P_padding_top },
            { L"position", P_position }, { L"right", P_right }, { L"row-gap", P_row_gap },
            { L"text-align", P_text_align }, { L"text-decoration", P_text_decoration },
            { L"text-decoration-line", P_text_decoration_line }, { L"text-transform", P_text_transform },
            { L"top", P_top }, { L"vertical-align", P_vertical_align }, { L"visibility", P_visibility },
            { L"width", P_width }, { L"z-index", P_z_index },
        };
        auto found = kProps.find(k);
        if (found == kProps.end()) return;
        const int prop = found->second;
        emBase_ = sv.fontSize; // what em resolves against in lengths below (resolveLength)
        if (prop == P_color) sv.colorSet = true;
        if (prop == P_font || prop == P_font_family || prop == P_font_weight || prop == P_font_style) sv.fontSet = true;

        if (prop == P_background_color) {
            Color unused;
            if (tryParseColor(v, unused)) sv.background = v;
        }
        else if (prop == P_background) {
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
        else if (prop == P_background_image) bgImage = { v, ++declSeq };
        else if (prop == P_background_size) bgSize = { v, ++declSeq };
        else if (prop == P_background_position) bgPosition = { v, ++declSeq };
        else if (prop == P_background_repeat) bgRepeat = { v, ++declSeq };
        else if (prop == P_box_shadow) shadowRaw = v;
        else if (prop == P_color) {
            // inherit/currentcolor keep the inherited value; anything that
            // isn't a valid color is ignored, same as a real browser.
            Color unused;
            if (v == L"initial" || v == L"unset") sv.paint.color.clear();
            else if (tryParseColor(v, unused)) sv.paint.color = v;
        }
        else if (prop == P_font_style) {
            if (v == L"italic" || v.rfind(L"oblique", 0) == 0) sv.paint.italic = true;
            else if (v == L"normal" || v == L"initial" || v == L"unset") sv.paint.italic = false;
        }
        else if (prop == P_font_family) {
            if (v != L"inherit") applyFamily(v);
        }
        else if (prop == P_text_align) {
            std::wstring a = lowerCase(trimmed(v));
            bool known = true;
            if (a == L"center" || a == L"-webkit-center") sv.paint.align = TextAlign::Center;
            else if (a == L"right" || a == L"end" || a == L"-webkit-right") sv.paint.align = TextAlign::Right;
            else if (a == L"left" || a == L"start" || a == L"justify" || a == L"-webkit-left") sv.paint.align = TextAlign::Left;
            else known = false;
            if (known) sv.paint.centerBlocks = a == L"-webkit-center";
        }
        else if (prop == P_line_height) lineHeightRaw = trimmed(v);
        else if (prop == P_text_decoration || prop == P_text_decoration_line) {
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
        else if (prop == P_text_transform) {
            std::wstring t = lowerCase(trimmed(v));
            if (t == L"uppercase") sv.paint.transform = TextTransform::Uppercase;
            else if (t == L"lowercase") sv.paint.transform = TextTransform::Lowercase;
            else if (t == L"capitalize") sv.paint.transform = TextTransform::Capitalize;
            else if (t == L"none") sv.paint.transform = TextTransform::None;
        }
        else if (prop == P_font) {
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
        else if (prop == P_font_weight) {
            if (v == L"bold" || v == L"bolder") sv.paint.bold = true;
            else if (v == L"normal" || v == L"lighter" || v == L"initial" || v == L"unset") sv.paint.bold = false;
            else {
                // Numeric weight: 600 and up is what GDI (and browsers,
                // with only a regular/bold pair of faces) render as bold.
                try { sv.paint.bold = std::stoi(v) >= 600; } catch (...) {}
            }
        }
        else if (prop == P_margin) {
            parseBoxShorthand(v, containingWidth, 0, sv.marginTop, sv.marginRight, sv.marginBottom, sv.marginLeft);
            marginTopSet = marginBottomSet = true;
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
        else if (prop == P_margin_top) { sv.marginTop = resolveLength(v, containingWidth, sv.marginTop); marginTopSet = true; }
        else if (prop == P_margin_right) { sv.marginRightAuto = lowerCase(trimmed(v)) == L"auto"; sv.marginRight = resolveLength(v, containingWidth, sv.marginRightAuto ? 0 : sv.marginRight); }
        else if (prop == P_margin_bottom) { sv.marginBottom = resolveLength(v, containingWidth, sv.marginBottom); marginBottomSet = true; }
        else if (prop == P_margin_left) { sv.marginLeftAuto = lowerCase(trimmed(v)) == L"auto"; sv.marginLeft = resolveLength(v, containingWidth, sv.marginLeftAuto ? 0 : sv.marginLeft); }
        else if (prop == P_padding) {
            parseBoxShorthand(v, containingWidth, 0, sv.paddingTop, sv.paddingRight, sv.paddingBottom, sv.paddingLeft);
        }
        else if (prop == P_padding_top) sv.paddingTop = resolveLength(v, containingWidth, sv.paddingTop);
        else if (prop == P_padding_right) sv.paddingRight = resolveLength(v, containingWidth, sv.paddingRight);
        else if (prop == P_padding_bottom) sv.paddingBottom = resolveLength(v, containingWidth, sv.paddingBottom);
        else if (prop == P_padding_left) sv.paddingLeft = resolveLength(v, containingWidth, sv.paddingLeft);
        else if (prop == P_width) {
            // While measuring a content width, a percentage counts as auto,
            // as in CSS - it would otherwise claim the whole width measured in.
            std::wstring w = trimmed(v);
            if (measuring_ && !w.empty() && w.back() == L'%') sv.width = -1;
            else sv.width = resolveLength(v, containingWidth, -1);
        }
        else if (prop == P_min_width) sv.minWidth = resolveLength(v, containingWidth, -1);
        else if (prop == P_max_width) sv.maxWidth = lowerCase(trimmed(v)) == L"none" ? -1 : resolveLength(v, containingWidth, -1);
        else if (prop == P_height) sv.height = resolveHeight(v, sv.fontSize, -1);
        else if (prop == P_min_height) sv.minHeight = resolveHeight(v, sv.fontSize, -1);
        else if (prop == P_max_height) sv.maxHeight = v == L"none" ? -1 : resolveHeight(v, sv.fontSize, -1);
        else if (prop == P_position) {
            std::wstring p = lowerCase(trimmed(v));
            if (p == L"relative") sv.position = ComputedStyle::Position::Relative;
            else if (p == L"absolute") sv.position = ComputedStyle::Position::Absolute;
            else if (p == L"fixed") sv.position = ComputedStyle::Position::Fixed;
            else if (p == L"sticky" || p == L"-webkit-sticky") sv.position = ComputedStyle::Position::Sticky;
            else if (p == L"static") sv.position = ComputedStyle::Position::Static;
        }
        else if (prop == P_top || prop == P_right || prop == P_bottom || prop == P_left) {
            Len& side = prop == P_top ? sv.top : prop == P_right ? sv.right : prop == P_bottom ? sv.bottom : sv.left;
            side = parseOffset(v, sv.fontSize);
        }
        else if (prop == P_inset) {
            // 1-4 values, like margin: top, right, bottom, left.
            std::vector<std::wstring> t = cssTokens(v);
            if (!t.empty() && t.size() <= 4) {
                static const int kPick[4][4] = { { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };
                Len* sides[4] = { &sv.top, &sv.right, &sv.bottom, &sv.left };
                for (int i = 0; i < 4; i++) *sides[i] = parseOffset(t[kPick[t.size() - 1][i]], sv.fontSize);
            }
        }
        else if (prop == P_z_index) {
            if (lowerCase(trimmed(v)) == L"auto") sv.zAuto = true;
            else { try { sv.zIndex = std::stoi(v); sv.zAuto = false; } catch (...) {} }
        }
        else if (prop == P_overflow || prop == P_overflow_x || prop == P_overflow_y) {
            // "hidden", or "<x> <y>" for the shorthand. Anything but visible
            // clips (see ComputedStyle::clipX).
            std::vector<std::wstring> t = cssTokens(lowerCase(v));
            if (t.empty()) return;
            auto clips = [](const std::wstring& o) { return o != L"visible"; };
            bool cx = clips(t[0]), cy = clips(t.size() > 1 ? t[1] : t[0]);
            if (prop != P_overflow_y) sv.clipX = cx;
            if (prop != P_overflow_x) sv.clipY = prop == P_overflow ? cy : cx;
        }
        else if (prop == P_box_sizing) {
            if (v == L"border-box") sv.boxSizing = BoxSizing::BorderBox;
            else if (v == L"content-box") sv.boxSizing = BoxSizing::ContentBox;
        }
        else if (prop == P_border_radius) {
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
        else if (prop == P_border_top_left_radius || prop == P_border_top_right_radius ||
                 prop == P_border_bottom_right_radius || prop == P_border_bottom_left_radius) {
            int corner = prop == P_border_top_left_radius ? 0 : prop == P_border_top_right_radius ? 1
                       : prop == P_border_bottom_right_radius ? 2 : 3;
            auto toks = cssTokens(v);
            LayoutBox::CornerRadius c;
            if (!toks.empty() && parseRadius(toks[0], sv.fontSize, c)) sv.radius[corner] = c;
        }
        else if (prop == P_border_color) sv.borderColor = v;
        else if (prop == P_border_width) sv.borderWidth = resolveLength(v, containingWidth, 0);
        else if (prop == P_border) {
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
        else if (prop == P_grid_template_columns) sv.gridTemplateColumns = parseGridTemplateTracks(v, containingWidth, sv.fontSize);
        else if (prop == P_grid_template_rows) sv.gridTemplateRows = parseGridTemplateTracks(v, containingWidth, sv.fontSize);
        else if (prop == P_grid_template_areas) sv.gridTemplateAreas = parseGridTemplateAreas(v);
        else if (prop == P_grid_template) {
            std::vector<std::vector<std::wstring>> areas;
            std::vector<GridTrack> rowTracks, colTracks;
            parseGridTemplateShorthand(v, containingWidth, sv.fontSize, areas, rowTracks, colTracks);
            if (!areas.empty()) sv.gridTemplateAreas = std::move(areas);
            if (!rowTracks.empty()) sv.gridTemplateRows = std::move(rowTracks);
            if (!colTracks.empty()) sv.gridTemplateColumns = std::move(colTracks);
        }
        else if (prop == P_grid_area) sv.gridArea = trimmed(v); // on an item: the area name to place into - see its ComputedStyle comment
        else if (prop == P_grid_column) {
            int s, e, n;
            if (parseGridLinePlacement(v, s, e, n)) { sv.gridColumnStart = s; sv.gridColumnEnd = e; sv.gridColumnSpan = n; }
        }
        else if (prop == P_grid_column_start || prop == P_grid_column_end) {
            int line, n;
            if (parseGridLine(v, line, n)) {
                (prop == P_grid_column_start ? sv.gridColumnStart : sv.gridColumnEnd) = line;
                if (n) sv.gridColumnSpan = n;
            }
        }
        else if (prop == P_grid_row) {
            int s, e, n;
            if (parseGridLinePlacement(v, s, e, n)) { sv.gridRowStart = s; sv.gridRowEnd = e; sv.gridRowSpan = n; }
        }
        else if (prop == P_grid_row_start || prop == P_grid_row_end) {
            int line, n;
            if (parseGridLine(v, line, n)) {
                (prop == P_grid_row_start ? sv.gridRowStart : sv.gridRowEnd) = line;
                if (n) sv.gridRowSpan = n;
            }
        }
        else if (prop == P_row_gap) sv.rowGap = resolveLength(v, containingWidth, 0);
        else if (prop == P_column_gap) sv.columnGap = resolveLength(v, containingWidth, 0);
        else if (prop == P_gap || prop == P_grid_gap) {
            // "gap: <row>" sets both; "gap: <row> <column>" sets them
            // separately, in that order - same order real CSS uses.
            std::wistringstream ss(v);
            std::wstring t1, t2;
            ss >> t1;
            int g1 = resolveLength(t1, containingWidth, 0);
            if (ss >> t2) { sv.rowGap = g1; sv.columnGap = resolveLength(t2, containingWidth, 0); }
            else { sv.rowGap = sv.columnGap = g1; }
        }
        else if (prop == P_font_size) sv.fontSize = resolveFontSize(v, inheritedFontSize, sv.fontSize);
        else if (prop == P_opacity) { try { sv.opacity = std::stof(v); } catch (...) {} }
        else if (prop == P_visibility) sv.visibilityHidden = (v == L"hidden" || v == L"collapse");
        else if (prop == P_display) {
            if (v == L"none") sv.display = Display::None;
            else if (v == L"inline" || v == L"inline-block") sv.display = Display::Inline;
            else if (v == L"block") sv.display = Display::Block;
            else if (v == L"grid") sv.display = Display::Grid;
            else if (v == L"flex") sv.display = Display::Flex;
            else if (v == L"table" || v == L"inline-table") sv.display = Display::Table;
            else if (v == L"table-row") sv.display = Display::TableRow;
            else if (v == L"table-cell") sv.display = Display::TableCell;
            else if (v == L"table-row-group") sv.display = Display::TableRowGroup;
            else if (v == L"table-header-group") sv.display = Display::TableHeaderGroup;
            else if (v == L"table-footer-group") sv.display = Display::TableFooterGroup;
            else if (v == L"table-caption") sv.display = Display::TableCaption;
            else if (v == L"table-column" || v == L"table-column-group") sv.display = Display::None;
            else if (v == L"flow-root") sv.display = Display::Block;
            sv.flowRoot = v == L"flow-root";
        }
        else if (prop == P_float) {
            std::wstring f = lowerCase(trimmed(v));
            if (f == L"left" || f == L"inline-start") sv.floatSide = ComputedStyle::Float::Left;
            else if (f == L"right" || f == L"inline-end") sv.floatSide = ComputedStyle::Float::Right;
            else if (f == L"none") sv.floatSide = ComputedStyle::Float::None;
        }
        else if (prop == P_clear) {
            std::wstring c = lowerCase(trimmed(v));
            if (c == L"left" || c == L"inline-start") sv.clear = ComputedStyle::Clear::Left;
            else if (c == L"right" || c == L"inline-end") sv.clear = ComputedStyle::Clear::Right;
            else if (c == L"both" || c == L"all") sv.clear = ComputedStyle::Clear::Both;
            else if (c == L"none") sv.clear = ComputedStyle::Clear::None;
        }
        else if (prop == P_vertical_align) {
            std::wstring a = lowerCase(trimmed(v));
            if (a == L"top" || a == L"text-top" || a == L"baseline") sv.verticalAlign = VAlign::Top;
            else if (a == L"middle") sv.verticalAlign = VAlign::Middle;
            else if (a == L"bottom" || a == L"text-bottom") sv.verticalAlign = VAlign::Bottom;
        }
        else if (prop == P_border_collapse) {
            std::wstring a = lowerCase(trimmed(v));
            if (a == L"collapse") sv.borderCollapse = true;
            else if (a == L"separate") sv.borderCollapse = false;
        }
        else if (prop == P_border_spacing) {
            // One length for both directions, or horizontal then vertical.
            std::vector<std::wstring> t = cssTokens(v);
            if (!t.empty() && t.size() <= 2) {
                sv.borderSpacingX = std::max(resolveLength(t[0], 0, sv.borderSpacingX), 0);
                sv.borderSpacingY = t.size() > 1 ? std::max(resolveLength(t[1], 0, sv.borderSpacingY), 0) : sv.borderSpacingX;
            }
        }
        else if (prop == P_flex_direction) {
            // The -reverse variants are laid out unreversed; anything
            // unrecognized falls back to row, the real default.
            sv.flexDirection = (v == L"column" || v == L"column-reverse") ? FlexDirection::Column : FlexDirection::Row;
        }
        else if (prop == P_justify_content) {
            if (v == L"center") sv.justifyContent = JustifyContent::Center;
            else if (v == L"flex-end") sv.justifyContent = JustifyContent::FlexEnd;
            else if (v == L"space-between") sv.justifyContent = JustifyContent::SpaceBetween;
            else if (v == L"space-around") sv.justifyContent = JustifyContent::SpaceAround;
            else sv.justifyContent = JustifyContent::FlexStart;
        }
        else if (prop == P_align_items) {
            if (v == L"flex-start") sv.alignItems = AlignItems::FlexStart;
            else if (v == L"center") sv.alignItems = AlignItems::Center;
            else if (v == L"flex-end") sv.alignItems = AlignItems::FlexEnd;
            else sv.alignItems = AlignItems::Stretch;
        }
        else if (prop == P_flex_grow) {
            try { sv.flexGrow = std::stof(v); } catch (...) {}
        }
        else if (prop == P_flex_shrink) {
            try { sv.flexShrink = std::max(std::stof(v), 0.0f); } catch (...) {}
        }
        else if (prop == P_flex_basis) {
            sv.flexBasis = (v == L"auto" || v == L"content") ? -1 : resolveLength(v, containingWidth, -1);
        }
        else if (prop == P_flex_wrap) {
            sv.flexWrap = v == L"wrap" ? FlexWrap::Wrap : v == L"wrap-reverse" ? FlexWrap::WrapReverse : FlexWrap::NoWrap;
        }
        else if (prop == P_flex_flow) {
            // "<direction> <wrap>" in either order, either part optional.
            for (const auto& tok : cssTokens(v)) {
                if (tok == L"row" || tok == L"row-reverse") sv.flexDirection = FlexDirection::Row;
                else if (tok == L"column" || tok == L"column-reverse") sv.flexDirection = FlexDirection::Column;
                else if (tok == L"wrap") sv.flexWrap = FlexWrap::Wrap;
                else if (tok == L"wrap-reverse") sv.flexWrap = FlexWrap::WrapReverse;
                else if (tok == L"nowrap") sv.flexWrap = FlexWrap::NoWrap;
            }
        }
        else if (prop == P_flex) {
            // The shorthand, per spec: none = 0 0 auto; auto = 1 1 auto;
            // otherwise up to two numbers (grow, then shrink) and a basis,
            // where a unitless number sets grow/shrink and anything else is
            // the basis. A basis left out becomes 0 - so `flex: 1` makes
            // items share the whole line equally, regardless of `width`.
            if (v == L"none") { sv.flexGrow = 0; sv.flexShrink = 0; sv.flexBasis = -1; }
            else if (v == L"auto") { sv.flexGrow = 1; sv.flexShrink = 1; sv.flexBasis = -1; }
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
                sv.flexShrink = shrink;
                sv.flexBasis = basisSet ? basis : 0;
            }
        }
    };

    // Every declaration that applies, in cascade order: matched rules (least
    // to most specific), then inline style.
    std::vector<const std::pair<std::wstring, std::wstring>*> cascade;
    // Presentational attributes (<table border=1 cellpadding=4>, <td
    // bgcolor valign width>) come first, so any author rule beats them.
    std::vector<std::pair<std::wstring, std::wstring>> hints = tableHints(e);
    for (const auto& decl : hints) cascade.push_back(&decl);
    if (rules) {
        std::vector<const CSS::Rule*> matched;
        const AncestorFilter& ancestors = ancestorFilter();
        auto consider = [&](const std::vector<const CSS::Rule*>& bucket) {
            for (const CSS::Rule* rule : bucket) {
                // A width-conditioned @media's rule carries the viewport
                // bound it needs (CSS::Rule's comment); checked here, against
                // the live viewport, rather than once at parse time - so a
                // resize (a full relayout, hence a fresh computeStyle pass)
                // re-evaluates it for free, no separate reactivity needed.
                if (rule->mediaMinWidth >= 0 && viewportWidth < rule->mediaMinWidth) continue;
                if (rule->mediaMaxWidth >= 0 && viewportWidth > rule->mediaMaxWidth) continue;
                // An ancestor the selector needs is missing (AncestorFilter).
                if (!ancestors.covers(ruleIndex.required[rule - rules->data()])) continue;
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
        if (!ruleIndex.byClass.empty() && e->attrs.count(L"class"))
            for (const auto& c : classesOf(e)) lookup(ruleIndex.byClass, c);
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
    emBase_ = sv.fontSize;

    // The tag's default vertical margins, in em of the final font size.
    if (!marginTopSet) sv.marginTop = (int)std::lround(uaMarginEm * sv.fontSize);
    if (!marginBottomSet) sv.marginBottom = (int)std::lround(uaMarginEm * sv.fontSize);

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
// then the result is a space-separated list of tracks (minmax(a, b) and
// fit-content(x) kept whole).
std::vector<LayoutRoot::GridTrack> LayoutRoot::parseGridTemplateTracks(const std::wstring& v, int containingWidth, int fontSize) {
    std::wstring expanded;
    size_t i = 0;
    while (i < v.size()) {
        size_t rep = v.find(L"repeat(", i);
        if (rep == std::wstring::npos) { expanded += v.substr(i); break; }
        expanded += v.substr(i, rep - i);
        // The matching ")" - the track inside can have its own (minmax(...)).
        size_t close = std::wstring::npos;
        for (size_t j = rep + 7, depth = 1; j < v.size(); j++) {
            if (v[j] == L'(') depth++;
            else if (v[j] == L')' && --depth == 0) { close = j; break; }
        }
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

    // A length: px, %, vw/vh, or em/rem (rem against the 14px base - see
    // LayoutRoot::layout - em against the element's font size).
    auto length = [&](const std::wstring& t, float& out) {
        double n; std::wstring unit;
        if (!parseNumberAndUnit(t, n, unit)) return false;
        if (unit == L"em") { out = (float)(n * fontSize); return true; }
        if (unit == L"rem") { out = (float)(n * 14); return true; }
        int px = resolveLength(t, containingWidth, -1);
        if (px < 0) return false;
        out = (float)px;
        return true;
    };
    auto isFr = [](const std::wstring& t, float& out) {
        if (t.size() < 3 || t.compare(t.size() - 2, 2, L"fr") != 0) return false;
        try { out = (float)std::stod(t.substr(0, t.size() - 2)); } catch (...) { out = 1.0f; }
        return true;
    };
    auto isContentKeyword = [](const std::wstring& t) {
        return t == L"auto" || t == L"min-content" || t == L"max-content";
    };
    auto track = [&](std::wstring t) -> GridTrack {
        t = lowerCase(trimmed(t));
        float n;
        if (isFr(t, n)) return { true, n };
        if (isContentKeyword(t)) return { false, 0, true };
        if (t.rfind(L"fit-content(", 0) == 0 && t.back() == L')') {
            GridTrack g{ false, 0, true };
            if (length(t.substr(12, t.size() - 13), n)) g.cap = n;
            return g;
        }
        if (t.rfind(L"minmax(", 0) == 0 && t.back() == L')') {
            // minmax(<min>, <max>): sized by its max - an fr share, the
            // content, or growing (like 1fr) up to a length. The min isn't
            // enforced.
            std::wstring inner = t.substr(7, t.size() - 8);
            size_t comma = inner.find(L',');
            std::wstring mx = trimmed(comma == std::wstring::npos ? inner : inner.substr(comma + 1));
            if (isFr(mx, n)) return { true, n };
            if (isContentKeyword(mx)) return { false, 0, true };
            GridTrack g{ true, 1.0f };
            if (length(mx, n)) g.cap = n;
            return g;
        }
        if (length(t, n)) return { false, n };
        return { true, 1.0f }; // not understood: 1fr - see the Layout.h comment on why
    };

    std::vector<GridTrack> tracks;
    for (const auto& tok : cssTokens(expanded)) tracks.push_back(track(tok));
    return tracks;
}

// See the declaration in Layout.h for the grammar/scope. One side of a
// grid-column/grid-row: "auto", a line number "N" (negative counts back
// from the explicit grid's last line, so -1 is the last line), or
// "span N". Sets `line` (0 = auto) or `span` (0 = none) accordingly.
bool LayoutRoot::parseGridLine(const std::wstring& v, int& line, int& span) {
    std::wistringstream ss(v);
    std::wstring a, b;
    if (!(ss >> a)) return false;
    if (a == L"auto") { line = 0; span = 0; return !(ss >> b); }
    if (a == L"span") {
        if (!(ss >> b)) return false;
        int n;
        try { n = std::stoi(b); } catch (...) { return false; }
        if (n <= 0) return false;
        line = 0; span = n;
        return true;
    }
    int n;
    try { n = std::stoi(a); } catch (...) { return false; }
    if (n == 0) return false; // line 0 doesn't exist in CSS
    line = n; span = 0;
    return true;
}

// See the declaration in Layout.h. Splits on '/' (minified CSS often has
// no spaces around it, e.g. "1/-1") and parses each side with
// parseGridLine; a missing end side is auto. Only one span is kept - if
// both sides give one, CSS ignores the end's.
bool LayoutRoot::parseGridLinePlacement(const std::wstring& v, int& start, int& end, int& span) {
    size_t slash = v.find(L'/');
    int sLine = 0, sSpan = 0, eLine = 0, eSpan = 0;
    if (!parseGridLine(v.substr(0, slash), sLine, sSpan)) return false;
    if (slash != std::wstring::npos && !parseGridLine(v.substr(slash + 1), eLine, eSpan)) return false;
    start = sLine; end = eLine;
    span = sSpan ? sSpan : eSpan;
    return true;
}

// The next quote (either kind) at or after `from`, or npos. A string ends at
// the next quote of the same kind.
static size_t findQuote(const std::wstring& s, size_t from) {
    return s.find_first_of(L"\"'", from);
}

// See the declaration in Layout.h for the grammar/scope.
std::vector<std::vector<std::wstring>> LayoutRoot::parseGridTemplateAreas(const std::wstring& v) {
    std::vector<std::vector<std::wstring>> rows;
    size_t i = 0;
    while (i < v.size()) {
        size_t open = findQuote(v, i);
        if (open == std::wstring::npos) break;
        size_t close = v.find(v[open], open + 1);
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
void LayoutRoot::parseGridTemplateShorthand(const std::wstring& v, int containingWidth, int fontSize,
                                             std::vector<std::vector<std::wstring>>& areas,
                                             std::vector<GridTrack>& rowTracks, std::vector<GridTrack>& colTracks) {
    size_t slash = v.find(L'/');
    std::wstring rowsPart = slash == std::wstring::npos ? v : v.substr(0, slash);

    // The plain form: `<rows> / <columns>`, no area strings.
    if (findQuote(rowsPart, 0) == std::wstring::npos) {
        std::wstring rows = lowerCase(trimmed(rowsPart));
        if (rows.empty() || rows == L"none") return;
        rowTracks = parseGridTemplateTracks(rows, containingWidth, fontSize);
        if (slash != std::wstring::npos) colTracks = parseGridTemplateTracks(trimmed(v.substr(slash + 1)), containingWidth, fontSize);
        return;
    }

    size_t i = 0;
    while (i < rowsPart.size()) {
        size_t open = findQuote(rowsPart, i);
        if (open == std::wstring::npos) break;
        size_t close = rowsPart.find(rowsPart[open], open + 1);
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
        size_t nextOpen = findQuote(rowsPart, close + 1);
        std::wstring between = trimmed(rowsPart.substr(close + 1, (nextOpen == std::wstring::npos ? rowsPart.size() : nextOpen) - close - 1));
        if (!between.empty()) {
            auto parsed = parseGridTemplateTracks(between, containingWidth, fontSize);
            rowTracks.push_back(!parsed.empty() ? parsed[0] : GridTrack{ true, 0.0f });
        } else {
            rowTracks.push_back({ true, 0.0f });
        }
        i = close + 1;
    }

    if (slash != std::wstring::npos) colTracks = parseGridTemplateTracks(trimmed(v.substr(slash + 1)), containingWidth, fontSize);
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

// The hash an ancestor filter files a tag name, id or class under - kept
// apart by `kind`, so a class "nav" and a tag "nav" don't collide.
static size_t filterKey(wchar_t kind, const std::wstring& name) {
    size_t h = std::hash<std::wstring>{}(name);
    return (h ^ (size_t)kind * 0x9E3779B97F4A7C15ull) * 0xBF58476D1CE4E5B9ull;
}

const std::vector<std::wstring>& LayoutRoot::classesOf(Element* el) {
    auto [it, inserted] = classCache.try_emplace(el);
    if (inserted) {
        auto c = el->attrs.find(L"class");
        if (c != el->attrs.end()) {
            std::wistringstream ss(c->second);
            for (std::wstring name; ss >> name;) it->second.push_back(name);
        }
    }
    return it->second;
}

const LayoutRoot::AncestorFilter& LayoutRoot::ancestorFilter() {
    // Keep the part that still matches ancestorStack, recompute the rest.
    size_t same = 0;
    while (same < filterFor_.size() && same < ancestorStack.size() && filterFor_[same] == ancestorStack[same]) same++;
    filterFor_.resize(same);
    filterStack_.resize(same);
    for (size_t i = same; i < ancestorStack.size(); i++) {
        Element* a = ancestorStack[i];
        AncestorFilter f = i ? filterStack_[i - 1] : AncestorFilter{};
        f.add(filterKey(L't', a->tag));
        auto id = a->attrs.find(L"id");
        if (id != a->attrs.end() && !id->second.empty()) f.add(filterKey(L'#', id->second));
        for (const auto& c : classesOf(a)) f.add(filterKey(L'.', c));
        filterFor_.push_back(a);
        filterStack_.push_back(f);
    }
    static const AncestorFilter empty;
    return filterStack_.empty() ? empty : filterStack_.back();
}

void LayoutRoot::rebuildRuleIndex() {
    ruleIndex = RuleIndex{};
    filterFor_.clear();
    filterStack_.clear();
    if (!rules) return;
    ruleIndex.required.resize(rules->size());
    for (size_t r = 0; r < rules->size(); r++) {
        // What the rule needs among the element's ancestors: every compound
        // whose right-hand combinator is a descendant or child one (a
        // compound left of + or ~ is a sibling, not an ancestor).
        const auto& chain = (*rules)[r].chain;
        for (size_t i = 0; i + 1 < chain.size(); i++) {
            CSS::Combinator next = chain[i + 1].combinator;
            if (next != CSS::Combinator::Descendant && next != CSS::Combinator::Child) continue;
            const CSS::CompoundSelector& c = chain[i];
            AncestorFilter& need = ruleIndex.required[r];
            if (!c.tag.empty()) need.add(filterKey(L't', c.tag));
            if (!c.id.empty()) need.add(filterKey(L'#', c.id));
            for (const auto& cls : c.classes) need.add(filterKey(L'.', cls));
        }
    }
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
    cellCache_.clear(); // likewise
    if (measureScale != cachedScale_) { textCache_.clear(); textCacheSize_ = 0; cachedScale_ = measureScale; } // widths change with zoom
    nextAnchor_ = 1;
    measuring_ = false;
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

    FloatContext rootFloats;
    floatCtx_ = &rootFloats;
    detachedRoot_ = false;
    int y = 10;
    // The 10px above <body>'s content stands in for its margin, which -
    // with no border or padding of its own - merges with its first child's.
    openTops_.clear();
    marginEndY_ = y;
    pendingMargin_ = 10;
    layoutElement(root, 10, y, viewportWidth - 20, bodyStyle.fontSize, false, bodyStyle.paint);
    floatCtx_ = nullptr;

    // Sticky children of <body> stick until the end of the page.
    for (int i : openStickies_.back())
        stickies[i].maxShift = std::max(y - stickies[i].naturalTop - stickies[i].maxShift, 0);
    openStickies_.clear();

    containingHeight_ = viewportHeight;
    layoutOutOfFlow(viewport, 0, 0, viewportWidth, viewportHeight, true, 0);
    positioned_.clear();
    // The anchors have served their purpose (LayoutBox::anchor).
    boxes.erase(std::remove_if(boxes.begin(), boxes.end(), [](const LayoutBox& b) { return b.anchor != 0; }), boxes.end());

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
    if (hasStatic) {
        // Mark the static position in the box list, so it moves with the
        // boxes around it (see LayoutBox::anchor).
        LayoutBox marker;
        marker.x = staticX;
        marker.y = staticY;
        marker.width = marker.height = 0;
        marker.anchor = item.anchor = nextAnchor_++;
        boxes.push_back(std::move(marker));
    }
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
void LayoutRoot::layoutOutOfFlow(ContainingBlock& cb, int cbX, int cbY, int cbW, int cbH, bool isViewport, size_t searchFrom) {
    if (cb.pending.empty()) return;
    if (isViewport) usedViewportHeight = true; // placed against the viewport's height
    // Where each anchor ended up (see LayoutBox::anchor). Scanned as needed:
    // laying one element out appends boxes, and anchors of elements it
    // queues (a fixed element inside it, to the viewport's list).
    std::unordered_map<int, std::pair<int, int>> anchors;
    size_t scanned = std::min(searchFrom, boxes.size());
    // By index: laying one out can add more (a fixed element inside it, to
    // the viewport's list).
    for (size_t i = 0; i < cb.pending.size(); i++) {
        OutOfFlow p = cb.pending[i];
        if (p.anchor) {
            for (; scanned < boxes.size(); scanned++)
                if (boxes[scanned].anchor) anchors[boxes[scanned].anchor] = { boxes[scanned].x, boxes[scanned].y };
            auto at = anchors.find(p.anchor);
            if (at != anchors.end()) { p.staticX = at->second.first; p.staticY = at->second.second; }
        }
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
    int key = style.zAuto ? 2 : 4 * style.zIndex + 2;
    for (size_t i = from; i < boxes.size(); i++) {
        // Boxes still in the flow (or in a float) take this element's key.
        // A positioned descendant keeps its own - unless this element has
        // an explicit z-index, which makes it a stacking context its
        // descendants can't escape (simplified: they all share its key).
        if (boxes[i].paintKey == 0 || boxes[i].paintKey == 1 || !style.zAuto) boxes[i].paintKey = key;
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
                // <br clear=all>: the next line starts below the floats.
                if (e->attrs.count(L"clear")) {
                    ComputedStyle bs = computeStyle(e, inheritedFontSize, containingWidth, inheritedVisuallyHidden, inheritedPaint);
                    if (bs.clear != ComputedStyle::Clear::None) {
                        flushInline();
                        y = std::max(y, clearance(bs.clear));
                    }
                }
                continue;
            }

            ComputedStyle sv = computeStyle(e, inheritedFontSize, containingWidth, inheritedVisuallyHidden, inheritedPaint);
            if (sv.display == Display::None) continue; // this element and its subtree take no space

            // Absolute/fixed: out of the flow - laid out once its containing
            // block is (see deferOutOfFlow), from where it would have been.
            if (sv.position == ComputedStyle::Position::Absolute || sv.position == ComputedStyle::Position::Fixed) {
                // A block-level one's static position is below the text
                // before it, so that text is laid out first.
                if (sv.display != Display::Inline) flushInline();
                deferOutOfFlow(e, sv, x, y, true);
                continue;
            }

            // A float joins the inline run, which places it where the run
            // reaches it (see placeFloat).
            if (sv.floatSide != ComputedStyle::Float::None) {
                pendingInline.push_back(makeFloatItem(e, sv));
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

// Two adjoining vertical margins merged into one, as CSS collapses them: the
// largest positive one plus the most negative one.
static int combineMargins(int a, int b) {
    return std::max(std::max(a, b), 0) + std::min(std::min(a, b), 0);
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
    // Floats. `clear` moves the block below them. A block that contains its
    // own floats (a float context root - see FloatContext) doesn't overlap
    // the floats around it either: it's narrowed to the space beside them,
    // or - a table that doesn't fit there - moved down past them.
    const bool detached = detachedRoot_;
    detachedRoot_ = false;
    const bool contextRoot = detached || sv.flowRoot || sv.clipX || sv.clipY || sv.display == Display::Table ||
                             sv.display == Display::Flex || sv.display == Display::Grid ||
                             sv.floatSide != ComputedStyle::Float::None;
    if (sv.clear != ComputedStyle::Clear::None) {
        int c = clearance(sv.clear);
        if (c != INT_MIN) y = std::max(y, c - sv.marginTop);
    }
    const int fullX = x, fullWidth = containingWidth;
    // Narrows x/containingWidth to the space beside the floats at y;
    // returns where to try next if that's too narrow, or INT_MIN.
    auto besideFloats = [&]() {
        x = fullX;
        containingWidth = fullWidth;
        int left, right, next;
        if (!contextRoot || detached || !floatCtx_ || floatCtx_->floats.empty() ||
            !spaceBeside(y + sv.marginTop, 1, fullX, fullWidth, left, right, next))
            return INT_MIN;
        x = left;
        containingWidth = right - left;
        return next;
    };
    int nextTry = besideFloats();

    // A table's width always includes its border and padding, as in browsers.
    const bool borderBox = sv.boxSizing == BoxSizing::BorderBox || sv.display == Display::Table;
    const int chromeX = borderBox ? 0 : sv.paddingLeft + sv.paddingRight + 2 * sv.borderWidth;
    int widthValue = sv.width >= 0 ? sv.width
                                   : std::max(containingWidth - sv.marginLeft - sv.marginRight - chromeX, 0); // auto: fill the container

    // A table with no width is as wide as its columns want, up to what's
    // available - and never narrower than they need.
    const int chromeFull = sv.paddingLeft + sv.paddingRight + 2 * sv.borderWidth;
    std::unique_ptr<TableModel> table;
    if (sv.display == Display::Table) {
        int available;
        for (;;) {
            available = std::max(containingWidth - sv.marginLeft - sv.marginRight - chromeFull, 0);
            table = buildTable(e, sv, available);
            if (nextTry == INT_MIN || table->minWidth <= available) break;
            y = nextTry - sv.marginTop; // too narrow beside the floats: try lower down
            nextTry = besideFloats();
        }
        if (sv.width < 0)
            widthValue = std::max(std::min(table->maxWidth, available), table->minWidth) + chromeFull - chromeX;
    }

    if (sv.maxWidth >= 0) widthValue = std::min(widthValue, sv.maxWidth);
    if (sv.minWidth >= 0) widthValue = std::max(widthValue, sv.minWidth);
    int outerWidth = widthValue + chromeX;
    int contentWidth = std::max(outerWidth - sv.paddingLeft - sv.paddingRight - 2 * sv.borderWidth, 0);
    if (table && contentWidth < table->minWidth) {
        contentWidth = table->minWidth;
        outerWidth = contentWidth + chromeFull;
    }

    // margin: auto - what's left of the container goes to the auto margin(s):
    // both centres the box (margin: 0 auto), one pushes it to the other side.
    int marginLeft = sv.marginLeft;
    if (measuring_) {} // a content width doesn't include centring (see measuring_)
    else if (sv.marginLeftAuto || sv.marginRightAuto) {
        int free = std::max(containingWidth - outerWidth - (sv.marginLeftAuto ? 0 : sv.marginLeft) -
                            (sv.marginRightAuto ? 0 : sv.marginRight), 0);
        if (sv.marginLeftAuto && sv.marginRightAuto) marginLeft = free / 2;
        else if (sv.marginLeftAuto) marginLeft = free;
    }
    else if (sv.centeredByParent) // inside <center> or align=center
        marginLeft += std::max(containingWidth - outerWidth - sv.marginLeft - sv.marginRight, 0) / 2;
    int boxX = x + marginLeft;

    // Margin collapsing (see marginEndY_): a top margin straight after
    // another margin - a previous sibling's bottom one, or the parent's own
    // top one with nothing in between - merges with it rather than adding
    // to it. If it's the parent's, this box's top is the parent's top too.
    int topMargin = sv.marginTop; // the margin above this box, merged
    bool collapsedWithParent = false;
    if (y == marginEndY_) {
        y -= pendingMargin_;
        topMargin = combineMargins(pendingMargin_, sv.marginTop);
        collapsedWithParent = !openTops_.empty() && openTops_.back().y == marginEndY_ && openTops_.back().newTop == INT_MIN;
    }
    y += topMargin;
    marginEndY_ = INT_MIN;
    if (collapsedWithParent) openTops_.back().newTop = y;
    // A table's captions sit above it, outside its border.
    if (table && !table->captions.empty()) {
        ancestorStack.push_back(e);
        for (const auto& [caption, captionStyle] : table->captions)
            layoutBlockChild(caption, boxX, y, outerWidth, captionStyle);
        ancestorStack.pop_back();
    }
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

    // With no border or padding above its content (and no float context of
    // its own), its top margin stays open to its first child's.
    const bool openTop = !contextRoot && !table && sv.borderWidth == 0 && sv.paddingTop == 0;
    if (openTop) {
        marginEndY_ = y;
        pendingMargin_ = topMargin;
        openTops_.push_back({ y, INT_MIN });
    }

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

    // A float context root keeps the floats inside it to itself.
    FloatContext ownFloats;
    FloatContext* savedFloats = floatCtx_;
    if (contextRoot) floatCtx_ = &ownFloats;

    int childX = boxX + sv.borderWidth + sv.paddingLeft;
    int contentTop = y;
    if (sv.display == Display::Grid) layoutGrid(e, childX, y, contentWidth, sv);
    else if (sv.display == Display::Flex) layoutFlex(e, childX, y, contentWidth, sv);
    else if (table) layoutTable(e, childX, y, contentWidth, sv, *table);
    else layoutElement(e, childX, y, contentWidth, sv.fontSize, sv.visuallyHidden, sv.paint);

    floatCtx_ = savedFloats;
    currentHref = savedHref;
    currentForm = savedForm;
    containingHeight_ = savedCH;

    // The content height: its own if set, else its children's - floats
    // included, for a float context root; then min-/max-height. Content
    // taller than a set height overflows - drawn anyway unless overflow
    // clips it (below) - but doesn't push what follows further down.
    if (contextRoot) y = std::max(y, ownFloats.bottom(true, true));

    // Its first child's top margin merged with its own: its box starts where
    // that child does (and so does its parent's, if it had merged with that).
    if (openTop) {
        int newTop = openTops_.back().newTop;
        openTops_.pop_back();
        if (newTop != INT_MIN && newTop > contentStartY) {
            if (collapsedWithParent && !openTops_.empty() && openTops_.back().newTop == contentStartY)
                openTops_.back().newTop = newTop;
            contentStartY = contentTop = newTop;
            if (bgIndex != static_cast<size_t>(-1)) boxes[bgIndex].y = newTop;
        }
    }
    // Likewise at the bottom: with no border, padding or set height below
    // its content, its last child's bottom margin (or, if it's empty, its
    // own top margin) carries through to merge with its bottom margin.
    const bool openBottom = !contextRoot && !table && sv.borderWidth == 0 && sv.paddingBottom == 0 &&
                            fixedHeight < 0 && sv.minHeight < 0;
    int marginBase = INT_MIN, carried = 0; // a margin carried through: where it starts, and its size
    if (openBottom && y == marginEndY_) {
        carried = pendingMargin_;
        marginBase = y - carried;
        y = std::max(marginBase, contentTop); // the content ends before it
    }
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
                        outerWidth - 2 * sv.borderWidth, borderHeight - 2 * sv.borderWidth, false, firstBox);
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
    // Its bottom margin - merged with one carried through it, if any - left
    // open for whatever comes next to merge with.
    if (marginBase != INT_MIN) {
        pendingMargin_ = combineMargins(carried, sv.marginBottom);
        y = marginBase + pendingMargin_;
    }
    else {
        pendingMargin_ = sv.marginBottom;
        y += sv.marginBottom;
    }
    marginEndY_ = y;
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
    // set here from grid-area, or from grid-column/grid-row once the
    // column count is known; -1 (auto) for the placement pass to fill in.
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
        // doesn't appear in it) fall through to line-based placement below.
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
            }
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
    // No template at all: one full-width column (content-sized while measuring).
    if (cols.empty()) cols.push_back(measuring_ ? GridTrack{ false, 0, true } : GridTrack{ false, (float)containingWidth });
    int numCols = (int)cols.size();

    // Resolve each item's grid-column/grid-row against the template (see
    // ComputedStyle's comment on those fields) into a 0-based cell range
    // per axis, or leave that axis auto (-1) with just a span size for
    // the placement pass. Columns clamp to the template's count - a line
    // beyond it doesn't create an implicit column (see Layout.h); rows
    // grow as needed.
    int explicitRows = (int)std::max(style.gridTemplateRows.size(), style.gridTemplateAreas.size());
    auto resolveAxis = [](int startLine, int endLine, int span, int numTracks, int& a, int& b, int& size) {
        auto idx = [&](int line) { return std::max(line > 0 ? line - 1 : numTracks + 1 + line, 0); };
        size = std::max(span, 1);
        if (startLine && endLine) {
            a = idx(startLine); b = idx(endLine);
            if (b < a) std::swap(a, b);
            if (a == b) b = a + 1;
        } else if (startLine) {
            a = idx(startLine); b = a + size;
        } else if (endLine) {
            b = std::max(idx(endLine), 1); a = std::max(b - size, 0);
        } else {
            a = b = -1; // auto
            return;
        }
        size = b - a;
    };
    std::vector<int> colSize(placements.size()), rowSize(placements.size());
    for (size_t i = 0; i < placements.size(); i++) {
        auto& p = placements[i];
        if (p.colStart >= 0) { colSize[i] = p.colEnd - p.colStart; rowSize[i] = p.rowEnd - p.rowStart; continue; } // placed by grid-area
        auto& cs = p.style;
        resolveAxis(cs.gridColumnStart, cs.gridColumnEnd, cs.gridColumnSpan, numCols, p.colStart, p.colEnd, colSize[i]);
        resolveAxis(cs.gridRowStart, cs.gridRowEnd, cs.gridRowSpan, explicitRows, p.rowStart, p.rowEnd, rowSize[i]);
        if (p.colStart >= 0) {
            p.colStart = std::clamp(p.colStart, 0, numCols - 1);
            p.colEnd = std::clamp(p.colEnd, p.colStart + 1, numCols);
        }
        colSize[i] = std::min(colSize[i], numCols);
    }

    // CSS's (sparse, grid-auto-flow: row) auto-placement: first items
    // definite in both axes claim their cells, then items with only a
    // definite row take the first columns that fit in it, then everything
    // else goes in source order behind a cursor that only moves forward -
    // an item with a definite column moves the cursor down a row if that
    // column is already behind it, a fully auto one takes the next spot
    // its whole span fits. Simpler than real CSS in one way: a definite
    // row that's already full doesn't grow implicit columns.
    std::set<std::pair<int, int>> occupied; // (row, col)
    // grid-template-areas defines the row count too, even for a trailing
    // row nothing ends up placed in (e.g. one made entirely of "." cells).
    int maxRowUsed = style.gridTemplateAreas.empty() ? -1 : (int)style.gridTemplateAreas.size() - 1;
    auto fits = [&](int r, int c, int w, int h) {
        if (c + w > numCols) return false;
        for (int rr = r; rr < r + h; rr++)
            for (int cc = c; cc < c + w; cc++)
                if (occupied.count({ rr, cc })) return false;
        return true;
    };
    auto place = [&](ItemPlacement& p, int r, int c, int w, int h) {
        p.rowStart = r; p.rowEnd = r + h;
        p.colStart = c; p.colEnd = c + w;
        for (int rr = r; rr < r + h; rr++)
            for (int cc = c; cc < c + w; cc++)
                occupied.insert({ rr, cc });
        maxRowUsed = std::max(maxRowUsed, r + h - 1);
    };
    for (auto& p : placements)
        if (p.colStart >= 0 && p.rowStart >= 0) place(p, p.rowStart, p.colStart, p.colEnd - p.colStart, p.rowEnd - p.rowStart);
    for (size_t i = 0; i < placements.size(); i++) {
        auto& p = placements[i];
        if (p.rowStart < 0 || p.colStart >= 0) continue; // only definite-row, auto-column items
        int w = colSize[i], h = p.rowEnd - p.rowStart, c = 0;
        while (c + w <= numCols && !fits(p.rowStart, c, w, h)) c++;
        if (c + w > numCols) c = 0; // row is full - overlap rather than grow implicit columns
        place(p, p.rowStart, c, w, h);
    }
    int cursorRow = 0, cursorCol = 0;
    for (size_t i = 0; i < placements.size(); i++) {
        auto& p = placements[i];
        if (p.rowStart >= 0) continue; // already placed above
        int h = rowSize[i];
        if (p.colStart >= 0) {
            int w = p.colEnd - p.colStart;
            if (p.colStart < cursorCol) cursorRow++;
            while (!fits(cursorRow, p.colStart, w, h)) cursorRow++;
            place(p, cursorRow, p.colStart, w, h);
            cursorCol = p.colEnd;
        } else {
            int w = colSize[i];
            for (;;) {
                if (cursorCol + w > numCols) { cursorCol = 0; cursorRow++; }
                if (fits(cursorRow, cursorCol, w, h)) break;
                cursorCol++;
            }
            place(p, cursorRow, cursorCol, w, h);
            cursorCol += w;
        }
    }
    int numRows = maxRowUsed + 1;

    // Column widths, now that it's known what sits in each column: fixed
    // tracks first; then content-sized ones (auto, min-content, ...), each
    // as wide as the widest item placed in it alone (shrink-to-fit, up to
    // what's left and its cap); then fr tracks share the rest (each up to
    // its cap, for minmax(0, <length>)). While measuring a content width
    // (measuring_), fr tracks are content-sized too, as CSS sizes them for
    // a max-content measurement - shared out, they'd claim all the width
    // measured in.
    const int colGap = style.columnGap;
    std::vector<int> colWidths(numCols, 0), colX(numCols);
    auto contentSized = [&](int i) { return cols[i].isAuto || (measuring_ && cols[i].isFr); };
    int used = colGap * std::max(numCols - 1, 0);
    for (int i = 0; i < numCols; i++) {
        if (cols[i].isFr || cols[i].isAuto) continue;
        colWidths[i] = std::max((int)std::lround(cols[i].value), 0);
        used += colWidths[i];
    }
    for (int i = 0; i < numCols; i++) {
        if (!contentSized(i)) continue;
        int available = std::max(containingWidth - used, 0);
        int w = 0;
        for (const auto& p : placements) {
            if (p.colStart != i || p.colEnd != i + 1) continue;
            const ComputedStyle& s = p.style;
            if (s.width >= 0) {
                int chrome = s.boxSizing == BoxSizing::BorderBox ? 0 : s.paddingLeft + s.paddingRight + 2 * s.borderWidth;
                w = std::max(w, s.marginLeft + s.width + chrome + s.marginRight);
            }
            else w = std::max(w, shrinkToFitWidth(p.el, s, available));
        }
        if (cols[i].cap >= 0) w = std::min(w, (int)std::lround(cols[i].cap));
        colWidths[i] = std::min(w, available);
        used += colWidths[i];
    }
    float frTotal = 0;
    for (const auto& t : cols) if (t.isFr) frTotal += t.value;
    const int remaining = std::max(containingWidth - used, 0);
    for (int i = 0; i < numCols; i++) {
        if (!cols[i].isFr || contentSized(i)) continue;
        int w = frTotal > 0 ? (int)std::lround(remaining * (cols[i].value / frTotal)) : 0;
        if (cols[i].cap >= 0) w = std::min(w, (int)std::lround(cols[i].cap));
        colWidths[i] = std::max(w, 0);
    }
    for (int i = 0, cx = x; i < numCols; i++) {
        colX[i] = cx;
        cx += colWidths[i] + colGap;
    }

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

        // A grid item is always block-level, regardless of its own tag's
        // default (real CSS "blockifies" it the same way), and laid out on
        // its own: its own float context, its margins kept to itself.
        if (real.display == Display::Inline) real.display = Display::Block;
        itemBoxes[idx] = layoutItemDetached(p.el, spanWidth, real, itemHeights[idx]);
    }

    // Row heights: an explicit grid-template-rows track wins if set (fr
    // tracks excepted, per its ComputedStyle comment); otherwise a row is
    // as tall as the tallest single-row item placed in it.
    std::vector<GridTrack> rowTracks = style.gridTemplateRows;
    std::vector<int> rowHeights(numRows, 0);
    std::vector<bool> rowExplicit(numRows, false);
    for (int r = 0; r < numRows; r++) {
        if (r < (int)rowTracks.size() && !rowTracks[r].isFr && !rowTracks[r].isAuto) {
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
    //    else its explicit width, else its content's width - a shrink-to-fit
    //    estimate (shrinkToFitWidth), standing in for CSS's max-content
    //    size. As in CSS, an item only grows past that if flex-grow says so.
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
    std::vector<char> basisFromContent(n, 0); // the basis is the content's width - never below its min-content
    for (int i = 0; i < n; i++) {
        const ComputedStyle& s = itemStyles[i];
        int chrome = s.paddingLeft + s.paddingRight + 2 * s.borderWidth;
        minWidth[i] = chrome;
        int w;
        if (s.flexBasis >= 0) w = s.boxSizing == BoxSizing::BorderBox ? s.flexBasis : s.flexBasis + chrome;
        else w = explicitWidth(s);
        basisFromContent[i] = w < 0;
        basis[i] = w >= 0 ? w : shrinkToFitWidth(items[i], s, containingWidth);
        basis[i] = std::max(basis[i], minWidth[i]);
        grow[i] = s.flexGrow;
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

        // Free space goes to items by flex-grow; overflow is taken back by
        // flex-shrink weighted by basis. Either way no item ends up below
        // its min-content width (CSS's min-width: auto for flex items, but
        // never above its own width if it set one): an item that would is
        // frozen there and the free space worked out again for the rest,
        // round by round. The min-content width is measured only where it
        // could matter - an item being shrunk, or one whose basis didn't
        // come from its content (flex: 1's basis of 0, say).
        // Measuring a content width (measuring_), free space isn't handed
        // out - not to flex-grow, nor to justify-content or auto margins
        // below.
        std::vector<int> floorW(count, -1);
        auto floorOf = [&](int k) {
            int i = a + k;
            if (floorW[k] < 0) {
                int f = minContentWidth(items[i], itemStyles[i]);
                int own = explicitWidth(itemStyles[i]);
                if (own >= 0) f = std::min(f, own);
                floorW[k] = std::max(f, minWidth[i]);
            }
            return floorW[k];
        };
        std::vector<char> frozen(count, 0);
        for (;;) {
            int usedNow = colGap * (count - 1);
            float growNow = 0, shrinkNow = 0;
            for (int k = 0; k < count; k++) {
                int i = a + k;
                usedNow += frozen[k] ? itemWidths[i] : basis[i];
                if (!frozen[k]) { growNow += grow[i]; shrinkNow += shrink[i] * basis[i]; }
            }
            int freeNow = containingWidth - usedNow;
            bool newlyFrozen = false;
            for (int k = 0; k < count; k++) {
                if (frozen[k]) continue;
                int i = a + k;
                int w = basis[i];
                if (freeNow > 0 && growNow > 0 && !measuring_) w += (int)std::lround(freeNow * (grow[i] / growNow));
                else if (freeNow < 0 && shrinkNow > 0) w += (int)std::lround(freeNow * (shrink[i] * basis[i] / shrinkNow));
                if ((w < basis[i] || !basisFromContent[i]) && w < floorOf(k)) { w = floorOf(k); frozen[k] = 1; newlyFrozen = true; }
                itemWidths[i] = w;
            }
            if (!newlyFrozen) break;
        }
        for (int i = a; i < b; i++) {
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
        int leftover = measuring_ ? 0 : std::max(containingWidth - usedWidth, 0);

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
    // Laid out at (0, 0), away from the floats around it - in a float
    // context of its own, which it contains (see FloatContext).
    FloatContext* savedFloats = floatCtx_;
    FloatContext detachedFloats;
    floatCtx_ = &detachedFloats;
    detachedRoot_ = true;
    // Nor do its margins merge with anything outside it.
    const int savedMarginEnd = marginEndY_, savedPending = pendingMargin_;
    std::vector<OpenTop> savedOpenTops;
    savedOpenTops.swap(openTops_);
    marginEndY_ = INT_MIN;
    int localY = 0;
    if (item->tag == L"input" || item->tag == L"button" || item->tag == L"select")
        layoutControl(item, 0, localY, width, style);
    else if (item->tag == L"img" || item->tag == L"svg")
        layoutImage(item, 0, localY, width, style);
    else
        layoutBlockChild(item, 0, localY, width, style);
    detachedRoot_ = false;
    floatCtx_ = savedFloats;
    marginEndY_ = savedMarginEnd;
    pendingMargin_ = savedPending;
    openTops_.swap(savedOpenTops);
    std::swap(boxes, scratch);
    height = localY;
    return scratch;
}

int LayoutRoot::shrinkToFitWidth(Element* item, const ComputedStyle& style, int available) {
    return std::min(contentWidth(item, style, available, false), available);
}

int LayoutRoot::minContentWidth(Element* item, const ComputedStyle& style) {
    return contentWidth(item, style, 0, true);
}

int LayoutRoot::contentWidth(Element* item, const ComputedStyle& style, int width, bool narrow) {
    // A measuring layout: content isn't aligned, centred or spread out to
    // fill the width, which would make it reach further than it needs.
    // Absolute elements met go to a throwaway containing block.
    const bool wasMeasuring = measuring_;
    measuring_ = true;
    ContainingBlock scratch;
    positioned_.push_back(&scratch);
    int height;
    std::vector<LayoutBox> trial = layoutItemDetached(item, width, style, height);
    positioned_.pop_back();
    measuring_ = wasMeasuring;
    int right = 0;
    for (const auto& b : trial) {
        if (b.anchor) continue; // marks a position; has no size of its own
        // Content has a natural width. A background box, when laid out
        // wide, just spans whatever width it was given, so it says nothing
        // about fit; laid out narrow, it's only as wide as it must be.
        bool content = !b.text.empty() || !b.imageSrc.empty() || b.control != LayoutBox::NoControl;
        if (!content && (!narrow || b.el == item)) continue;
        // A text box starts at the run's content edge, but layoutInlineRun
        // wraps at the content width minus a 4px inset on *each* side.
        int textInset = b.text.empty() || b.control != LayoutBox::NoControl ? 0 : 8;
        right = std::max(right, b.x + b.width + textInset);
    }
    int measured = right + style.paddingRight + style.borderWidth + style.marginRight + 1; // +1: rounding in text measurement
    return std::max(measured, 0);
}

// --- Floats ----------------------------------------------------------------------
//
// A float is taken out of the flow and laid out on its own (shrink-to-fit
// unless it has a width, in a float context of its own), then placed at the
// left or right edge of its containing block, as high as it can go: no
// higher than where the inline run reached it, nor than an earlier float,
// and lower down if it doesn't fit beside the floats already there. Lines
// of text in the same float context are then shortened by the floats
// beside them (layoutInlineRun's openLine), in nested blocks as well -
// floats are kept in the same coordinates as the boxes.
//
// Simplified: a float met mid-line goes below that line rather than onto
// it; there's no margin collapsing to account for; and a block without
// its own float context ends at its content, so floats inside it can hang
// out of the bottom (as in CSS - that's what clearfixes are for - but
// ::after clearfixes aren't supported, so overflow: hidden or display:
// flow-root is what contains them here).

int LayoutRoot::FloatContext::bottom(bool left, bool right) const {
    int b = INT_MIN;
    for (const auto& f : floats)
        if (f.left ? left : right) b = std::max(b, f.y + f.h);
    return b;
}

bool LayoutRoot::spaceBeside(int top, int height, int x, int width, int& left, int& right, int& nextTop) const {
    left = x;
    right = x + width;
    nextTop = INT_MAX;
    bool narrowed = false;
    if (!floatCtx_) return false;
    for (const auto& f : floatCtx_->floats) {
        if (f.y >= top + height || f.y + f.h <= top) continue; // not beside it
        if (f.left ? f.x + f.w <= left : f.x >= right) continue; // doesn't reach into it
        if (f.left) left = f.x + f.w;
        else right = f.x;
        narrowed = true;
        nextTop = std::min(nextTop, f.y + f.h);
    }
    if (right < left) right = left;
    return narrowed;
}

int LayoutRoot::clearance(ComputedStyle::Clear clear) const {
    if (!floatCtx_ || clear == ComputedStyle::Clear::None) return INT_MIN;
    return floatCtx_->bottom(clear != ComputedStyle::Clear::Right, clear != ComputedStyle::Clear::Left);
}

LayoutRoot::InlineItem LayoutRoot::makeFloatItem(Element* e, ComputedStyle style, bool block) {
    auto f = std::make_shared<FloatItem>();
    f->el = e;
    f->style = std::move(style);
    f->ancestors = ancestorStack;
    f->href = currentHref;
    f->form = currentForm;
    InlineItem item;
    item.owner = e;
    (block ? item.blockItem : item.floatItem) = std::move(f);
    return item;
}

void LayoutRoot::layoutInlineBlock(const FloatItem& item, int x, int& y, int width) {
    std::vector<Element*> ancestors = item.ancestors;
    std::swap(ancestorStack, ancestors);
    std::wstring savedHref = currentHref;
    Element* savedForm = currentForm;
    currentHref = item.href;
    currentForm = item.form;
    const std::wstring& tag = item.el->tag;
    if (tag == L"input" || tag == L"button" || tag == L"select") layoutControl(item.el, x, y, width, item.style);
    else layoutBlockChild(item.el, x, y, width, item.style);
    currentHref = savedHref;
    currentForm = savedForm;
    std::swap(ancestorStack, ancestors);
}

void LayoutRoot::placeFloat(const FloatItem& item, int cbX, int cbW, int y) {
    if (!floatCtx_) return;
    ComputedStyle s = item.style;
    if (s.display == Display::Inline) s.display = Display::Block; // a float is blockified
    const std::wstring& tag = item.el->tag;
    const bool replaced = tag == L"img" || tag == L"svg" || tag == L"input" || tag == L"button" || tag == L"select";

    // Laid out in the context it was met in.
    std::vector<Element*> ancestors = item.ancestors;
    std::swap(ancestorStack, ancestors);
    std::wstring savedHref = currentHref;
    Element* savedForm = currentForm;
    currentHref = item.href;
    currentForm = item.form;
    int savedCH = containingHeight_;
    containingHeight_ = -1;

    // Its width (margin box): shrink-to-fit for a block with no width. An
    // image, a control or a table sizes itself - laid out in the whole
    // containing block, it's as wide as its boxes reach.
    const bool sizesItself = replaced || s.display == Display::Table;
    int width = sizesItself ? cbW : s.width >= 0 ? cbW : shrinkToFitWidth(item.el, s, cbW);
    int height = 0;
    std::vector<LayoutBox> laid = layoutItemDetached(item.el, width, s, height);
    if (sizesItself) {
        int right = 0;
        for (const auto& b : laid) if (!b.anchor) right = std::max(right, b.x + b.width);
        if (replaced) { // layoutImage/layoutControl leave horizontal margins out
            for (auto& b : laid) b.x += s.marginLeft;
            right += s.marginLeft;
        }
        width = right + s.marginRight;
    }
    else if (s.width >= 0) {
        int chrome = s.boxSizing == BoxSizing::BorderBox ? 0 : s.paddingLeft + s.paddingRight + 2 * s.borderWidth;
        int w = s.width;
        if (s.maxWidth >= 0) w = std::min(w, s.maxWidth);
        if (s.minWidth >= 0) w = std::max(w, s.minWidth);
        width = s.marginLeft + w + chrome + s.marginRight;
    }

    containingHeight_ = savedCH;
    currentHref = savedHref;
    currentForm = savedForm;
    std::swap(ancestorStack, ancestors);

    // Where it goes: as high as allowed, then down past floats until it fits.
    // While measuring a content width, a right float goes on the left too:
    // at the far right edge it would make the content seem to reach all
    // the way across; beside the rest, it adds just its own width, as it
    // does to a max-content width in CSS.
    const bool left = s.floatSide != ComputedStyle::Float::Right || measuring_;
    int top = std::max(y, floatCtx_->lastTop);
    int c = clearance(s.clear);
    if (c != INT_MIN) top = std::max(top, c);
    int fx = left ? cbX : cbX + cbW - width;
    for (;;) {
        int l, r, next;
        if (!spaceBeside(top, std::max(height, 1), cbX, cbW, l, r, next) || r - l >= width || next <= top) {
            fx = left ? l : r - width;
            break;
        }
        top = next;
    }

    for (auto& b : laid) {
        moveBox(b, fx, top);
        if (b.paintKey == 0) b.paintKey = 1; // above in-flow backgrounds (see LayoutBox::paintKey)
        boxes.push_back(std::move(b));
    }
    floatCtx_->floats.push_back({ left, fx, top, width, height });
    floatCtx_->lastTop = top;
}

// --- Tables ----------------------------------------------------------------------
//
// The automatic table layout browsers use, simplified:
// 1. buildTable reads the table's rows and cells (row groups, a <thead>
//    first and a <tfoot> last, anonymous rows for cells with no <tr>) and
//    places each cell in the grid, honouring colspan/rowspan.
// 2. It measures every cell twice: laid out as narrow as it goes (its
//    min-content width - the widest word, image or fixed-width box) and
//    with as much room as the table has (its max-content width). A
//    column's min/max is the largest of its cells'; a spanning cell's
//    shortfall is shared among the columns it spans. An explicit width on
//    a cell or <col> replaces the max-content width.
// 3. An auto-width table is as wide as the sum of its columns' max widths,
//    if that fits, else as wide as is available (but never below the sum
//    of their min widths) - see layoutBlockChild.
// 4. layoutTable shares that width out (columnWidths), lays each cell out
//    at its column width, makes each row as tall as its tallest cell,
//    stretches every cell's background to its row and places its content
//    by vertical-align.
// Not done: table-layout: fixed, percentage columns as such (a % width is
// resolved against the space available and treated like a fixed one),
// collapsed borders of different widths/colours (cells simply overlap
// their neighbours' borders), caption-side: bottom, and text that sits
// directly between table parts (dropped).

// Adds `excess` px to columns [from, from + count), in proportion to
// `weights` (equally when they're all 0), rounding so the parts add up.
static void shareOut(std::vector<int>& cols, int from, int count, int excess, std::vector<int> weights) {
    long long total = 0;
    for (int i = from; i < from + count; i++) total += std::max(weights[i], 0);
    double exact = 0;
    int given = 0;
    for (int i = from; i < from + count; i++) {
        exact += total > 0 ? (double)excess * std::max(weights[i], 0) / (double)total : (double)excess / count;
        int share = (int)std::lround(exact) - given;
        cols[i] += share;
        given += share;
    }
}

// Splits `target` px among columns: each gets at least its min; up to the
// sum of the maxes, every column moves the same fraction of the way from
// its min to its max; past that, the extra goes to the columns without an
// explicit width, in proportion to their max widths.
static std::vector<int> columnWidths(int target, const std::vector<int>& mins, const std::vector<int>& maxs,
                                     const std::vector<bool>& fixed) {
    const int n = (int)mins.size();
    long long sumMin = 0, sumMax = 0;
    for (int i = 0; i < n; i++) { sumMin += mins[i]; sumMax += maxs[i]; }
    std::vector<int> out = mins;
    if (target <= sumMin) return out;
    if (target <= sumMax) {
        std::vector<int> room(n);
        for (int i = 0; i < n; i++) room[i] = maxs[i] - mins[i];
        shareOut(out, 0, n, (int)(target - sumMin), room);
        return out;
    }
    out = maxs;
    bool anyFlexible = false, anyFlexibleWidth = false;
    for (int i = 0; i < n; i++)
        if (!fixed[i]) { anyFlexible = true; if (maxs[i] > 0) anyFlexibleWidth = true; }
    std::vector<int> weights(n);
    for (int i = 0; i < n; i++)
        weights[i] = !anyFlexible ? maxs[i] : fixed[i] ? 0 : anyFlexibleWidth ? maxs[i] : 1;
    shareOut(out, 0, n, (int)(target - sumMax), weights);
    return out;
}

std::unique_ptr<LayoutRoot::TableModel> LayoutRoot::buildTable(Element* table, const ComputedStyle& style, int available) {
    auto model = std::make_unique<TableModel>();
    TableModel& m = *model;
    ancestorStack.push_back(table);

    // Calls `visit` for each element child of `parent` that takes part in
    // the table, with its computed style. A <form> between table parts
    // (<table><form><tr>...) is looked through: its children count as
    // `parent`'s own, and it's the form their controls belong to.
    using Visit = std::function<void(Element*, ComputedStyle&, Element*)>;
    std::function<void(Element*, const ComputedStyle&, Element*, const Visit&)> eachPart =
        [&](Element* parent, const ComputedStyle& ps, Element* form, const Visit& visit) {
        for (auto& child : parent->children) {
            if (child->type != Node::ELEMENT) continue; // text between table parts isn't shown
            auto* e = static_cast<Element*>(child.get());
            if (e->tag == L"head" || e->tag == L"script" || e->tag == L"style" || e->tag == L"title" ||
                e->tag == L"meta" || e->tag == L"link" || e->tag == L"base" || e->tag == L"template")
                continue;
            if (e->tag == L"input" && lowerCase(getAttr(e, L"type")) == L"hidden") continue;
            ComputedStyle cs = computeStyle(e, ps.fontSize, available, ps.visuallyHidden, ps.paint);
            if (e->tag == L"form" && cs.display == Display::Block) {
                ancestorStack.push_back(e);
                eachPart(e, cs, e, visit);
                ancestorStack.pop_back();
                continue;
            }
            if (cs.position == ComputedStyle::Position::Absolute || cs.position == ComputedStyle::Position::Fixed) {
                if (cs.display != Display::None) deferOutOfFlow(e, cs, 0, 0, false);
                continue;
            }
            visit(e, cs, form);
        }
    };

    // The table's own children: captions, column widths, and the parts
    // rows come from - header groups first, then the body in document
    // order, then footer groups.
    struct Part {
        Element* el;
        ComputedStyle style;
        Element* form;
        std::vector<Element*> ancestors;
    };
    std::vector<Part> sections[3];
    std::vector<int> colWidths; // from <col>/<colgroup>; -1 where none is given
    auto addCols = [&](Element* e, const ComputedStyle& cs) {
        int span = 1;
        try { span = std::clamp(std::stoi(getAttr(e, L"span", L"1")), 1, 1000); } catch (...) {}
        int w = cs.width < 0 ? -1 : cs.boxSizing == BoxSizing::BorderBox ? cs.width
                                  : cs.width + cs.paddingLeft + cs.paddingRight + 2 * cs.borderWidth;
        colWidths.insert(colWidths.end(), span, w);
    };
    eachPart(table, style, nullptr, [&](Element* e, ComputedStyle& cs, Element* form) {
        if (e->tag == L"col") { addCols(e, cs); return; }
        if (e->tag == L"colgroup") {
            bool any = false;
            ancestorStack.push_back(e);
            for (auto& child : e->children) {
                if (child->type != Node::ELEMENT) continue;
                auto* col = static_cast<Element*>(child.get());
                if (col->tag != L"col") continue;
                any = true;
                addCols(col, computeStyle(col, cs.fontSize, available, cs.visuallyHidden, cs.paint));
            }
            ancestorStack.pop_back();
            if (!any) addCols(e, cs);
            return;
        }
        if (cs.display == Display::None) return;
        if (cs.display == Display::TableCaption) { m.captions.push_back({ e, cs }); return; }
        int s = cs.display == Display::TableHeaderGroup ? 0 : cs.display == Display::TableFooterGroup ? 2 : 1;
        sections[s].push_back({ e, cs, form, ancestorStack });
    });

    // Rows and cells. Each row group is a section of its own, and so is
    // each run of rows directly in the table; rowspan stops at its end.
    int section = 0;
    int anonRow = -1; // the open anonymous row that loose cells go into
    auto addRow = [&](Element* el, const ComputedStyle& rs, int group) {
        TableRow row;
        row.el = el;
        row.style = rs;
        row.group = group;
        row.section = section;
        if (el) row.minHeight = std::max(rs.height, rs.minHeight);
        m.rows.push_back(std::move(row));
        return (int)m.rows.size() - 1;
    };
    auto addCell = [&](Element* e, ComputedStyle cs, Element* form, int row) {
        TableCell c;
        c.el = e;
        c.form = form;
        c.row = row;
        c.ancestors = ancestorStack;
        auto spanAttr = [&](const wchar_t* name, int lo, int hi) {
            auto it = e->attrs.find(name);
            if (it == e->attrs.end()) return 1;
            try { return std::clamp(std::stoi(it->second), lo, hi); } catch (...) { return 1; }
        };
        c.colSpan = spanAttr(L"colspan", 1, 1000);
        c.rowSpan = spanAttr(L"rowspan", 0, 65534); // 0: to the end of its section
        if (cs.width >= 0)
            c.fixedWidth = cs.boxSizing == BoxSizing::BorderBox ? cs.width
                         : cs.width + cs.paddingLeft + cs.paddingRight + 2 * cs.borderWidth;
        c.valign = cs.verticalAlign;
        // The table decides the cell's size; its own width and height only
        // feed into its column's and row's.
        cs.width = cs.minWidth = cs.maxWidth = -1;
        if (cs.height >= 0) cs.minHeight = std::max(cs.minHeight, cs.height);
        cs.height = -1;
        cs.marginTop = cs.marginBottom = cs.marginLeft = cs.marginRight = 0;
        cs.marginLeftAuto = cs.marginRightAuto = false;
        if (cs.display != Display::Grid && cs.display != Display::Flex && cs.display != Display::Table)
            cs.display = Display::TableCell;
        c.style = std::move(cs);
        m.cells.push_back(std::move(c));
    };
    // One row, or a cell (or anything else) outside a row, which joins an
    // anonymous row with the loose cells next to it.
    auto addPart = [&](Element* e, ComputedStyle& cs, Element* form, int group, const ComputedStyle& parentStyle) {
        if (cs.display == Display::TableRow) {
            anonRow = -1;
            int row = addRow(e, cs, group);
            ancestorStack.push_back(e);
            eachPart(e, cs, form, [&](Element* cell, ComputedStyle& cellStyle, Element* f) {
                if (cellStyle.display != Display::None) addCell(cell, cellStyle, f, row);
            });
            ancestorStack.pop_back();
        }
        else {
            if (anonRow < 0) anonRow = addRow(nullptr, parentStyle, group);
            addCell(e, cs, form, anonRow);
        }
    };
    bool inLooseRun = false;
    for (auto& parts : sections) {
        for (Part& p : parts) {
            std::swap(ancestorStack, p.ancestors);
            if (p.style.display == Display::TableRowGroup || p.style.display == Display::TableHeaderGroup ||
                p.style.display == Display::TableFooterGroup) {
                section++;
                anonRow = -1;
                inLooseRun = false;
                m.groups.push_back({ p.el, p.style });
                int group = (int)m.groups.size() - 1;
                ancestorStack.push_back(p.el);
                eachPart(p.el, p.style, p.form, [&](Element* e, ComputedStyle& cs, Element* form) {
                    if (cs.display != Display::None) addPart(e, cs, form, group, p.style);
                });
                ancestorStack.pop_back();
                anonRow = -1;
            }
            else {
                if (!inLooseRun) { section++; anonRow = -1; inLooseRun = true; }
                addPart(p.el, p.style, p.form, -1, style);
            }
            std::swap(ancestorStack, p.ancestors);
        }
    }

    // Grid placement: each cell takes the next column in its row that a
    // rowspan from above hasn't already filled.
    std::vector<std::vector<char>> taken(m.rows.size());
    int lastRow = -1, cursor = 0;
    for (TableCell& c : m.cells) {
        if (c.row != lastRow) { lastRow = c.row; cursor = 0; }
        while (cursor < (int)taken[c.row].size() && taken[c.row][cursor]) cursor++;
        c.col = cursor;
        int want = c.rowSpan == 0 ? INT_MAX : c.rowSpan;
        int end = c.row;
        while (end + 1 < (int)m.rows.size() && m.rows[end + 1].section == m.rows[c.row].section && end + 1 - c.row < want) end++;
        c.rowSpan = end - c.row + 1;
        for (int r = c.row; r <= end; r++) {
            if ((int)taken[r].size() < cursor + c.colSpan) taken[r].resize(cursor + c.colSpan, 0);
            std::fill(taken[r].begin() + cursor, taken[r].begin() + cursor + c.colSpan, 1);
        }
        cursor += c.colSpan;
        m.numCols = std::max(m.numCols, cursor);
    }

    // Spacing: border-spacing, or for border-collapse, neighbouring cells
    // overlapping by their border (and the outer ones overlapping the
    // table's own border).
    if (style.borderCollapse) {
        int cellBorder = 0;
        for (const TableCell& c : m.cells) cellBorder = std::max(cellBorder, c.style.borderWidth);
        m.gapX = m.gapY = -cellBorder;
        m.edgeX = m.edgeY = -std::min(style.borderWidth, cellBorder);
    }
    else {
        m.gapX = m.edgeX = style.borderSpacingX;
        m.gapY = m.edgeY = style.borderSpacingY;
    }

    // Measure every cell (see the overview above). How far right a
    // measuring layout's content reaches: text, images and controls; when
    // laid out narrow, every box (an auto-width block is then only as wide
    // as its own content); when wide, only nested tables and cells besides
    // content, since other blocks just fill the width they're given.
    auto rightEdge = [](const std::vector<LayoutBox>& laid, const TableCell& c, bool narrow) {
        int right = 0;
        for (const auto& b : laid) {
            if (b.anchor) continue; // marks a position; has no size of its own
            bool text = !b.text.empty() && b.control == LayoutBox::NoControl;
            bool content = text || !b.imageSrc.empty() || b.control != LayoutBox::NoControl;
            if (!content) {
                if (b.el == c.el) continue; // the cell's own background
                if (!narrow && !(b.el && (b.el->tag == L"table" || b.el->tag == L"td" || b.el->tag == L"th"))) continue;
            }
            // Text is drawn 4px into its box and wraps 4px short of the right edge.
            right = std::max(right, b.x + b.width + (text ? 8 : !b.imageSrc.empty() ? 4 : 0));
        }
        const ComputedStyle& s = c.style;
        return std::max(right + s.paddingRight + s.borderWidth, s.paddingLeft + s.paddingRight + 2 * s.borderWidth);
    };
    // Absolute elements met while measuring go into a throwaway containing
    // block - they're registered for real when the cells are laid out at
    // their final size.
    ContainingBlock scratch;
    positioned_.push_back(&scratch);
    const bool wasMeasuring = measuring_;
    measuring_ = true;
    for (TableCell& c : m.cells) {
        c.minWidth = rightEdge(layoutCell(c, 0).first, c, true);
        if (c.fixedWidth >= 0) c.maxWidth = std::max(c.minWidth, c.fixedWidth);
        else c.maxWidth = std::max(rightEdge(layoutCell(c, available).first, c, false), c.minWidth);
    }
    measuring_ = wasMeasuring;
    positioned_.pop_back();

    // Column min/max widths: single-column cells and <col> widths first,
    // then whatever spanning cells need beyond that, narrowest span first.
    m.colMin.assign(m.numCols, 0);
    m.colMax.assign(m.numCols, 0);
    m.colFixed.assign(m.numCols, false);
    for (int i = 0; i < std::min((int)colWidths.size(), m.numCols); i++)
        if (colWidths[i] >= 0) { m.colMax[i] = colWidths[i]; m.colFixed[i] = true; }
    std::vector<const TableCell*> spanning;
    for (const TableCell& c : m.cells) {
        if (c.colSpan > 1) { spanning.push_back(&c); continue; }
        m.colMin[c.col] = std::max(m.colMin[c.col], c.minWidth);
        m.colMax[c.col] = std::max(m.colMax[c.col], c.maxWidth);
        if (c.fixedWidth >= 0) m.colFixed[c.col] = true;
    }
    for (int i = 0; i < m.numCols; i++) m.colMax[i] = std::max(m.colMax[i], m.colMin[i]);
    std::stable_sort(spanning.begin(), spanning.end(), [](const TableCell* a, const TableCell* b) { return a->colSpan < b->colSpan; });
    for (const TableCell* c : spanning) {
        int haveMin = m.gapX * (c->colSpan - 1), haveMax = haveMin;
        for (int k = c->col; k < c->col + c->colSpan; k++) { haveMin += m.colMin[k]; haveMax += m.colMax[k]; }
        if (c->minWidth > haveMin) shareOut(m.colMin, c->col, c->colSpan, c->minWidth - haveMin, m.colMax);
        if (c->maxWidth > haveMax) shareOut(m.colMax, c->col, c->colSpan, c->maxWidth - haveMax, m.colMax);
        for (int k = c->col; k < c->col + c->colSpan; k++) m.colMax[k] = std::max(m.colMax[k], m.colMin[k]);
    }
    if (m.numCols > 0) {
        long long sumMin = 0, sumMax = 0;
        for (int i = 0; i < m.numCols; i++) { sumMin += m.colMin[i]; sumMax += m.colMax[i]; }
        int spacing = m.gapX * (m.numCols - 1) + 2 * m.edgeX;
        m.minWidth = (int)std::max(sumMin + spacing, 0LL);
        m.maxWidth = (int)std::max(sumMax + spacing, 0LL);
    }

    ancestorStack.pop_back();
    return model;
}

std::pair<std::vector<LayoutBox>, int> LayoutRoot::layoutCell(const TableCell& cell, int width) {
    const bool cache = measuring_;
    if (cache) {
        auto it = cellCache_.find({ cell.el, width });
        if (it != cellCache_.end()) return it->second;
    }
    // Laid out in its own context: its ancestors (for selectors), its
    // form, and no definite height for height: % to resolve against.
    std::vector<Element*> ancestors = cell.ancestors;
    std::swap(ancestorStack, ancestors);
    Element* savedForm = currentForm;
    if (cell.form) currentForm = cell.form;
    int savedCH = containingHeight_;
    containingHeight_ = -1;

    std::pair<std::vector<LayoutBox>, int> result;
    result.first = layoutItemDetached(cell.el, width, cell.style, result.second);

    containingHeight_ = savedCH;
    currentForm = savedForm;
    std::swap(ancestorStack, ancestors);
    if (cache) cellCache_.emplace(std::make_pair((const Element*)cell.el, width), result);
    return result;
}

void LayoutRoot::layoutTable(Element* el, int x, int& y, int contentWidth, const ComputedStyle& style, const TableModel& t) {
    (void)el;
    (void)style;
    if (t.numCols == 0 || t.rows.empty()) return;

    const int n = t.numCols;
    std::vector<int> colW = columnWidths(contentWidth - t.gapX * (n - 1) - 2 * t.edgeX, t.colMin, t.colMax, t.colFixed);
    std::vector<int> colX(n);
    int cx = x + t.edgeX;
    for (int i = 0; i < n; i++) { colX[i] = cx; cx += colW[i] + t.gapX; }

    // Every cell at the width of the columns it spans.
    std::vector<std::pair<std::vector<LayoutBox>, int>> laid;
    laid.reserve(t.cells.size());
    for (const TableCell& c : t.cells) {
        int w = t.gapX * (c.colSpan - 1);
        for (int k = c.col; k < c.col + c.colSpan; k++) w += colW[k];
        laid.push_back(layoutCell(c, std::max(w, 0)));
    }

    // Row heights: the tallest cell in the row (or the row's own height);
    // a cell spanning rows that are too short for it adds the difference
    // to the last of them.
    const int numRows = (int)t.rows.size();
    std::vector<int> rowH(numRows);
    for (int r = 0; r < numRows; r++) rowH[r] = std::max(t.rows[r].minHeight, 0);
    for (size_t i = 0; i < t.cells.size(); i++)
        if (t.cells[i].rowSpan == 1) rowH[t.cells[i].row] = std::max(rowH[t.cells[i].row], laid[i].second);
    auto spanHeight = [&](int row, int span) {
        int h = t.gapY * (span - 1);
        for (int r = row; r < row + span; r++) h += rowH[r];
        return h;
    };
    for (size_t i = 0; i < t.cells.size(); i++) {
        const TableCell& c = t.cells[i];
        if (c.rowSpan == 1) continue;
        int have = spanHeight(c.row, c.rowSpan);
        if (laid[i].second > have) rowH[c.row + c.rowSpan - 1] += laid[i].second - have;
    }
    std::vector<int> rowY(numRows);
    int ry = y + t.edgeY;
    for (int r = 0; r < numRows; r++) { rowY[r] = ry; ry += rowH[r] + t.gapY; }

    // Row group and row backgrounds, under the cells, across every column.
    const int gridLeft = colX[0], gridWidth = colX[n - 1] + colW[n - 1] - colX[0];
    auto paintBand = [&](Element* e, const ComputedStyle& s, int top, int height) {
        if (s.background.empty() && s.backgrounds.empty()) return;
        LayoutBox box;
        box.x = gridLeft;
        box.y = top;
        box.width = gridWidth;
        box.height = height;
        box.background = s.background;
        box.backgrounds = s.backgrounds;
        box.el = e;
        box.visuallyHidden = s.visuallyHidden;
        boxes.push_back(std::move(box));
    };
    for (int g = 0; g < (int)t.groups.size(); g++) {
        int first = -1, last = -1;
        for (int r = 0; r < numRows; r++)
            if (t.rows[r].group == g) { if (first < 0) first = r; last = r; }
        if (first >= 0) paintBand(t.groups[g].first, t.groups[g].second, rowY[first], rowY[last] + rowH[last] - rowY[first]);
    }
    for (int r = 0; r < numRows; r++)
        if (t.rows[r].el) paintBand(t.rows[r].el, t.rows[r].style, rowY[r], rowH[r]);

    // The cells: background stretched to the rows they span, content
    // placed by vertical-align (the cell's own, else its row's, else its
    // row group's, else middle).
    for (size_t i = 0; i < t.cells.size(); i++) {
        const TableCell& c = t.cells[i];
        const TableRow& row = t.rows[c.row];
        VAlign va = c.valign;
        if (va == VAlign::Unset) va = row.style.verticalAlign;
        if (va == VAlign::Unset && row.group >= 0) va = t.groups[row.group].second.verticalAlign;
        int height = spanHeight(c.row, c.rowSpan);
        int free = std::max(height - laid[i].second, 0);
        int shift = va == VAlign::Bottom ? free : va == VAlign::Top ? 0 : free / 2;
        bool first = true;
        for (LayoutBox& b : laid[i].first) {
            // layoutBlockChild puts a cell's own background box first.
            bool own = first && b.el == c.el && b.text.empty() && b.imageSrc.empty() && b.control == LayoutBox::NoControl;
            first = false;
            if (own) b.height = height;
            moveBox(b, colX[c.col], rowY[c.row] + (own ? 0 : shift));
            boxes.push_back(std::move(b));
        }
    }

    y = rowY[numRows - 1] + rowH[numRows - 1] + t.edgeY;
}
