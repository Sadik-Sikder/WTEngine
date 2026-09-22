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
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <string>
#include <sstream>
#include <vector>

// Minimal color struct for OpenGL

Color parseColor(const std::wstring& str) {
    bool valid = str.size() == 7 && str[0] == L'#';
    for (size_t i = 1; valid && i < str.size(); i++) valid = iswxdigit(str[i]) != 0;

    if (!valid) {
        // default light gray (also for "#gggggg", which std::stoi below would throw on)
        return { 0.94f, 0.94f, 0.94f, 1.0f };
    }

    int r = std::stoi(std::string(str.begin() + 1, str.begin() + 3), nullptr, 16);
    int g = std::stoi(std::string(str.begin() + 3, str.begin() + 5), nullptr, 16);
    int b = std::stoi(std::string(str.begin() + 5, str.begin() + 7), nullptr, 16);

    return { r / 255.0f, g / 255.0f, b / 255.0f, 1.0f };
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
    layoutRoot.measureText = [this](const std::wstring& text, int fontSize) {
        return measurer ? measurer->measureText(text, (float)fontSize)
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
    runScripts();
    doLayout();
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

// Runs every <script> on the page once, in document order, after the whole
// DOM is built (see Layout.h's LayoutRoot::loadImage comment for the same
// "batch, not streaming" simplification applied to images - here it means
// a script can't document.write() more markup mid-parse, since parsing has
// already finished by the time any script runs).
void Engine::runScripts() {
    // Drop the previous page's state FIRST, while its JS runtime still exists:
    // domState holds JSValues (click listeners, pending timers) that must be
    // freed against their own runtime, and quickjs asserts if a runtime is
    // destroyed while any value is still alive. So: state, then old realm, then new.
    domState = DOMBindingState{};
    jsEngine.reset();
    jsEngine = std::make_unique<JSEngine>(); // fresh realm per page
    if (!document || !document->body) return;

    installDOMBindings(jsEngine->context(), document->body.get(), &domState);

    std::vector<Element*> scripts;
    collectScripts(document->body.get(), scripts);

    for (Element* scriptEl : scripts) {
        if (!isClassicScript(scriptEl)) continue;

        std::wstring code;
        auto srcAttr = scriptEl->attrs.find(L"src");
        if (srcAttr != scriptEl->attrs.end() && !srcAttr->second.empty()) {
            std::wstring resolved = resolveUrl(pageBaseUrl, srcAttr->second);
            if (resolved.empty()) continue;
            FetchResult res = fetchPage(resolved);
            if (!res.ok) continue;
            code = res.html;
        }
        else {
            for (auto& child : scriptEl->children) {
                if (child->type == Node::TEXT) code += static_cast<TextNode*>(child.get())->text;
            }
        }
        if (code.empty()) continue;

        std::wstring result = jsEngine->eval(code);
        if (result.rfind(L"Error: ", 0) == 0) {
            std::wstring line = L"[script] " + result + L"\n";
            wprintf(L"%ls", line.c_str());
            OutputDebugStringW(line.c_str());
        }
    }
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

// Fetches and appends the rules of every <link rel="stylesheet"> on the
// page (in document order) to `doc->styles`, after whatever inline <style>
// rules HTMLParser already parsed there. Each fetched stylesheet's own
// `order` values start at 0 (CSS::parseStylesheet has no idea it's one of
// several sources) - offsetting them by doc->styles.size() before
// appending keeps them all after everything already collected, so
// specificity ties still resolve in a sensible document-ish order.
//
// Simplification: every linked stylesheet's rules end up ordered after
// every inline <style> block's rules, regardless of their true relative
// position in the markup - correct for the overwhelmingly common case (a
// stylesheet link in <head>, any inline overrides after it) and only wrong
// if a page deliberately puts an overriding <style> block *before* its
// <link rel=stylesheet> and relies on that ordering to win a specificity
// tie. Like a script fetch, this blocks the UI thread.
static void loadLinkedStylesheets(Document* doc, const std::wstring& baseUrl) {
    if (!doc) return;
    Element* searchRoot = doc->root ? doc->root.get() : doc->body.get();

    std::vector<Element*> links;
    collectStylesheetLinks(searchRoot, links);

    for (Element* link : links) {
        std::wstring resolved = resolveUrl(baseUrl, link->attrs[L"href"]);
        if (resolved.empty()) continue; // e.g. a relative href on a local-file page

        FetchResult res = fetchPage(resolved);
        if (!res.ok) continue;

        std::vector<CSS::Rule> extra = CSS::parseStylesheet(res.html);
        int offset = (int)doc->styles.size();
        for (auto& r : extra) r.order += offset;
        doc->styles.insert(doc->styles.end(),
                            std::make_move_iterator(extra.begin()),
                            std::make_move_iterator(extra.end()));
    }
}

void Engine::parseAndBuild(const std::wstring& html) {
    // The old DOM (and the elements form state points into) is about to go.
    focusedEl = nullptr;
    focusedForm = nullptr;
    hasSubmission = false;
    openSelect = nullptr;

    HTMLParser parser;
    document = parser.parse(html);
    if (document) loadLinkedStylesheets(document.get(), pageBaseUrl);

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

    layoutRoot.boxes.clear();
    layoutRoot.layout();

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

    // A script mutated the DOM (appendChild, textContent=, setAttribute,
    // innerHTML=, ...) since the last layout - re-layout to pick it up.
    if (domState.domDirty) {
        domState.domDirty = false;
        doLayout();
    }

    for (const auto& b : layoutRoot.boxes) {

        int screenY = b.y - scrollY + topInset;

        // Cull boxes outside viewport
        if (screenY + b.height < topInset || screenY > height)
            continue;

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

        // Draw text
        if (!b.text.empty()) {
            renderer.drawText(
                b.x + 4,
                screenY + 4,
                b.text,
                b.fontSize > 0 ? b.fontSize : 14,
                b.href.empty() ? Color{ 0, 0, 0, 1 } : Color{ 0.0f, 0.2f, 0.8f, 1.0f } // links are blue
            );
        }
    }

    drawOpenSelect(renderer); // on top of the page, same treatment main.cpp gives AddressBar
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
        int textW = static_cast<int>(renderer.measureText(b.text, fontSize));
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
