// Engine.cpp
#define NOMINMAX
#include "Engine.h"
#include "HTMLParser.h"
#include "CSS.h"
#include "Renderer.h"
#include "Fetcher.h"
#include "JSEngine.h"
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
    layoutRoot.measureText = [this](const std::wstring& text, int fontSize, bool bold) {
        return measurer ? measurer->measureText(text, (float)fontSize, bold)
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
    // domState holds JSValues (click listeners, pending timers) that must be
    // freed against their own runtime, and quickjs asserts if a runtime is
    // destroyed while any value is still alive. So: state, then old realm, then new.
    domState = DOMBindingState{};
    domState.pageUrl = pageBaseUrl; // so location.href/.replace()/.assign() have a base
    jsEngine.reset();
    jsEngine = std::make_unique<JSEngine>(); // fresh realm per page
    scriptTasks_.clear();
    scriptCursor_ = 0;
    if (!document || !document->body) { scriptLoader_.start({}); return; }

    installDOMBindings(jsEngine->context(), document->body.get(), &domState);

    std::vector<Element*> scripts;
    collectScripts(document->body.get(), scripts);

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
    scriptLoader_.start(std::move(urls));
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
        if (task.external) {
            if (!scriptLoader_.ready(task.fetchIndex)) break; // wait here even if a later task is already ready
            const FetchResult& res = scriptLoader_.result(task.fetchIndex);
            scriptCursor_++;
            if (!res.ok) continue;
            code = res.html;
        }
        else {
            code = task.inlineCode;
            scriptCursor_++;
        }
        if (code.empty()) continue;

        std::wstring result = jsEngine->eval(code);
        if (result.rfind(L"Error: ", 0) == 0) {
            std::wstring line = L"[script] " + result + L"\n";
            wprintf(L"%ls", line.c_str());
            OutputDebugStringW(line.c_str());
        }
        domState.domDirty = true; // conservatively assume the script may have mutated the DOM
    }
}

// Precomputed per-<link> band, wide enough that no single stylesheet will
// ever produce this many rules - see parseAndBuild's comment on why this
// (not simple append-order) is what keeps specificity ties resolving in
// true document order despite stylesheets applying in arbitrary fetch-
// completion order.
static constexpr int kStyleOrderBand = 1'000'000;

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
        if (!res.ok) continue;

        std::vector<CSS::Rule> extra = CSS::parseStylesheet(res.html);
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
        styleLoader_.start(std::move(urls));
    }
    else {
        styleLoader_.start({});
    }

    if (document && document->body) {
        layoutRoot.rootNode = document->body.get();
        layoutRoot.rules = &document->styles;
    }
    else {
        layoutRoot.rules = nullptr;
    }
}

void Engine::onResize(int w, int h) {
    if (w == width && h == height) return; // nothing to re-wrap
    width = w;
    height = h;

    layoutRoot.viewportWidth = width;
    doLayout();
}

void Engine::doLayout() {
    if (!document || !document->body)
        return;

    // Any relayout can move a select's box, invalidating openSelectBox's
    // snapshot - simplest safe rule is a dropdown doesn't survive a
    // relayout, loosely matching how a native select popup also closes on
    // scroll/resize in a real browser.
    openSelect = nullptr;

    auto layoutStart = std::chrono::steady_clock::now();
    layoutRoot.boxes.clear();
    layoutRoot.layout();
    lastLayoutMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - layoutStart).count();

    // Calculate document height
    documentHeight = 0;
    for (const auto& b : layoutRoot.boxes) {
        int bottom = b.y + b.height;
        if (bottom > documentHeight)
            documentHeight = bottom;
    }

    // Clamp scroll
    int maxScroll = documentHeight - viewHeight();
    if (maxScroll < 0) maxScroll = 0;

    if (scrollY > maxScroll) scrollY = maxScroll;
    if (scrollY < 0) scrollY = 0;
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

    // A stylesheet/script that was still being fetched in the background
    // may have just landed - apply whatever's newly ready (also sets
    // domDirty on anything that changes; see pollResources).
    pollResources();

    // A script mutated the DOM (appendChild, textContent=, setAttribute,
    // innerHTML=, ...) since the last layout - re-layout to pick it up.
    if (domState.domDirty) {
        domState.domDirty = false;
        hoverSetLive = false; // the mutation may have freed hovered elements
        doLayout();
    }

    for (const auto& b : layoutRoot.boxes) {

        int screenY = b.y - scrollY + topInset;

        // Cull boxes outside viewport
        if (screenY + b.height < topInset || screenY > height)
            continue;

        // opacity:0 / visibility:hidden (own or inherited): still occupies
        // its layout position (handled above/below), just not painted.
        // Click hit-testing (dispatchClick/onClick) is unaffected - a
        // deliberate simplification, see Layout.h's LayoutBox comment.
        if (b.visuallyHidden) continue;

        if (b.control != LayoutBox::NoControl) {
            drawControl(renderer, b, screenY, timeSeconds);
            continue;
        }

        // Draw border: four thin rects forming a hollow frame, not one
        // filled rect, so a box with a border but no background still shows
        // whatever's behind it through the middle - like a real CSS border.
        if (b.borderWidth > 0) {
            Color bc = parseColor(b.borderColor);
            int bw = b.borderWidth;
            renderer.drawRect(b.x, screenY, b.width, bw, bc);                          // top
            renderer.drawRect(b.x, screenY + b.height - bw, b.width, bw, bc);          // bottom
            renderer.drawRect(b.x, screenY, bw, b.height, bc);                         // left
            renderer.drawRect(b.x + b.width - bw, screenY, bw, b.height, bc);          // right
        }

        // Draw background, inset by the border so it fills only the middle
        if (!b.background.empty()) {
            renderer.drawRect(
                b.x + b.borderWidth,
                screenY + b.borderWidth,
                b.width - 2 * b.borderWidth,
                b.height - 2 * b.borderWidth,
                parseColor(b.background)
            );
        }

        // Draw image
        if (!b.imageSrc.empty()) {
            std::wstring src = resolveImageSrc(pageBaseUrl, b.imageSrc);
            if (!src.empty()) renderer.drawImage(b.x, screenY, b.width, b.height, src);
            continue;
        }

        // Draw text. An empty color means black; links arrive already
        // colored blue by layout (computeStyle), unless CSS overrode it.
        if (!b.text.empty()) {
            renderer.drawText(
                b.x + 4,
                screenY + 4,
                b.text,
                b.fontSize > 0 ? b.fontSize : 14,
                b.color.empty() ? Color{ 0, 0, 0, 1 } : parseColor(b.color),
                b.bold
            );
        }
    }

    drawOpenSelect(renderer); // on top of the page, same treatment main.cpp gives AddressBar
}

std::wstring Engine::title() const {
    return document && document->root ? documentTitle(document->root.get()) : L"";
}

// A :hover restyle is a full relayout (there's no restyle-only path), so
// on a page whose layout already takes longer than this, hover changes are
// ignored rather than stalling the window on every mouse move.
static constexpr double kHoverRelayoutBudgetMs = 50;

void Engine::updateHover(int x, int y) {
    // A pending DOM mutation means layoutRoot.boxes may point at freed
    // elements; render() will relayout first, and the next call catches up.
    if (!document || domState.domDirty) return;

    CSS::HoverSet next;
    for (const Element* el = elementAt(x, y); el; el = el->parent) next.insert(el);
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

std::wstring Engine::linkAt(int x, int y, Renderer& renderer) const {
    if (y < topInset) return L"";
    int docY = y - topInset + scrollY;

    // Later boxes paint on top, so check them first.
    for (auto it = layoutRoot.boxes.rbegin(); it != layoutRoot.boxes.rend(); ++it) {
        const auto& b = *it;
        if (b.href.empty() || b.text.empty()) continue;

        // Text is drawn at (x + 4, y + 4) and is only as wide as its glyphs,
        // so hit-test that area rather than the whole row.
        float fontSize = b.fontSize > 0 ? b.fontSize : 14;
        int textW = static_cast<int>(renderer.measureText(b.text, fontSize, b.bold));
        int left = b.x + 4, top = b.y + 4;
        if (x >= left && x < left + textW && docY >= top && docY < top + b.height)
            return b.href;
    }
    return L"";
}

void Engine::setTopInset(int px) {
    topInset = px;
    doLayout(); // re-clamp scroll for the new viewport height
}
