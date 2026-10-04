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
struct NodeWrappers;    // opaque, same treatment again: the one JS object per wrapped node.

// Per-page state the DOM bindings need beyond what's reachable from a
// wrapped node's own opaque pointer:
//  - detachedNodes keeps newly-created (createElement/createTextNode) or
//    just-removed (removeChild) nodes alive as long as JS might still hold
//    a reference to them, since nothing else in the C++ tree owns them
//    once they're not attached anywhere.
//  - domDirty is set by any DOM-mutating call; the caller (Engine) polls
//    it once per frame - the same pattern already used for
//    Renderer::imageGeneration() - to know when to re-layout.
//  - listeners holds registered event listeners and on* handlers, keyed
//    by element (nullptr for window).
//  - wrappers gives each node one JS object for its lifetime, so
//    `e.target === el` holds and expando properties stick. It also keeps
//    every wrapped node alive until the page goes away, so a script holding
//    an element whose parent's innerHTML was replaced never touches freed
//    memory.
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
    std::unique_ptr<NodeWrappers> wrappers;

    // document.readyState: "loading" until every script has run,
    // "interactive" from DOMContentLoaded, "complete" from load. Advanced by
    // advanceReadyState.
    std::wstring readyState = L"loading";

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

// --- Firing DOM events from the engine --------------------------------
// Each of these builds a trusted event (isTrusted = true) and runs it
// through the capture, target and bubble phases along `target`'s ancestors,
// then window - calling addEventListener listeners, on* handler properties
// and inline on*="..." attributes. `target` nullptr means window itself
// (e.g. "load"). Each returns true if a listener called preventDefault(),
// which the engine uses to skip that event's default action (following a
// link, typing a character, submitting a form, ...).
//
// A listener can change or free parts of the DOM: the caller must not
// assume any Element* it held is still in the page afterwards
// (Engine::stillInPage).

// Mouse position and button for a MouseEvent: client* is relative to the
// viewport, page* to the document (both in page pixels).
struct MouseInfo {
    double clientX = 0, clientY = 0, pageX = 0, pageY = 0;
    int button = 0; // 0 = primary
};

// A key press or release, as KeyboardEvent describes it: `key` is what the
// key means ("a", "A", "Enter", "ArrowLeft"), `code` which physical key it
// is ("KeyA", "Enter"), `keyCode` the legacy numeric code.
struct KeyInfo {
    std::wstring key, code;
    int keyCode = 0;
    int location = 0; // 1 = left modifier, 2 = right, 3 = numpad
    bool ctrl = false, shift = false, alt = false, meta = false;
    bool repeat = false;
};

// A plain Event ("change", "focus", "DOMContentLoaded", ...).
bool fireEvent(JSContext* ctx, Element* target, const wchar_t* type, bool bubbles, bool cancelable);
// "click", "mouseover", "mouseout", "mouseenter", "mouseleave".
bool fireMouseEvent(JSContext* ctx, Element* target, const wchar_t* type, const MouseInfo& info,
                    Element* relatedTarget = nullptr);
// "keydown" / "keyup".
bool fireKeyboardEvent(JSContext* ctx, Element* target, const wchar_t* type, const KeyInfo& info);
// "input" after a field's text changed; `inputType` as InputEvent names it
// ("insertText", "deleteContentBackward", ...), `data` the inserted text.
void fireInputEvent(JSContext* ctx, Element* target, const wchar_t* inputType, const std::wstring& data);
// "submit" on `form`; `submitter` is the button that submitted it, if any.
bool fireSubmitEvent(JSContext* ctx, Element* form, Element* submitter);

// Sets document.readyState ("interactive" / "complete") and fires
// "readystatechange" at document.
void advanceReadyState(JSContext* ctx, const wchar_t* state);

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
