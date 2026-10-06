// Engine.cpp
#define NOMINMAX
#include "Engine.h"
#include "HTMLParser.h"
#include "CSS.h"
#include "Renderer.h"
#include "Fetcher.h"
#include "JSEngine.h"
#include "DevConsole.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <string>
#include <sstream>
#include <unordered_map>
#include <vector>

// ------------------ Colors ------------------

// The CSS named colors (CSS Color Module Level 4), as 0xRRGGBB.
static const std::unordered_map<std::wstring, unsigned>& namedColors() {
    static const std::unordered_map<std::wstring, unsigned> kNames = {
        {L"aliceblue",0xf0f8ff},{L"antiquewhite",0xfaebd7},{L"aqua",0x00ffff},{L"aquamarine",0x7fffd4},
        {L"azure",0xf0ffff},{L"beige",0xf5f5dc},{L"bisque",0xffe4c4},{L"black",0x000000},
        {L"blanchedalmond",0xffebcd},{L"blue",0x0000ff},{L"blueviolet",0x8a2be2},{L"brown",0xa52a2a},
        {L"burlywood",0xdeb887},{L"cadetblue",0x5f9ea0},{L"chartreuse",0x7fff00},{L"chocolate",0xd2691e},
        {L"coral",0xff7f50},{L"cornflowerblue",0x6495ed},{L"cornsilk",0xfff8dc},{L"crimson",0xdc143c},
        {L"cyan",0x00ffff},{L"darkblue",0x00008b},{L"darkcyan",0x008b8b},{L"darkgoldenrod",0xb8860b},
        {L"darkgray",0xa9a9a9},{L"darkgreen",0x006400},{L"darkgrey",0xa9a9a9},{L"darkkhaki",0xbdb76b},
        {L"darkmagenta",0x8b008b},{L"darkolivegreen",0x556b2f},{L"darkorange",0xff8c00},{L"darkorchid",0x9932cc},
        {L"darkred",0x8b0000},{L"darksalmon",0xe9967a},{L"darkseagreen",0x8fbc8f},{L"darkslateblue",0x483d8b},
        {L"darkslategray",0x2f4f4f},{L"darkslategrey",0x2f4f4f},{L"darkturquoise",0x00ced1},{L"darkviolet",0x9400d3},
        {L"deeppink",0xff1493},{L"deepskyblue",0x00bfff},{L"dimgray",0x696969},{L"dimgrey",0x696969},
        {L"dodgerblue",0x1e90ff},{L"firebrick",0xb22222},{L"floralwhite",0xfffaf0},{L"forestgreen",0x228b22},
        {L"fuchsia",0xff00ff},{L"gainsboro",0xdcdcdc},{L"ghostwhite",0xf8f8ff},{L"gold",0xffd700},
        {L"goldenrod",0xdaa520},{L"gray",0x808080},{L"green",0x008000},{L"greenyellow",0xadff2f},
        {L"grey",0x808080},{L"honeydew",0xf0fff0},{L"hotpink",0xff69b4},{L"indianred",0xcd5c5c},
        {L"indigo",0x4b0082},{L"ivory",0xfffff0},{L"khaki",0xf0e68c},{L"lavender",0xe6e6fa},
        {L"lavenderblush",0xfff0f5},{L"lawngreen",0x7cfc00},{L"lemonchiffon",0xfffacd},{L"lightblue",0xadd8e6},
        {L"lightcoral",0xf08080},{L"lightcyan",0xe0ffff},{L"lightgoldenrodyellow",0xfafad2},{L"lightgray",0xd3d3d3},
        {L"lightgreen",0x90ee90},{L"lightgrey",0xd3d3d3},{L"lightpink",0xffb6c1},{L"lightsalmon",0xffa07a},
        {L"lightseagreen",0x20b2aa},{L"lightskyblue",0x87cefa},{L"lightslategray",0x778899},{L"lightslategrey",0x778899},
        {L"lightsteelblue",0xb0c4de},{L"lightyellow",0xffffe0},{L"lime",0x00ff00},{L"limegreen",0x32cd32},
        {L"linen",0xfaf0e6},{L"magenta",0xff00ff},{L"maroon",0x800000},{L"mediumaquamarine",0x66cdaa},
        {L"mediumblue",0x0000cd},{L"mediumorchid",0xba55d3},{L"mediumpurple",0x9370db},{L"mediumseagreen",0x3cb371},
        {L"mediumslateblue",0x7b68ee},{L"mediumspringgreen",0x00fa9a},{L"mediumturquoise",0x48d1cc},{L"mediumvioletred",0xc71585},
        {L"midnightblue",0x191970},{L"mintcream",0xf5fffa},{L"mistyrose",0xffe4e1},{L"moccasin",0xffe4b5},
        {L"navajowhite",0xffdead},{L"navy",0x000080},{L"oldlace",0xfdf5e6},{L"olive",0x808000},
        {L"olivedrab",0x6b8e23},{L"orange",0xffa500},{L"orangered",0xff4500},{L"orchid",0xda70d6},
        {L"palegoldenrod",0xeee8aa},{L"palegreen",0x98fb98},{L"paleturquoise",0xafeeee},{L"palevioletred",0xdb7093},
        {L"papayawhip",0xffefd5},{L"peachpuff",0xffdab9},{L"peru",0xcd853f},{L"pink",0xffc0cb},
        {L"plum",0xdda0dd},{L"powderblue",0xb0e0e6},{L"purple",0x800080},{L"rebeccapurple",0x663399},
        {L"red",0xff0000},{L"rosybrown",0xbc8f8f},{L"royalblue",0x4169e1},{L"saddlebrown",0x8b4513},
        {L"salmon",0xfa8072},{L"sandybrown",0xf4a460},{L"seagreen",0x2e8b57},{L"seashell",0xfff5ee},
        {L"sienna",0xa0522d},{L"silver",0xc0c0c0},{L"skyblue",0x87ceeb},{L"slateblue",0x6a5acd},
        {L"slategray",0x708090},{L"slategrey",0x708090},{L"snow",0xfffafa},{L"springgreen",0x00ff7f},
        {L"steelblue",0x4682b4},{L"tan",0xd2b48c},{L"teal",0x008080},{L"thistle",0xd8bfd8},
        {L"tomato",0xff6347},{L"turquoise",0x40e0d0},{L"violet",0xee82ee},{L"wheat",0xf5deb3},
        {L"white",0xffffff},{L"whitesmoke",0xf5f5f5},{L"yellow",0xffff00},{L"yellowgreen",0x9acd32},
    };
    return kNames;
}

static int hexDigit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    c = (wchar_t)towlower(c);
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    return -1;
}

// One rgb()/rgba() argument: a number (0-255, or 0-1 for alpha) or a
// percentage. False if `t` isn't entirely a number/percentage.
static bool parseColorComponent(const std::wstring& t, bool isAlpha, float& out) {
    if (t.empty()) return false;
    size_t used = 0;
    float v;
    try { v = std::stof(t, &used); } catch (...) { return false; }
    bool percent = used < t.size() && t[used] == L'%';
    if (used + (percent ? 1 : 0) != t.size()) return false;
    if (percent) v /= 100.0f;
    else if (!isAlpha) v /= 255.0f;
    out = std::clamp(v, 0.0f, 1.0f);
    return true;
}

bool tryParseColor(const std::wstring& raw, Color& out) {
    // Trim, lowercase, and drop a trailing "!important" (the declaration
    // parser leaves it on the value).
    std::wstring s;
    for (wchar_t c : raw) s += (wchar_t)towlower(c);
    size_t bang = s.find(L'!');
    if (bang != std::wstring::npos) s.erase(bang);
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t lead = 0;
    while (lead < s.size() && iswspace(s[lead])) lead++;
    s.erase(0, lead);
    if (s.empty()) return false;

    if (s[0] == L'#') {
        std::vector<int> d;
        for (size_t i = 1; i < s.size(); i++) {
            int h = hexDigit(s[i]);
            if (h < 0) return false;
            d.push_back(h);
        }
        float c[4] = { 1, 1, 1, 1 };
        if (d.size() == 3 || d.size() == 4) {       // #rgb / #rgba: each digit doubled
            for (size_t i = 0; i < d.size(); i++) c[i] = (d[i] * 17) / 255.0f;
        } else if (d.size() == 6 || d.size() == 8) { // #rrggbb / #rrggbbaa
            for (size_t i = 0; i < d.size() / 2; i++) c[i] = (d[2 * i] * 16 + d[2 * i + 1]) / 255.0f;
        } else {
            return false;
        }
        out = { c[0], c[1], c[2], c[3] };
        return true;
    }

    if (s.rfind(L"rgb(", 0) == 0 || s.rfind(L"rgba(", 0) == 0) {
        size_t open = s.find(L'('), close = s.rfind(L')');
        if (close == std::wstring::npos || close < open) return false;
        // Both the legacy "r, g, b[, a]" and the modern "r g b [/ a]"
        // syntaxes: treat commas and '/' as whitespace, then split.
        std::wstring args = s.substr(open + 1, close - open - 1);
        for (auto& c : args) if (c == L',' || c == L'/') c = L' ';
        std::wistringstream ss(args);
        std::vector<std::wstring> parts;
        for (std::wstring t; ss >> t;) parts.push_back(t);
        if (parts.size() != 3 && parts.size() != 4) return false;
        float c[4] = { 0, 0, 0, 1 };
        for (size_t i = 0; i < parts.size(); i++)
            if (!parseColorComponent(parts[i], i == 3, c[i])) return false;
        out = { c[0], c[1], c[2], c[3] };
        return true;
    }

    if (s.rfind(L"hsl(", 0) == 0 || s.rfind(L"hsla(", 0) == 0) {
        size_t open = s.find(L'('), close = s.rfind(L')');
        if (close == std::wstring::npos || close < open) return false;
        std::wstring args = s.substr(open + 1, close - open - 1);
        for (auto& c : args) if (c == L',' || c == L'/') c = L' ';
        std::wistringstream ss(args);
        std::vector<std::wstring> parts;
        for (std::wstring t; ss >> t;) parts.push_back(t);
        if (parts.size() != 3 && parts.size() != 4) return false;

        // Hue: a bare number is degrees; deg/rad/grad/turn units too.
        size_t used = 0;
        float h;
        try { h = std::stof(parts[0], &used); } catch (...) { return false; }
        std::wstring unit = parts[0].substr(used);
        if (unit == L"rad") h = h * 180.0f / 3.14159265f;
        else if (unit == L"grad") h = h * 0.9f;
        else if (unit == L"turn") h = h * 360.0f;
        else if (!unit.empty() && unit != L"deg") return false;
        h = std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f) / 60.0f;

        // Saturation and lightness: percentages (a bare number is read as
        // one too, as CSS Color 4 allows).
        float sl[2];
        for (int i = 0; i < 2; i++) {
            const std::wstring& t = parts[i + 1];
            try { sl[i] = std::stof(t, &used); } catch (...) { return false; }
            if (used != t.size() && !(used + 1 == t.size() && t[used] == L'%')) return false;
            sl[i] = std::clamp(sl[i] / 100.0f, 0.0f, 1.0f);
        }
        float alpha = 1;
        if (parts.size() == 4 && !parseColorComponent(parts[3], true, alpha)) return false;

        float chroma = (1 - std::fabs(2 * sl[1] - 1)) * sl[0];
        float x = chroma * (1 - std::fabs(std::fmod(h, 2.0f) - 1));
        float r = 0, g = 0, b = 0;
        switch ((int)h) {
        case 0: r = chroma; g = x; break;
        case 1: r = x; g = chroma; break;
        case 2: g = chroma; b = x; break;
        case 3: g = x; b = chroma; break;
        case 4: r = x; b = chroma; break;
        default: r = chroma; b = x; break;
        }
        float m = sl[1] - chroma / 2;
        out = { r + m, g + m, b + m, alpha };
        return true;
    }

    if (s == L"transparent") { out = { 0, 0, 0, 0 }; return true; }

    auto it = namedColors().find(s);
    if (it == namedColors().end()) return false;
    unsigned v = it->second;
    out = { ((v >> 16) & 0xff) / 255.0f, ((v >> 8) & 0xff) / 255.0f, (v & 0xff) / 255.0f, 1.0f };
    return true;
}

Color parseColor(const std::wstring& str) {
    Color c;
    if (tryParseColor(str, c)) return c;
    return { 0.94f, 0.94f, 0.94f, 1.0f }; // default light gray
}

// ------------------ Engine Implementation ------------------

// data: URIs carry their payload inline - resolving them against the page's
// URL makes no sense (and resolveUrl rejects non-http schemes anyway, since
// that's correct for <a href>), so they're used as-is; everything else goes
// through the normal base-URL resolution links use.
static std::wstring resolveImageSrc(const std::wstring& baseUrl, const std::wstring& src) {
    if (src.rfind(L"data:", 0) == 0) return src;
    return resolveUrl(baseUrl, src);
}

Engine::Engine(int w, int h)
    : width(w), height(h) {
    layoutRoot.viewportWidth = width;
    layoutRoot.hover = &hoverSet;
    layoutRoot.measureText = [this](const std::wstring& text, int fontSize, bool bold, bool italic, const std::wstring* family) {
        return measurer ? measurePageText(*measurer, text, (float)fontSize, bold, italic, family)
                        : text.size() * fontSize * 0.55f;
    };
    layoutRoot.loadImage = [this](const std::wstring& src, int& w, int& h) {
        if (!measurer) return false;
        std::wstring resolved = resolveImageSrc(pageBaseUrl, src);
        return !resolved.empty() && measurer->preloadImage(resolved, w, h);
    };
}

Engine::~Engine() {}

void Engine::setRenderer(Renderer* r) {
    measurer = r;
    doLayout(); // re-wrap with real text metrics
}

void Engine::loadHTML(const std::wstring& html, const std::wstring& baseUrl) {
    pageBaseUrl = baseUrl;
    if (measurer) measurer->setPageUrl(baseUrl); // the referrer for this page's images
    parseAndBuild(html);
    beginScripts();
    doLayout(); // first paint: DOM + inline <style> rules + whatever beginScripts ran synchronously (see its comment) - external resources fill in over the next few frames via pollResources
}

// Collects <script> elements under `el` (and `el` itself) in document
// order. A <script> is a raw-text tag (see HTMLParser::isRawTextTag), so it
// never has element children of its own - no need to recurse into it.
static void collectScripts(Element* el, std::vector<Element*>& out) {
    if (!el) return;
    if (el->tag == L"script") { out.push_back(el); return; }
    for (auto& child : el->children) {
        if (child->type == Node::ELEMENT) collectScripts(static_cast<Element*>(child.get()), out);
    }
}

// A <script> with an unrecognized `type` isn't JavaScript at all - commonly
// JSON-LD structured data (type="application/ld+json") or a template
// library's inline template (type="text/template") - and must not be
// evaluated as a script. No `type`, or an empty one, means classic JS.
// type="module" is also skipped for now: import/export needs
// JS_EVAL_TYPE_MODULE and module resolution, neither implemented yet.
static bool isClassicScript(Element* scriptEl) {
    auto it = scriptEl->attrs.find(L"type");
    if (it == scriptEl->attrs.end() || it->second.empty()) return true;
    std::wstring t = it->second;
    for (auto& c : t) c = (wchar_t)towlower(c);
    return t == L"text/javascript" || t == L"application/javascript" || t == L"application/ecmascript";
}

// Sets up every <script> on the page to run once, in document order, after
// the whole DOM is built (see Layout.h's LayoutRoot::loadImage comment for
// the same "batch, not streaming" simplification applied to images - here
// it means a script can't document.write() more markup mid-parse, since
// parsing has already finished by the time any script runs). An inline
// script's code is available immediately; an external one (<script src>)
// is fetched concurrently with every other external script/stylesheet on
// the page (ResourceLoader) instead of blocking here - advanceScripts()
// (called once synchronously below, then again each frame from
// pollResources) runs each task the moment it's ready, in order, so a page
// with no external scripts behaves exactly as it did when this all ran
// synchronously, and a page with some just fills them in over a few frames
// instead of freezing the UI thread on each one in turn.
void Engine::beginScripts() {
    // Drop the previous page's state FIRST, while its JS runtime still exists:
    // domState holds JSValues (event listeners, pending timers) that must be
    // freed against their own runtime, and quickjs asserts if a runtime is
    // destroyed while any value is still alive. So: state, then old realm, then new.
    domState = DOMBindingState{};
    domState.pageUrl = pageBaseUrl; // so location.href/.replace()/.assign() have a base
    consoleLog().clear(); // a new page starts with an empty console, as in browsers
    jsEngine.reset();
    jsEngine = std::make_unique<JSEngine>(); // fresh realm per page
    scriptTasks_.clear();
    scriptCursor_ = 0;
    domContentLoadedFired_ = false;
    loadFired_ = false;
    if (!document || !document->body) { scriptLoader_.start({}); return; }

    installDOMBindings(jsEngine->context(), document->body.get(), &domState);

    // From the top of the tree, not <body>: real pages put most of their
    // scripts in <head>. Those run first, in document order, as in browsers.
    std::vector<Element*> scripts;
    collectScripts(document->root ? document->root.get() : document->body.get(), scripts);

    std::vector<std::wstring> urls;
    for (Element* scriptEl : scripts) {
        if (!isClassicScript(scriptEl)) continue;

        auto srcAttr = scriptEl->attrs.find(L"src");
        if (srcAttr != scriptEl->attrs.end() && !srcAttr->second.empty()) {
            std::wstring resolved = resolveUrl(pageBaseUrl, srcAttr->second);
            if (resolved.empty()) continue;
            ScriptTask task;
            task.external = true;
            task.fetchIndex = urls.size();
            scriptTasks_.push_back(task);
            urls.push_back(resolved);
        }
        else {
            ScriptTask task;
            task.external = false;
            for (auto& child : scriptEl->children) {
                if (child->type == Node::TEXT) task.inlineCode += static_cast<TextNode*>(child.get())->text;
            }
            scriptTasks_.push_back(std::move(task));
        }
    }
    scriptLoader_.start(std::move(urls), FetchDest::Script, pageBaseUrl);
    advanceScripts();
}

// Runs scriptTasks_[scriptCursor_..] for as long as each one is ready
// (inline is always ready; external is ready once its ResourceLoader fetch
// completes), stopping at the first that isn't - preserving document
// order even though external scripts can finish fetching in any order.
void Engine::advanceScripts() {
    while (scriptCursor_ < scriptTasks_.size()) {
        ScriptTask& task = scriptTasks_[scriptCursor_];
        std::wstring code;
        std::wstring name; // what error stacks call this script, so the console can say which one threw
        if (task.external) {
            if (!scriptLoader_.ready(task.fetchIndex)) break; // wait here even if a later task is already ready
            const FetchResult& res = scriptLoader_.result(task.fetchIndex);
            scriptCursor_++;
            if (!res.ok) {
                consoleLog().add(LogLevel::Error, L"network", L"Failed to load script " +
                                 scriptLoader_.url(task.fetchIndex) + L": " + res.error);
                continue;
            }
            code = res.html;
            name = res.finalUrl;
        }
        else {
            code = task.inlineCode;
            scriptCursor_++;
            name = L"<inline script " + std::to_wstring(scriptCursor_) + L">";
        }
        if (code.empty()) continue;

        std::string filename; // URLs are ASCII; anything else just degrades the label
        for (wchar_t c : name) filename.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        JSEngine::Result result = jsEngine->eval(code, filename.c_str());
        if (!result.ok) consoleLog().add(LogLevel::Error, L"script", result.text);
        domState.domDirty = true; // conservatively assume the script may have mutated the DOM
    }
    fireReadyEvents();
}

// Fires the page's lifecycle events once each, as soon as they're due:
// DOMContentLoaded when every script has run (the DOM itself is complete
// before any script runs - see beginScripts), then load once every
// stylesheet has also arrived. (Unlike browsers, load doesn't wait for
// images.) Each is preceded by document.readyState advancing and its
// readystatechange event. Called from advanceScripts, so both first get a
// chance during loadHTML and then every frame via pollResources.
void Engine::fireReadyEvents() {
    JSContext* ctx = jsContext();
    if (!ctx || scriptCursor_ < scriptTasks_.size()) return;
    if (!domContentLoadedFired_) {
        domContentLoadedFired_ = true;
        advanceReadyState(ctx, L"interactive");
        fireEvent(ctx, document->body.get(), L"DOMContentLoaded", true, false);
        domState.domDirty = true; // listeners commonly build the page here
    }
    if (loadFired_) return;
    for (const auto& task : styleTasks_) if (!task.applied) return;
    loadFired_ = true;
    advanceReadyState(ctx, L"complete");
    fireEvent(ctx, nullptr, L"load", false, false);
    domState.domDirty = true;
}

// Precomputed per-<link> band, wide enough that no single stylesheet will
// ever produce this many rules - see parseAndBuild's comment on why this
// (not simple append-order) is what keeps specificity ties resolving in
// true document order despite stylesheets applying in arbitrary fetch-
// completion order.
static constexpr int kStyleOrderBand = 1'000'000;

// Rewrites every url(...) in a stylesheet's text that's relative to an
// absolute one, resolved against the stylesheet's own URL - which is what
// a relative url() in CSS is relative to. Rules from every stylesheet end
// up in one list, and images are resolved against the page when drawn, so
// without this "url(../img/hero.jpg)" in /css/site.css would be looked up
// next to the page instead. data: URIs and unresolvable ones are left as is.
static std::wstring absolutizeCssUrls(const std::wstring& css, const std::wstring& base) {
    if (base.empty()) return css;
    std::wstring out;
    size_t i = 0;
    while (true) {
        size_t at = css.find(L"url(", i);
        if (at == std::wstring::npos) { out.append(css, i, std::wstring::npos); return out; }
        size_t close = css.find(L')', at + 4);
        if (close == std::wstring::npos) { out.append(css, i, std::wstring::npos); return out; }
        out.append(css, i, at + 4 - i);
        std::wstring inner = css.substr(at + 4, close - at - 4);
        size_t a = inner.find_first_not_of(L" \t\r\n"), b = inner.find_last_not_of(L" \t\r\n");
        std::wstring url = a == std::wstring::npos ? L"" : inner.substr(a, b - a + 1);
        wchar_t quote = 0;
        if (url.size() >= 2 && (url[0] == L'"' || url[0] == L'\'') && url.back() == url[0]) {
            quote = url[0];
            url = url.substr(1, url.size() - 2);
        }
        std::wstring resolved = url.empty() || url.rfind(L"data:", 0) == 0 || url[0] == L'#' ? L"" : resolveUrl(base, url);
        if (resolved.empty()) out += inner;
        else {
            if (quote) out += quote;
            out += resolved;
            if (quote) out += quote;
        }
        out += L')';
        i = close + 1;
    }
}

// Applies every styleTask_/scriptTask_ that has become ready since the last
// call, in whatever order their fetches happened to complete (stylesheets)
// or strictly in document order (scripts, via advanceScripts) - called
// once per frame from render(). This is what makes resource loading
// "progressive": the page already painted once (Engine::loadHTML's first
// doLayout) before any external resource was necessarily ready, and each
// one that lands here nudges domState.domDirty so render()'s existing
// domDirty check re-lays-out to reflect it - the same mechanism already
// used for JS timers/DOM mutations, not a new one.
void Engine::pollResources() {
    for (auto& task : styleTasks_) {
        if (task.applied) continue;
        if (!styleLoader_.ready(task.fetchIndex)) continue;
        task.applied = true;
        const FetchResult& res = styleLoader_.result(task.fetchIndex);
        if (!res.ok) {
            consoleLog().add(LogLevel::Error, L"network", L"Failed to load stylesheet " +
                             styleLoader_.url(task.fetchIndex) + L": " + res.error);
            continue;
        }

        // url()s in a stylesheet are relative to the stylesheet, not the page
        // (a stylesheet read from disk has no finalUrl - its path is the base).
        const std::wstring& sheetUrl = res.finalUrl.empty() ? styleLoader_.url(task.fetchIndex) : res.finalUrl;
        std::vector<CSS::Rule> extra = CSS::parseStylesheet(absolutizeCssUrls(res.html, sheetUrl));
        for (auto& r : extra) r.order += task.orderBase;
        document->styles.insert(document->styles.end(),
                                 std::make_move_iterator(extra.begin()),
                                 std::make_move_iterator(extra.end()));
        domState.domDirty = true;
    }
    advanceScripts();
}

// Finds every <link rel="stylesheet" href="..."> under `el` (and `el`
// itself), in document order - the same recursive-descent shape
// HTMLParser's own collectStyleText uses for <style> blocks. <link> is a
// void tag (see HTMLParser::isVoidTag), so it never has children to
// recurse into.
static void collectStylesheetLinks(Element* el, std::vector<Element*>& out) {
    if (!el) return;
    if (el->tag == L"link") {
        auto relIt = el->attrs.find(L"rel");
        auto hrefIt = el->attrs.find(L"href");
        if (relIt == el->attrs.end() || hrefIt == el->attrs.end()) return;
        std::wstring rel = relIt->second;
        for (auto& c : rel) c = (wchar_t)towlower(c);
        // Exact match only - a real UA token-matches a space-separated rel
        // list (rel="preload stylesheet" and the like), which is rare
        // enough for a <link> that this doesn't bother.
        if (rel == L"stylesheet") out.push_back(el);
        return;
    }
    for (auto& child : el->children) {
        if (child->type == Node::ELEMENT) collectStylesheetLinks(static_cast<Element*>(child.get()), out);
    }
}

bool Engine::takeNavigation(std::wstring& outUrl, bool& outReplace) {
    if (!domState.navigationPending) return false;
    outUrl = std::move(domState.navigationUrl);
    outReplace = domState.navigationReplace;
    domState.navigationPending = false;
    domState.navigationUrl.clear();
    domState.navigationReplace = false;
    return true;
}

// Collects every <link rel="stylesheet"> on the page (in document order,
// after whatever inline <style> rules HTMLParser already parsed into
// doc->styles) and starts fetching them all concurrently (ResourceLoader) -
// pollResources applies each one's rules the moment it's ready, rather
// than this blocking on them one at a time.
//
// Each stylesheet gets a precomputed order *band* (its 1-based position
// among links, times kStyleOrderBand) added to every rule it produces,
// instead of the old approach of offsetting by doc->styles.size() at
// append time. That old approach relied on appending happening in
// document order, which relied on fetching happening serially - no longer
// true now that stylesheets apply in whatever order their fetches happen
// to complete. Precomputing each one's band from its position in the
// *document* rather than its position in doc->styles keeps specificity
// ties resolving in true document order regardless of fetch order (band 0
// is implicitly reserved for inline <style> rules, whose order values
// HTMLParser already assigns starting at 0).
//
// Simplification carried over unchanged from before: every linked
// stylesheet's rules end up ordered after every inline <style> block's
// rules, regardless of their true relative position in the markup -
// correct for the overwhelmingly common case (a stylesheet link in <head>,
// any inline overrides after it) and only wrong if a page deliberately
// puts an overriding <style> block *before* its <link rel=stylesheet> and
// relies on that ordering to win a specificity tie.
void Engine::parseAndBuild(const std::wstring& html) {
    // The old DOM (and the elements form state points into) is about to go.
    focusedEl = nullptr;
    focusedForm = nullptr;
    hasSubmission = false;
    openSelect = nullptr;
    mouseTarget_ = nullptr;
    hoverSet.clear(); // points into the old DOM
    hoverSetLive = false;

    HTMLParser parser;
    document = parser.parse(html);

    styleTasks_.clear();
    if (document) {
        Element* searchRoot = document->root ? document->root.get() : document->body.get();
        std::vector<Element*> links;
        collectStylesheetLinks(searchRoot, links);

        std::vector<std::wstring> urls;
        for (Element* link : links) {
            std::wstring resolved = resolveUrl(pageBaseUrl, link->attrs[L"href"]);
            if (resolved.empty()) continue; // e.g. a relative href on a local-file page

            StyleTask task;
            task.fetchIndex = urls.size();
            task.orderBase = kStyleOrderBand * (int)(styleTasks_.size() + 1);
            styleTasks_.push_back(task);
            urls.push_back(resolved);
        }
        styleLoader_.start(std::move(urls), FetchDest::Style, pageBaseUrl);
    }
    else {
        styleLoader_.start({});
    }

    if (document && document->body) {
        layoutRoot.rootNode = document->body.get();
        layoutRoot.rules = &document->styles;
        layoutRoot.quirks = document->quirks;
    }
    else {
        layoutRoot.rules = nullptr;
    }
}

// Called every frame, so it tries hard not to relayout more than needed:
// - Only the width affects layout. A height change just re-clamps the
//   scroll position.
// - A width change relayouts at once when layout is cheap (≤ 30 ms), so
//   small pages reflow live.
// - When it isn't (a big page - Wikipedia takes ~0.4 s in Release, seconds
//   in Debug), the relayout is deferred (see the top of render()):
//   * always by at least one frame, so the window first repaints at its
//     new size - with the old layout - instead of sitting unpainted while
//     the relayout runs (most visible on maximize, snap and restore);
//   * and while the user is still dragging a window edge (liveResize_),
//     until the size has held still for kResizeSettleMs, or the drag ends,
//     so the drag itself stays smooth.
static constexpr double kLiveResizeLayoutBudgetMs = 30;
static constexpr int kResizeSettleMs = 150;

void Engine::onResize(int w, int h) {
    if (w == width && h == height) return;
    // Layout depends on the width; on the height only for a page that used
    // it (vh units, fixed elements, height: 100% down from the viewport).
    bool widthChanged = w != width;
    width = w;
    height = h;
    if (!widthChanged && !layoutRoot.usedViewportHeight) {
        scrollY = std::clamp(scrollY, 0, maxScroll());
        return;
    }

    layoutRoot.viewportWidth = (int)(width / zoom_); // layout sees the window as 1/zoom as wide
    if (lastLayoutMs <= kLiveResizeLayoutBudgetMs) {
        doLayout();
    } else {
        relayoutPending_ = true;
        resizePainted_ = false;
        lastResize_ = std::chrono::steady_clock::now();
    }
}

void Engine::setLiveResize(bool live) {
    liveResize_ = live;
    // The drag just ended: any deferred relayout no longer needs to wait
    // for the size to settle (render() still lets one frame paint first).
}

void Engine::doLayout() {
    relayoutPending_ = false; // whatever triggered this, it lays out at the current width
    if (!document || !document->body)
        return;

    // Any relayout can move a select's box, invalidating openSelectBox's
    // snapshot - simplest safe rule is a dropdown doesn't survive a
    // relayout, loosely matching how a native select popup also closes on
    // scroll/resize in a real browser.
    openSelect = nullptr;

    auto layoutStart = std::chrono::steady_clock::now();
    layoutRoot.boxes.clear();
    layoutRoot.viewportHeight = viewHeight(); // vh units, height: %, fixed boxes
    layoutRoot.layout();
    lastLayoutMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - layoutStart).count();

    // Calculate document height. A fixed box doesn't scroll with the page,
    // so it doesn't make it longer; clipped content only counts as far as
    // its clip reaches.
    documentHeight = 0;
    for (const auto& b : layoutRoot.boxes) {
        if (b.fixed) continue;
        int bottom = b.y + b.height;
        if (b.clipped) bottom = std::min(bottom, b.clipY + b.clipH);
        if (bottom > documentHeight)
            documentHeight = bottom;
    }

    // Clamp scroll
    int maxScroll = documentHeight - viewHeight();
    if (maxScroll < 0) maxScroll = 0;

    if (scrollY > maxScroll) scrollY = maxScroll;
    if (scrollY < 0) scrollY = 0;

    // Find matches point into the boxes just replaced - redo the search
    // against the new layout (keeping the current match, but not scrolling).
    if (!findQuery_.empty()) runFind();
}

int Engine::getDocumentHeight() const {
    return documentHeight;
}

void Engine::scroll(int delta) {
    scrollY += delta;

    int maxScroll = documentHeight - viewHeight();
    if (maxScroll < 0) maxScroll = 0;

    scrollY = std::max(0, std::min(scrollY, maxScroll));
}

void Engine::render(Renderer& renderer, double timeSeconds) {
    // Renderer should clear framebuffer first:
    // glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // A background image load finished since the last layout: an <img> box
    // sized from a guess (no width/height attrs, natural size unknown at
    // the time) may need to be resized now that the real size is known.
    int gen = renderer.imageGeneration();
    if (gen != lastImageGeneration) {
        lastImageGeneration = gen;
        doLayout();
    }

    // A setTimeout/setInterval callback may mutate the DOM below (sets
    // domDirty, same as any other script), so this runs before that check.
    if (jsEngine) fireDueTimers(jsEngine->context(), timeSeconds);

    // Same for fetch() promises whose request has finished since last frame.
    if (jsEngine) pollFetches(jsEngine->context());

    // A stylesheet/script that was still being fetched in the background
    // may have just landed - apply whatever's newly ready (also sets
    // domDirty on anything that changes; see pollResources).
    pollResources();

    // A script mutated the DOM (appendChild, textContent=, setAttribute,
    // innerHTML=, ...) since the last layout - re-layout to pick it up.
    if (domState.domDirty) {
        // The mutation may have freed elements the engine holds on to.
        // Once domDirty is cleared nothing would catch that later, so drop
        // any that are gone now.
        if (focusedEl && !inPage(focusedEl)) { focusedEl = nullptr; focusedForm = nullptr; }
        if (focusedForm && !inPage(focusedForm)) focusedForm = nullptr;
        if (openSelect && !inPage(openSelect)) openSelect = nullptr;
        if (mouseTarget_ && !inPage(mouseTarget_)) mouseTarget_ = nullptr;
        domState.domDirty = false;
        hoverSetLive = false; // the mutation may have freed hovered elements
        // syncValue keeps the focused field's `value` attribute equal to
        // the editor's text after every edit, so a difference here means a
        // script set .value - show that instead of the stale editor text.
        if (focusedEl) {
            auto it = focusedEl->attrs.find(L"value");
            std::wstring v = it == focusedEl->attrs.end() ? L"" : it->second;
            if (v != editor.text()) editor.setText(v);
        }
        doLayout();
    }

    // A width change deferred by onResize (see there): relayout once a frame
    // has shown the window at its new size, and - during an edge drag -
    // once the size has held still for a moment.
    if (relayoutPending_) {
        bool settled = !liveResize_ ||
            std::chrono::steady_clock::now() - lastResize_ >= std::chrono::milliseconds(kResizeSettleMs);
        if (resizePainted_ && settled) doLayout(); // clears relayoutPending_
        resizePainted_ = true; // this frame paints the new size with the old layout
    }

    // The page is drawn in page coordinates: the transform moves it below
    // the address bar and applies the zoom (see Renderer::setPageTransform).
    renderer.setPageTransform((float)topInset, zoom_);
    const int visibleHeight = viewHeight();

    // Pops a box's overflow clip however its painting below ends.
    struct ClipScope {
        Renderer& r;
        bool on;
        ~ClipScope() { if (on) r.popClip(); }
    };

    for (const auto& b : layoutRoot.boxes) {

        const int shift = boxShift(b); // fixed/sticky
        int screenY = b.y + shift - scrollY; // relative to the top of the page area

        // Cull boxes outside viewport
        if (screenY + b.height < 0 || screenY > visibleHeight)
            continue;

        // opacity:0 / visibility:hidden (own or inherited): still occupies
        // its layout position (handled above/below), just not painted.
        // Click hit-testing (dispatchClick/onClick) is unaffected - a
        // deliberate simplification, see Layout.h's LayoutBox comment.
        if (b.visuallyHidden) continue;

        // An ancestor's overflow clip (see LayoutBox::clipped); a box that's
        // clipped away entirely isn't drawn at all.
        if (b.clipped) {
            if (b.clipW <= 0 || b.clipH <= 0) continue;
            int clipTop = b.clipY + shift - scrollY;
            if (clipTop > visibleHeight || clipTop + b.clipH < 0) continue;
        }
        ClipScope clip{ renderer, b.clipped };
        if (b.clipped) renderer.pushClip((float)b.clipX, (float)(b.clipY + shift - scrollY), (float)b.clipW, (float)b.clipH);

        // box-shadow, under everything else the box draws (controls too).
        paintShadows(renderer, b, (float)screenY);

        if (b.control != LayoutBox::NoControl) {
            drawControl(renderer, b, screenY, timeSeconds);
            continue;
        }

        // The background - colour, then image/gradient layers - inside the
        // border, its corners rounded to match the border's inner edge;
        // then the border over it.
        float r[4], inner[4];
        b.cornerRadii(r);
        const bool rounded = b.rounded();
        const float bw = (float)b.borderWidth;
        for (int i = 0; i < 4; i++) inner[i] = std::max(r[i] - bw, 0.0f);
        const float ax = b.x + bw, ay = screenY + bw, aw = b.width - 2 * bw, ah = b.height - 2 * bw;
        if (!b.background.empty()) {
            if (rounded) renderer.drawRoundedRect(ax, ay, aw, ah, inner, parseColor(b.background));
            else renderer.drawRect(ax, ay, aw, ah, parseColor(b.background));
        }
        paintBackgroundLayers(renderer, b, ax, ay, aw, ah, rounded ? inner : nullptr);

        if (bw > 0) {
            Color bc = parseColor(b.borderColor);
            if (rounded) renderer.drawRoundedFrame(b.x, screenY, b.width, b.height, r, bw, bc);
            else {
                // Four thin rects forming a hollow frame, not one filled
                // rect, so a box with a border but no background still shows
                // whatever's behind it through the middle - like a real CSS border.
                renderer.drawRect(b.x, screenY, b.width, bw, bc);                          // top
                renderer.drawRect(b.x, screenY + b.height - bw, b.width, bw, bc);          // bottom
                renderer.drawRect(b.x, screenY, bw, b.height, bc);                         // left
                renderer.drawRect(b.x + b.width - bw, screenY, bw, b.height, bc);          // right
            }
        }

        // Draw image
        if (!b.imageSrc.empty()) {
            std::wstring src = resolveImageSrc(pageBaseUrl, b.imageSrc);
            if (!src.empty()) renderer.drawImage(b.x, screenY, b.width, b.height, src, rounded ? r : nullptr);
            continue;
        }

        // Find-in-page highlights, behind the text: yellow for every match,
        // orange for the current one, as in browsers.
        size_t boxIndex = &b - layoutRoot.boxes.data();
        if (boxIndex < findHighlights_.size()) {
            float fs = b.fontSize > 0 ? (float)b.fontSize : 14;
            for (const auto& [span, current] : findHighlights_[boxIndex]) {
                float x0 = measurePageText(renderer, b.text.substr(0, span.start), fs, b.bold, b.italic, b.family);
                float x1 = measurePageText(renderer, b.text.substr(0, span.end), fs, b.bold, b.italic, b.family);
                renderer.drawRect(b.x + 4 + x0, (float)screenY + 2, x1 - x0, fs + 6,
                                  current ? Color{ 1.0f, 0.60f, 0.15f, 1 } : Color{ 1.0f, 0.93f, 0.35f, 1 });
            }
        }

        // Draw text. An empty color means black; links arrive already
        // colored blue by layout (computeStyle), unless CSS overrode it.
        if (!b.text.empty()) {
            float fs = b.fontSize > 0 ? (float)b.fontSize : 14;
            Color ink = b.color.empty() ? Color{ 0, 0, 0, 1 } : parseColor(b.color);
            float textTop = (float)screenY + 4;
            renderer.drawText(b.x + 4, textTop, b.text, fs, ink, b.bold, b.italic, b.family);

            // text-decoration, in the text's colour. Text is drawn from the
            // top of its line cell, where the baseline sits about 1.08 em
            // down in Segoe UI (close enough for other faces): an underline
            // just below that, a line-through about a third of an em above.
            if (b.underline || b.lineThrough) {
                float thickness = std::max(1.0f, std::round(fs / 14));
                float w = (float)std::max(b.decorationWidth, 0);
                if (b.underline) renderer.drawRect(b.x + 4, std::round(textTop + fs * 1.15f), w, thickness, ink);
                if (b.lineThrough) renderer.drawRect(b.x + 4, std::round(textTop + fs * 0.72f), w, thickness, ink);
            }
        }
    }

    drawOpenSelect(renderer); // on top of the page, same treatment main.cpp gives AddressBar
    renderer.resetTransform();
    drawScrollbar(renderer); // window coordinates: browser UI, not zoomed
}

int Engine::boxShift(const LayoutBox& b) const {
    if (b.fixed) return scrollY;
    if (b.sticky >= 0 && b.sticky < (int)layoutRoot.stickies.size()) {
        const LayoutRoot::Sticky& s = layoutRoot.stickies[b.sticky];
        return std::clamp(scrollY + s.top - s.naturalTop, 0, std::max(s.maxShift, 0));
    }
    return 0;
}

bool Engine::boxContains(const LayoutBox& b, int docX, int docY) const {
    int shift = boxShift(b);
    int top = b.y + shift;
    if (docX < b.x || docX >= b.x + b.width || docY < top || docY >= top + b.height) return false;
    if (!b.clipped) return true;
    int clipTop = b.clipY + shift;
    return docX >= b.clipX && docX < b.clipX + b.clipW && docY >= clipTop && docY < clipTop + b.clipH;
}

// Each shadow is the box's shape moved by its offset and grown by its
// spread (corners too), with its edge blurred. The last one listed is
// painted first, so the first ends up on top, as in CSS. Unlike CSS, a
// shadow isn't cut away under the box itself - which only shows through a
// box with a see-through background.
void Engine::paintShadows(Renderer& renderer, const LayoutBox& b, float screenY) {
    if (b.shadows.empty()) return;
    float r[4];
    b.cornerRadii(r);
    for (auto it = b.shadows.rbegin(); it != b.shadows.rend(); ++it) {
        const BoxShadow& s = *it;
        if (s.inset) continue;
        float grown[4];
        for (int i = 0; i < 4; i++) grown[i] = r[i] > 0 ? std::max(r[i] + s.spread, 0.0f) : 0;
        renderer.drawShadow(b.x + s.x - s.spread, screenY + s.y - s.spread,
                            b.width + 2 * s.spread, b.height + 2 * s.spread, grown, s.blur, s.color);
    }
}

// Layers are painted last-listed first, so the first is on top. A
// gradient fills the whole area. An image is sized (auto/cover/contain/
// explicit), placed (a percentage puts that point of the image on that
// point of the area, as CSS does) and repeated along each axis that
// repeats. An image that covers the whole area is drawn as one rounded
// shape, so rounded corners clip it; a smaller or repeating one is clipped
// to the area's rectangle.
void Engine::paintBackgroundLayers(Renderer& renderer, const LayoutBox& b, float x, float y, float w, float h,
                                   const float* radii) {
    if (b.backgrounds.empty() || w <= 0 || h <= 0) return;
    for (auto it = b.backgrounds.rbegin(); it != b.backgrounds.rend(); ++it) {
        const BackgroundLayer& layer = *it;
        if (layer.gradient) {
            renderer.drawGradient(x, y, w, h, *layer.gradient, radii);
            continue;
        }
        std::wstring src = resolveImageSrc(pageBaseUrl, layer.image);
        int iw = 0, ih = 0;
        if (src.empty() || !renderer.preloadImage(src, iw, ih) || iw <= 0 || ih <= 0) continue; // loading, or failed

        // The tile's size.
        float tw = (float)iw, th = (float)ih;
        switch (layer.size) {
        case BackgroundLayer::Size::Cover:
        case BackgroundLayer::Size::Contain: {
            float sx = w / iw, sy = h / ih;
            float s = layer.size == BackgroundLayer::Size::Cover ? std::max(sx, sy) : std::min(sx, sy);
            tw = iw * s; th = ih * s;
            break;
        }
        case BackgroundLayer::Size::Explicit: {
            auto resolve = [](const BgLength& l, float area) { return l.percent ? l.value / 100 * area : l.value; };
            bool autoW = layer.width.isAuto, autoH = layer.height.isAuto;
            if (!autoW) tw = resolve(layer.width, w);
            if (!autoH) th = resolve(layer.height, h);
            if (autoW && !autoH) tw = th * iw / ih; // one auto side keeps the image's proportions
            if (autoH && !autoW) th = tw * ih / iw;
            break;
        }
        case BackgroundLayer::Size::Auto: break;
        }
        if (tw < 1 || th < 1) continue;

        // Its position: a percentage aligns that point of the tile with
        // that point of the area ("100%" = right edge to right edge).
        auto place = [](const BgLength& l, float area, float tile) {
            return l.percent ? (area - tile) * l.value / 100 : l.value;
        };
        float tx = x + place(layer.posX, w, tw), ty = y + place(layer.posY, h, th);

        if (tx <= x && ty <= y && tx + tw >= x + w && ty + th >= y + h) { // one tile covers it all
            float tile[4] = { tx, ty, tw, th };
            renderer.drawImage(x, y, w, h, src, radii, tile);
            continue;
        }
        // Tiles from the first that reaches the area's edge, along each
        // repeating axis; one tile along the others.
        float x0 = tx, y0 = ty;
        if (layer.repeatX) x0 = tx - std::ceil((tx - x) / tw) * tw;
        if (layer.repeatY) y0 = ty - std::ceil((ty - y) / th) * th;
        float x1 = layer.repeatX ? x + w : x0 + 1, y1 = layer.repeatY ? y + h : y0 + 1;
        if ((x1 - x0) / tw * (y1 - y0) / th > 4000) continue; // a tiny tile over a huge box: not worth it
        renderer.pushClip(x, y, w, h);
        for (float py = y0; py < y1; py += th)
            for (float px = x0; px < x1; px += tw) renderer.drawImage(px, py, tw, th, src);
        renderer.popClip();
    }
}

std::wstring Engine::title() const {
    return document && document->root ? documentTitle(document->root.get()) : L"";
}

// A :hover restyle is a full relayout (there's no restyle-only path), so
// on a page whose layout already takes longer than this, hover changes are
// ignored rather than stalling the window on every mouse move.
static constexpr double kHoverRelayoutBudgetMs = 50;

void Engine::updateHover(int x, int y) {
    scrollbarHovered_ = scrollbarAt(x, y) != ScrollbarPart::None;
    if (scrollbarHovered_) { x = -1; y = -1; } // the page under the scrollbar isn't hovered

    // A pending DOM mutation means layoutRoot.boxes may point at freed
    // elements; render() will relayout first, and the next call catches up.
    if (!document || domState.domDirty) return;

    Element* under = elementAt(x, y);
    if (under != mouseTarget_) {
        Element* from = mouseTarget_;
        mouseTarget_ = under;
        fireMouseTransition(from, under, mouseInfoAt(x, y));
        if (domState.domDirty) return; // a listener changed the page: boxes are stale until render() relayouts
    }

    CSS::HoverSet next;
    for (const Element* el = under; el; el = el->parent) next.insert(el);
    if (next == hoverSet) return;

    bool matters = false;
    if (layoutRoot.rules && CSS::hasHoverRules(*layoutRoot.rules)) {
        std::vector<const Element*> changed;
        for (const Element* el : next) if (!hoverSet.count(el)) changed.push_back(el);
        for (const Element* el : hoverSet) {
            if (next.count(el)) continue;
            // An element that left the hover set can only be tested if it's
            // known to still be alive; otherwise assume the worst.
            if (hoverSetLive) changed.push_back(el);
            else matters = true;
        }
        matters = matters || CSS::hoverCouldAffect(*layoutRoot.rules, changed);
    }

    // A relayout would close an open <select> dropdown (see doLayout), so
    // hold off - leaving hoverSet as-is means this is retried every frame
    // and applied once the dropdown closes.
    if (matters && openSelect) return;

    hoverSet = std::move(next);
    hoverSetLive = true;
    if (matters && lastLayoutMs <= kHoverRelayoutBudgetMs) doLayout();
}

// The mouse moved from element `from` to `to` (either may be nullptr: off
// the page). Fires mouseout at `from` and mouseover at `to` (both bubble),
// then mouseleave at each element left and mouseenter at each one entered
// - the ancestors the two don't share, innermost first for leave and
// outermost first for enter, as in browsers.
void Engine::fireMouseTransition(Element* from, Element* to, const MouseInfo& info) {
    JSContext* ctx = jsContext();
    if (!ctx) return;

    // Taken before any listener runs: one may move or free these elements.
    std::vector<Element*> fromChain, toChain;
    for (Element* e = from; e; e = e->parent) fromChain.push_back(e);
    for (Element* e = to; e; e = e->parent) toChain.push_back(e);
    std::vector<Element*> left, entered;
    for (Element* e : fromChain)
        if (std::find(toChain.begin(), toChain.end(), e) == toChain.end()) left.push_back(e);
    for (Element* e : toChain)
        if (std::find(fromChain.begin(), fromChain.end(), e) == fromChain.end()) entered.push_back(e);
    std::reverse(entered.begin(), entered.end());

    if (from && stillInPage(from)) fireMouseEvent(ctx, from, L"mouseout", info, stillInPage(to) ? to : nullptr);
    for (Element* e : left)
        if (stillInPage(e)) fireMouseEvent(ctx, e, L"mouseleave", info, stillInPage(to) ? to : nullptr);
    if (to && stillInPage(to)) fireMouseEvent(ctx, to, L"mouseover", info, stillInPage(from) ? from : nullptr);
    for (Element* e : entered)
        if (stillInPage(e)) fireMouseEvent(ctx, e, L"mouseenter", info, stillInPage(from) ? from : nullptr);
}

MouseInfo Engine::mouseInfoAt(int x, int y) const {
    MouseInfo info;
    int pageX, pageY;
    if (toPage(x, y, pageX, pageY)) {
        info.pageX = pageX;
        info.pageY = pageY;
        info.clientX = pageX;
        info.clientY = pageY - scrollY;
    }
    return info;
}

std::wstring Engine::linkAt(int x, int y, Renderer& renderer) const {
    int docX, docY;
    if (!toPage(x, y, docX, docY)) return L"";

    // Later boxes paint on top, so check them first.
    for (auto it = layoutRoot.boxes.rbegin(); it != layoutRoot.boxes.rend(); ++it) {
        const auto& b = *it;
        if (b.href.empty()) continue;
        if (b.text.empty()) {
            // An image inside a link: its whole box is the link.
            if (!b.imageSrc.empty() && boxContains(b, docX, docY))
                return b.href;
            continue;
        }

        // Text is drawn at (x + 4, y + 4) and is only as wide as its glyphs,
        // so hit-test that area rather than the whole row.
        float fontSize = b.fontSize > 0 ? b.fontSize : 14;
        int textW = static_cast<int>(measurePageText(renderer, b.text, fontSize, b.bold, b.italic, b.family));
        int shift = boxShift(b);
        int left = b.x + 4, top = b.y + shift + 4;
        bool inClip = !b.clipped || (docX >= b.clipX && docX < b.clipX + b.clipW &&
                                     docY >= b.clipY + shift && docY < b.clipY + shift + b.clipH);
        if (docX >= left && docX < left + textW && docY >= top && docY < top + b.height && inClip)
            return b.href;
    }
    return L"";
}

void Engine::setTopInset(int px) {
    topInset = px;
    doLayout(); // re-clamp scroll for the new viewport height
}

// Called every frame (the console can open/close any time), so it only
// re-clamps the scroll - no relayout: the page's width doesn't change.
void Engine::setBottomInset(int px) {
    if (px == bottomInset) return;
    bottomInset = px;
    if (layoutRoot.usedViewportHeight) { doLayout(); return; } // the viewport got shorter (or taller)
    int maxScroll = std::max(documentHeight - viewHeight(), 0);
    scrollY = std::clamp(scrollY, 0, maxScroll);
}

// --- Keyboard scrolling and the scrollbar ------------------------------

static constexpr int kLineScroll = 40;        // px per arrow-key press (same as one wheel notch)
static constexpr int kScrollbarWidth = 12;
static constexpr float kMinThumb = 30;

void Engine::scrollLines(int lines) { scroll(lines * kLineScroll); }

void Engine::scrollPages(int pages) {
    int page = std::max(viewHeight() - kLineScroll, kLineScroll); // keep a line of overlap for context
    scroll(pages * page);
}

void Engine::scrollToEdge(bool bottom) {
    scrollY = bottom ? maxScroll() : 0;
}

bool Engine::scrollbarGeometry(float& trackTop, float& trackH, float& thumbTop, float& thumbH) const {
    if (documentHeight <= viewHeight() || viewHeight() <= 0) return false;
    trackTop = (float)topInset;
    trackH = (float)(height - topInset - bottomInset);
    thumbH = std::max(trackH * viewHeight() / documentHeight, kMinThumb);
    thumbTop = trackTop + (trackH - thumbH) * scrollY / std::max(maxScroll(), 1);
    return true;
}

Engine::ScrollbarPart Engine::scrollbarAt(int x, int y) const {
    float trackTop, trackH, thumbTop, thumbH;
    if (!scrollbarGeometry(trackTop, trackH, thumbTop, thumbH)) return ScrollbarPart::None;
    if (x < width - kScrollbarWidth || y < trackTop || y >= trackTop + trackH) return ScrollbarPart::None;
    return (y >= thumbTop && y < thumbTop + thumbH) ? ScrollbarPart::Thumb : ScrollbarPart::Track;
}

void Engine::beginScrollbarDrag(int y) {
    float trackTop, trackH, thumbTop, thumbH;
    if (!scrollbarGeometry(trackTop, trackH, thumbTop, thumbH)) return;
    draggingScrollbar_ = true;
    dragGrabOffset_ = y - thumbTop;
}

void Engine::dragScrollbar(int y) {
    float trackTop, trackH, thumbTop, thumbH;
    if (!draggingScrollbar_ || !scrollbarGeometry(trackTop, trackH, thumbTop, thumbH)) return;
    float range = trackH - thumbH;
    float t = range > 0 ? std::clamp((y - dragGrabOffset_ - trackTop) / range, 0.0f, 1.0f) : 0.0f;
    scrollY = (int)std::lround(t * maxScroll());
}

void Engine::pageTowards(int y) {
    float trackTop, trackH, thumbTop, thumbH;
    if (!scrollbarGeometry(trackTop, trackH, thumbTop, thumbH)) return;
    scrollPages(y < thumbTop ? -1 : 1);
}

// An overlay bar, drawn over the page's right edge in window coordinates,
// as modern browsers do on touch-friendly platforms; no layout space is
// reserved for it.
void Engine::drawScrollbar(Renderer& renderer) {
    float trackTop, trackH, thumbTop, thumbH;
    if (!scrollbarGeometry(trackTop, trackH, thumbTop, thumbH)) return;
    const float x = (float)(width - kScrollbarWidth);
    renderer.drawRect(x, trackTop, (float)kScrollbarWidth, trackH, Color{ 0.93f, 0.93f, 0.94f, 0.85f });
    const bool active = draggingScrollbar_ || scrollbarHovered_;
    const float shade = active ? 0.45f : 0.65f;
    renderer.drawRect(x + 2, thumbTop + 2, (float)kScrollbarWidth - 4, thumbH - 4, Color{ shade, shade, shade + 0.02f, 1.0f });
}

void Engine::setZoom(float zoom) {
    zoom = std::clamp(zoom, 0.25f, 5.0f);
    if (zoom == zoom_) return;
    zoom_ = zoom;
    layoutRoot.viewportWidth = (int)(width / zoom_);
    doLayout(); // reflows at the new width, and re-clamps the scroll
}

// --- Find in page ------------------------------------------------------

static constexpr size_t kMaxFindMatches = 1000; // a one-letter search on a huge page stays cheap

static std::wstring lowered(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

// Searches the page's visible text in paint order (= document order for
// flowing text). Layout makes one box per word, so the boxes' words are
// joined with single spaces into one stream - which also lets a search
// for "two words" match across boxes and lines - with a map from each
// stream character back to its box and position.
void Engine::runFind() {
    findMatches_.clear();
    std::wstring query;
    for (wchar_t c : lowered(findQuery_)) { // runs of whitespace in the query match a single space
        if (iswspace(c)) { if (!query.empty() && query.back() != L' ') query.push_back(L' '); }
        else query.push_back(c);
    }
    while (!query.empty() && query.back() == L' ') query.pop_back();

    if (!query.empty()) {
        std::wstring stream;
        struct Origin { size_t box, pos; };
        std::vector<Origin> origin;
        const auto& boxes = layoutRoot.boxes;
        for (size_t i = 0; i < boxes.size(); i++) {
            const LayoutBox& b = boxes[i];
            if (b.text.empty() || b.control != LayoutBox::NoControl || b.visuallyHidden || !b.imageSrc.empty()) continue;
            if (!stream.empty()) { stream.push_back(L' '); origin.push_back({ SIZE_MAX, 0 }); }
            std::wstring word = lowered(b.text);
            for (size_t k = 0; k < word.size(); k++) { stream.push_back(word[k]); origin.push_back({ i, k }); }
        }
        for (size_t pos = stream.find(query); pos != std::wstring::npos && findMatches_.size() < kMaxFindMatches;
             pos = stream.find(query, pos + query.size())) {
            FindMatch m;
            for (size_t k = pos; k < pos + query.size(); k++) {
                const Origin& o = origin[k];
                if (o.box == SIZE_MAX) continue; // the joining space between two boxes
                if (!m.spans.empty() && m.spans.back().box == o.box) m.spans.back().end = o.pos + 1;
                else m.spans.push_back({ o.box, o.pos, o.pos + 1 });
            }
            if (!m.spans.empty()) findMatches_.push_back(std::move(m));
        }
    }
    if (findMatches_.empty()) findCurrent_ = -1;
    else findCurrent_ = std::clamp(findCurrent_, 0, (int)findMatches_.size() - 1);
    rebuildFindHighlights();
}

void Engine::rebuildFindHighlights() {
    findHighlights_.assign(layoutRoot.boxes.size(), {});
    for (size_t m = 0; m < findMatches_.size(); m++)
        for (const FindSpan& s : findMatches_[m].spans)
            findHighlights_[s.box].push_back({ s, (int)m == findCurrent_ });
}

void Engine::scrollToFindMatch() {
    if (findCurrent_ < 0) return;
    const FindMatch& m = findMatches_[findCurrent_];
    const LayoutBox& first = layoutRoot.boxes[m.spans.front().box];
    const LayoutBox& last = layoutRoot.boxes[m.spans.back().box];
    if (first.y >= scrollY && last.y + last.height <= scrollY + viewHeight()) return; // already in view
    scroll((first.y - viewHeight() / 3) - scrollY); // a third of the way down, like focusing a field
}

void Engine::findText(const std::wstring& query) {
    findQuery_ = query;
    findCurrent_ = 0;
    runFind();
    // Start from the first match at or below the top of the view, as
    // browsers do, rather than jumping back to the top of the page.
    for (size_t i = 0; i < findMatches_.size(); i++) {
        if (layoutRoot.boxes[findMatches_[i].spans.front().box].y >= scrollY) { findCurrent_ = (int)i; break; }
    }
    rebuildFindHighlights();
    scrollToFindMatch();
}

void Engine::findNext(bool backwards) {
    if (findMatches_.empty()) return;
    int n = (int)findMatches_.size();
    findCurrent_ = (findCurrent_ + (backwards ? n - 1 : 1)) % n; // wraps around, as browsers do
    rebuildFindHighlights();
    scrollToFindMatch();
}

void Engine::clearFind() {
    findQuery_.clear();
    findMatches_.clear();
    findCurrent_ = -1;
    findHighlights_.clear();
}

float Engine::measurePageText(Renderer& r, const std::wstring& text, float fontSize, bool bold,
                               bool italic, const std::wstring* family) const {
    if (zoom_ == 1.0f) return r.measureText(text, fontSize, bold, italic, family);
    float pixelSize = std::max(std::round(fontSize * zoom_), 1.0f); // the size OpenGLRenderer::drawText rasterizes at
    return r.measureText(text, pixelSize, bold, italic, family) / zoom_;
}

bool Engine::toPage(int x, int y, int& pageX, int& pageY) const {
    if (y < topInset) return false;
    pageX = (int)std::floor(x / zoom_);
    pageY = (int)std::floor((y - topInset) / zoom_) + scrollY;
    return true;
}

void Engine::consoleEval(const std::wstring& code) {
    if (jsEngine) evaluateInConsole(jsEngine->context(), code);
}
