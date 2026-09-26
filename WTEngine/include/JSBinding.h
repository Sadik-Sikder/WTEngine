// JSBinding.h
#pragma once
#include <string>
#include <vector>
#include <memory>

struct JSContext;
struct Element;
struct Node;
struct ListenerStorage; // opaque; holds registered addEventListener callbacks - a JSValue map, so its
                        // definition (and quickjs.h) stays confined to JSBinding.cpp, out of every
                        // Engine.h includer. Same forward-declare-and-define-elsewhere shape as
                        // Engine's own unique_ptr<JSEngine>.
struct TimerStorage;    // opaque, same treatment as ListenerStorage: holds pending setTimeout/
                        // setInterval callbacks (JSValues), so quickjs.h stays out of Engine.h too.
struct FetchStorage;    // opaque, same treatment again: in-flight fetch() calls and their promise
                        // resolve/reject functions.

// Per-page state the DOM bindings need beyond what's reachable from a
// wrapped node's own opaque pointer:
//  - detachedNodes keeps newly-created (createElement/createTextNode) or
//    just-removed (removeChild) nodes alive as long as JS might still hold
//    a reference to them, since nothing else in the C++ tree owns them
//    once they're not attached anywhere.
//  - domDirty is set by any DOM-mutating call; the caller (Engine) polls
//    it once per frame - the same pattern already used for
//    Renderer::imageGeneration() - to know when to re-layout.
//  - listeners holds registered click callbacks, keyed by element.
// Owned by whoever calls installDOMBindings (Engine); installDOMBindings
// only stores a pointer to it, so it must outlive the JSContext.
struct DOMBindingState {
    DOMBindingState();
    ~DOMBindingState();
    DOMBindingState(DOMBindingState&&) noexcept;
    DOMBindingState& operator=(DOMBindingState&&) noexcept;
    DOMBindingState(const DOMBindingState&) = delete;
    DOMBindingState& operator=(const DOMBindingState&) = delete;

    std::vector<std::shared_ptr<Node>> detachedNodes;
    bool domDirty = false;
    std::unique_ptr<ListenerStorage> listeners;
    std::unique_ptr<TimerStorage> timers; // pending setTimeout/setInterval callbacks
    std::unique_ptr<FetchStorage> fetches; // in-flight fetch() calls - see pollFetches

    // The element `document` wraps (set by installDOMBindings) - so a
    // property shared by every node, like `title`, can tell `document`
    // apart from an ordinary element.
    Element* documentEl = nullptr;

    // The page's own URL - set by Engine::runScripts before any script
    // runs. Used to resolve a relative URL passed to location.href=/
    // .replace()/.assign(), and as location.href's own value.
    std::wstring pageUrl;

    // Set by location.href=/.replace()/.assign()/.reload(), or a bare
    // `location = url` / `window.location = url` assignment.
    // Engine::takeNavigation polls this once per frame, the same pattern
    // already used for domDirty and a queued form submission.
    bool navigationPending = false;
    std::wstring navigationUrl;
    bool navigationReplace = false; // true for .replace()/.reload() - see PageHistory::replaceCurrent
};

// Installs `document` (wrapping `documentRoot` - pass the page's <body>)
// and its Node/Event wrapper classes into `ctx`'s global object. Must be
// called once per fresh JSContext, after the DOM is built and before any
// script runs.
void installDOMBindings(JSContext* ctx, Element* documentRoot, DOMBindingState* state);

// Dispatches a click at `target`, bubbling up through its ancestors and
// running every "click" listener registered (via addEventListener) on each
// one in turn. Returns true if any listener called event.preventDefault() -
// the caller (Engine) uses that to suppress its own default click handling
// (e.g. following a link) for this click. Does not support
// stopPropagation(): every ancestor's listeners always run.
bool dispatchClick(JSContext* ctx, Element* target);

// Fires any setTimeout/setInterval callback due by `nowSeconds` - the same
// clock Engine::render's `timeSeconds` parameter already uses (glfwGetTime()
// via main.cpp). Called once per frame from Engine::render, the same
// per-frame polling pattern already used for domDirty/imageGeneration.
void fireDueTimers(JSContext* ctx, double nowSeconds);

// Settles the promise of every fetch() whose background request has
// finished since the last call (resolving with a Response, or rejecting
// with a TypeError on a network error), then runs the resulting promise
// jobs. Called once per frame from Engine::render, like fireDueTimers.
void pollFetches(JSContext* ctx);

// Runs one line typed into the developer console against the page: logs it
// as Input, then its value (formatted like a REPL shows it) as Result, or
// the error it threw. Marks the DOM dirty, since it may have changed it.
void evaluateInConsole(JSContext* ctx, const std::wstring& code);

// The document's title: the text of the first <title> element under
// `root` (outside any <svg>), with runs of whitespace collapsed to single
// spaces and trimmed, as browsers do. Empty if there's no <title>.
std::wstring documentTitle(Element* root);
