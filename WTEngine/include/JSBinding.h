// JSBinding.h
#pragma once
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
