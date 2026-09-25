// JSBinding.cpp
#define NOMINMAX
#include "JSBinding.h"
#include "JSEngine.h"
#include "quickjs.h"
#include "DOM.h"
#include "CSS.h"
#include "HTMLParser.h"
#include "Fetcher.h"
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <map>
#include <mutex>

#define countof(x) (sizeof(x) / sizeof((x)[0]))

// Every quickjs string is UTF-8; the rest of WTEngine is wstring
// throughout - same conversion helpers JSEngine.cpp, Fetcher.cpp and
// main.cpp each keep their own copy of.
static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

static std::wstring utf8ToWide(const char* s) {
    if (!s || !*s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring out(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    return out;
}

static JSValue jsStr(JSContext* ctx, const std::wstring& w) {
    std::string u8 = wideToUtf8(w);
    return JS_NewStringLen(ctx, u8.c_str(), u8.size());
}

// A single string argument, converted; "" if missing or not a string.
static std::wstring argStr(JSContext* ctx, int argc, JSValueConst* argv, int i) {
    if (i >= argc) return L"";
    const char* s = JS_ToCString(ctx, argv[i]);
    std::wstring w = utf8ToWide(s);
    JS_FreeCString(ctx, s);
    return w;
}

// ---------------------------------------------------------------------
// Registered addEventListener callbacks, keyed by element. Owns a JSValue
// per callback (JS_DupValue'd on addEventListener), so it needs the
// JSContext to free them again - set once by installDOMBindings. Freeing
// happens in the destructor, which unique_ptr<ListenerStorage>'s own
// move-assignment already invokes correctly for both DOMBindingState's
// full destruction and its per-navigation `= DOMBindingState{}` reset in
// Engine::beginScripts() (replacing the pointer destroys the old target).
struct ListenerStorage {
    std::map<Element*, std::vector<JSValue>> click;
    JSContext* ctx = nullptr;
    ~ListenerStorage() {
        if (!ctx) return;
        for (auto& [el, fns] : click)
            for (JSValue fn : fns) JS_FreeValue(ctx, fn);
    }
};

// ---------------------------------------------------------------------
// Pending setTimeout/setInterval callbacks. Owns a JSValue per timer (like
// ListenerStorage owns one per click listener), freed either when the timer
// fires/is cleared or, for whatever's left, in the destructor - same
// per-navigation reset as ListenerStorage via DOMBindingState's move
// assignment.
struct Timer {
    int id;
    JSValue callback;
    double periodSeconds; // one-shot delay, or repeat period for setInterval
    double nextFire;      // absolute engine-clock time this timer is next due
    bool repeating;
};
struct TimerStorage {
    std::vector<Timer> timers;
    int nextId = 1;
    double now = 0; // last time seen via fireDueTimers; baseline new timers schedule against
    JSContext* ctx = nullptr;
    ~TimerStorage() {
        if (!ctx) return;
        for (auto& t : timers) JS_FreeValue(ctx, t.callback);
    }
};

// ---------------------------------------------------------------------
// In-flight fetch() calls. The request itself runs on Fetcher's shared
// network thread (fetchHttpAsync - async I/O, no thread per request),
// whose completion callback only ever touches its FetchSlot - never a
// JSValue, since quickjs isn't thread-safe. pollFetches, on the UI
// thread, settles the promise once the slot is done. A navigation drops
// the storage (freeing the promise functions); a request still in flight
// then completes into a slot nothing reads anymore (the same
// abandon-in-place pattern PageLoader uses).
struct FetchSlot {
    std::mutex mutex;
    bool done = false;
    HttpResponse response;
};
struct PendingFetch {
    std::shared_ptr<FetchSlot> slot;
    JSValue resolve, reject;
};
struct FetchStorage {
    std::vector<PendingFetch> pending;
    JSContext* ctx = nullptr;
    ~FetchStorage() {
        if (!ctx) return;
        for (auto& f : pending) { JS_FreeValue(ctx, f.resolve); JS_FreeValue(ctx, f.reject); }
    }
};

DOMBindingState::DOMBindingState()
    : listeners(std::make_unique<ListenerStorage>()), timers(std::make_unique<TimerStorage>()),
      fetches(std::make_unique<FetchStorage>()) {}
DOMBindingState::~DOMBindingState() = default;
DOMBindingState::DOMBindingState(DOMBindingState&&) noexcept = default;
DOMBindingState& DOMBindingState::operator=(DOMBindingState&&) noexcept = default;

// ---------------------------------------------------------------------
// Node wrapper: one JS class for both Element and TextNode. Real DOM
// splits these into separate types with Node as their common base; here
// one class covers both and methods that only make sense for one kind
// (e.g. appendChild on a TextNode) just no-op via the type checks below,
// which keeps the binding to a single prototype instead of two.
// ---------------------------------------------------------------------

static JSClassID js_node_class_id = 0;

// Opaque pointers here are non-owning views into the page's DOM tree (or
// DOMBindingState::detachedNodes) - never something this wrapper itself
// must free, so no finalizer is needed.
static JSClassDef js_node_class_def = { "Node", nullptr };

static JSValue wrapNode(JSContext* ctx, Node* node) {
    if (!node) return JS_NULL;
    JSValue obj = JS_NewObjectClass(ctx, js_node_class_id);
    if (!JS_IsException(obj)) JS_SetOpaque(obj, node);
    return obj;
}

// JS_GetOpaque (not ...2) never raises an exception on a class mismatch;
// callers just treat a null result as "not a valid node" and return
// gracefully, so a leftover pending exception can never surprise later,
// unrelated JS code.
static Node* unwrapNode(JSValueConst v) {
    return static_cast<Node*>(JS_GetOpaque(v, js_node_class_id));
}

static Element* unwrapElement(JSValueConst v) {
    Node* n = unwrapNode(v);
    return (n && n->type == Node::ELEMENT) ? static_cast<Element*>(n) : nullptr;
}

static DOMBindingState* bindingState(JSContext* ctx) {
    return static_cast<DOMBindingState*>(JS_GetContextOpaque(ctx));
}

static void markDirty(JSContext* ctx) {
    if (DOMBindingState* s = bindingState(ctx)) s->domDirty = true;
}

// --- textContent ------------------------------------------------------

static void gatherText(Node* n, std::wstring& out) {
    if (!n) return;
    if (n->type == Node::TEXT) { out += static_cast<TextNode*>(n)->text; return; }
    for (auto& c : static_cast<Element*>(n)->children) gatherText(c.get(), out);
}

static JSValue js_get_textContent(JSContext* ctx, JSValueConst this_val) {
    Node* n = unwrapNode(this_val);
    if (!n) return JS_NULL;
    std::wstring out;
    gatherText(n, out);
    return jsStr(ctx, out);
}

static JSValue js_set_textContent(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    Node* n = unwrapNode(this_val);
    if (!n) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, val);
    std::wstring text = utf8ToWide(s);
    JS_FreeCString(ctx, s);

    if (n->type == Node::TEXT) {
        static_cast<TextNode*>(n)->text = text;
    }
    else {
        auto* el = static_cast<Element*>(n);
        el->children.clear();
        if (!text.empty()) el->children.push_back(std::make_shared<TextNode>(text));
    }
    markDirty(ctx);
    return JS_UNDEFINED;
}

// --- innerHTML (write-only: no HTML serializer exists yet for a getter) --

static JSValue js_set_innerHTML(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    Element* el = unwrapElement(this_val);
    if (!el) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, val);
    std::wstring html = utf8ToWide(s);
    JS_FreeCString(ctx, s);

    // `html` has no top-level <body>, so HTMLParser::parse() synthesizes
    // one wrapping every top-level node parsed from it - exactly a
    // fragment's node list, reused as-is with no separate "fragment mode".
    HTMLParser parser;
    auto frag = parser.parse(html);
    el->children = frag->body->children;
    // The moved children's `parent` still points at frag->body, which is
    // about to be destroyed (frag is local) - repoint them at `el`, their
    // real new parent, before that dangles.
    for (auto& child : el->children) {
        if (child->type == Node::ELEMENT) static_cast<Element*>(child.get())->parent = el;
    }
    markDirty(ctx);
    return JS_UNDEFINED;
}

// --- tagName / id / className -----------------------------------------

static JSValue js_get_tagName(JSContext* ctx, JSValueConst this_val) {
    Element* el = unwrapElement(this_val);
    if (!el) return JS_NULL;
    std::wstring upper = el->tag;
    for (auto& c : upper) c = (wchar_t)towupper(c);
    return jsStr(ctx, upper);
}

static const wchar_t* const kMagicAttrNames[] = { L"id", L"class" };

static JSValue js_get_attr_magic(JSContext* ctx, JSValueConst this_val, int magic) {
    Element* el = unwrapElement(this_val);
    std::wstring val;
    if (el) {
        auto it = el->attrs.find(kMagicAttrNames[magic]);
        if (it != el->attrs.end()) val = it->second;
    }
    return jsStr(ctx, val);
}

static JSValue js_set_attr_magic(JSContext* ctx, JSValueConst this_val, JSValueConst v, int magic) {
    if (Element* el = unwrapElement(this_val)) {
        const char* s = JS_ToCString(ctx, v);
        el->attrs[kMagicAttrNames[magic]] = utf8ToWide(s);
        JS_FreeCString(ctx, s);
        markDirty(ctx);
    }
    return JS_UNDEFINED;
}

// --- get/set/has/removeAttribute ---------------------------------------

static JSValue js_getAttribute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    Element* el = unwrapElement(this_val);
    if (!el || argc < 1) return JS_NULL;
    auto it = el->attrs.find(argStr(ctx, argc, argv, 0));
    return it == el->attrs.end() ? JS_NULL : jsStr(ctx, it->second);
}

static JSValue js_setAttribute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (Element* el = unwrapElement(this_val); el && argc >= 2) {
        el->attrs[argStr(ctx, argc, argv, 0)] = argStr(ctx, argc, argv, 1);
        markDirty(ctx);
    }
    return JS_UNDEFINED;
}

static JSValue js_hasAttribute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    Element* el = unwrapElement(this_val);
    bool has = el && argc >= 1 && el->attrs.count(argStr(ctx, argc, argv, 0)) > 0;
    return JS_NewBool(ctx, has);
}

static JSValue js_removeAttribute(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    if (Element* el = unwrapElement(this_val); el && argc >= 1) {
        el->attrs.erase(argStr(ctx, argc, argv, 0));
        markDirty(ctx);
    }
    return JS_UNDEFINED;
}

// --- createElement / createTextNode ------------------------------------
// Bound on the shared Node prototype (so document.createElement works,
// since `document` is just a wrapped Element - see installDOMBindings)
// rather than a separate Document-only class; harmless if also called on
// a plain element, since creation doesn't actually depend on `this`.

static JSValue js_createElement(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    std::wstring tag = argStr(ctx, argc, argv, 0);
    for (auto& c : tag) c = (wchar_t)towlower(c);

    auto el = std::make_shared<Element>(tag);
    if (DOMBindingState* s = bindingState(ctx)) s->detachedNodes.push_back(el);
    return wrapNode(ctx, el.get());
}

static JSValue js_createTextNode(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    auto node = std::make_shared<TextNode>(argStr(ctx, argc, argv, 0));
    if (DOMBindingState* s = bindingState(ctx)) s->detachedNodes.push_back(node);
    return wrapNode(ctx, node.get());
}

// --- appendChild / removeChild ------------------------------------------

// Only attaches a node created via createElement/createTextNode and not
// yet attached anywhere - the common "build then insert" pattern. Moving
// an already-attached node (re-parenting) isn't supported: doing that
// safely would need a way to get a shared_ptr for an arbitrary Element*
// already owned elsewhere in the tree, which the raw-pointer wrapping used
// throughout this codebase (LayoutBox::el, Engine::focusedEl, ...) doesn't
// provide. Silently does nothing in that case rather than risk a
// dangling/double-owned node.
static JSValue js_appendChild(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    Element* parent = unwrapElement(this_val);
    if (!parent || argc < 1) return JS_NULL;
    Node* child = unwrapNode(argv[0]);
    if (!child) return JS_NULL;

    DOMBindingState* state = bindingState(ctx);
    if (!state) return JS_NULL;

    for (size_t i = 0; i < state->detachedNodes.size(); i++) {
        if (state->detachedNodes[i].get() == child) {
            if (child->type == Node::ELEMENT) static_cast<Element*>(child)->parent = parent;
            parent->children.push_back(state->detachedNodes[i]);
            state->detachedNodes.erase(state->detachedNodes.begin() + i);
            markDirty(ctx);
            return JS_DupValue(ctx, argv[0]);
        }
    }
    return JS_NULL;
}

// Detaches (not destroys) the child: its shared_ptr moves into
// detachedNodes, so it stays alive - and still usable from JS, matching
// real removeChild returning the removed node - until the page navigates
// away, even though nothing in the visible tree references it anymore.
static JSValue js_removeChild(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    Element* parent = unwrapElement(this_val);
    if (!parent || argc < 1) return JS_NULL;
    Node* child = unwrapNode(argv[0]);
    if (!child) return JS_NULL;

    for (size_t i = 0; i < parent->children.size(); i++) {
        if (parent->children[i].get() == child) {
            if (DOMBindingState* state = bindingState(ctx)) state->detachedNodes.push_back(parent->children[i]);
            if (child->type == Node::ELEMENT) static_cast<Element*>(child)->parent = nullptr;
            parent->children.erase(parent->children.begin() + i);
            markDirty(ctx);
            return JS_DupValue(ctx, argv[0]);
        }
    }
    return JS_NULL;
}

// --- getElementById / getElementsByTagName ------------------------------
// Bound on the shared Node prototype rather than a Document-only class
// (see createElement above) - so technically callable, and searching that
// element's own subtree, on any element, not just `document`. A harmless
// over-exposure for a toy engine, not spec-accurate.

static void findById(Node* n, const std::wstring& id, Element*& out) {
    if (!n || out || n->type != Node::ELEMENT) return;
    auto* el = static_cast<Element*>(n);
    auto it = el->attrs.find(L"id");
    if (it != el->attrs.end() && it->second == id) { out = el; return; }
    for (auto& c : el->children) { findById(c.get(), id, out); if (out) return; }
}

static JSValue js_getElementById(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    Element* root = unwrapElement(this_val);
    if (!root || argc < 1) return JS_NULL;
    std::wstring id = argStr(ctx, argc, argv, 0);

    Element* found = nullptr;
    for (auto& c : root->children) { findById(c.get(), id, found); if (found) break; }
    return wrapNode(ctx, found);
}

static void findByTag(Node* n, const std::wstring& tag, std::vector<Element*>& out) {
    if (!n || n->type != Node::ELEMENT) return;
    auto* el = static_cast<Element*>(n);
    if (el->tag == tag) out.push_back(el);
    for (auto& c : el->children) findByTag(c.get(), tag, out);
}

static JSValue js_getElementsByTagName(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    Element* root = unwrapElement(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (!root || argc < 1) return arr;
    std::wstring tag = argStr(ctx, argc, argv, 0);
    for (auto& c : tag) c = (wchar_t)towlower(c);

    std::vector<Element*> found;
    for (auto& c : root->children) findByTag(c.get(), tag, found);
    for (size_t i = 0; i < found.size(); i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, wrapNode(ctx, found[i]));
    return arr;
}

// --- querySelector / querySelectorAll -----------------------------------
// Reuses CSS::parseSelector and CSS::matches - the same selector grammar
// and matcher stylesheet rules use against Layout's ancestorStack - rather
// than duplicating either here.

static bool parseSingleSelector(const std::wstring& sel, CSS::Rule& outRule) {
    return CSS::parseSelector(sel, outRule.chain);
}

// `ancestors` starts empty at `root` (the search root, e.g. `document`),
// same as Layout's ancestorStack starts empty at the true document root -
// so a descendant-combinator selector like "div p" only considers
// ancestors within the subtree being searched, not root's real ancestors
// further up the page (which querySelector on a scoped element wouldn't
// see either, in a real browser, for this simple case).
static void queryAll(Element* el, const CSS::Rule& rule, std::vector<Element*>& ancestors,
                     std::vector<Element*>& out, bool stopAtFirst) {
    for (auto& child : el->children) {
        if (child->type != Node::ELEMENT) continue;
        auto* ce = static_cast<Element*>(child.get());
        if (CSS::matches(rule, ancestors, ce)) {
            out.push_back(ce);
            if (stopAtFirst) return;
        }
        ancestors.push_back(ce);
        queryAll(ce, rule, ancestors, out, stopAtFirst);
        ancestors.pop_back();
        if (stopAtFirst && !out.empty()) return;
    }
}

static JSValue js_querySelector(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    Element* root = unwrapElement(this_val);
    if (!root || argc < 1) return JS_NULL;

    CSS::Rule rule;
    if (!parseSingleSelector(argStr(ctx, argc, argv, 0), rule)) return JS_NULL;

    std::vector<Element*> ancestors, found;
    queryAll(root, rule, ancestors, found, true);
    return found.empty() ? JS_NULL : wrapNode(ctx, found[0]);
}

static JSValue js_querySelectorAll(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    Element* root = unwrapElement(this_val);
    JSValue arr = JS_NewArray(ctx);
    if (!root || argc < 1) return arr;

    CSS::Rule rule;
    if (!parseSingleSelector(argStr(ctx, argc, argv, 0), rule)) return arr;

    std::vector<Element*> ancestors, found;
    queryAll(root, rule, ancestors, found, false);
    for (size_t i = 0; i < found.size(); i++)
        JS_SetPropertyUint32(ctx, arr, (uint32_t)i, wrapNode(ctx, found[i]));
    return arr;
}

// --- addEventListener ---------------------------------------------------
// Only "click" is supported; nothing else fires. No removeEventListener
// yet (documented gap) - correctly identifying "the same function" to
// remove needs a JSValue equality check this phase doesn't add.

static JSValue js_addEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    Element* el = unwrapElement(this_val);
    if (!el || argc < 2) return JS_UNDEFINED;
    if (argStr(ctx, argc, argv, 0) != L"click") return JS_UNDEFINED;
    if (!JS_IsFunction(ctx, argv[1])) return JS_UNDEFINED;

    if (DOMBindingState* state = bindingState(ctx))
        state->listeners->click[el].push_back(JS_DupValue(ctx, argv[1]));
    return JS_UNDEFINED;
}

// --- Event: passed to a click listener, carries target + preventDefault ---
// Opaque data is a stack-owned ClickEventData valid only for the duration
// of the synchronous dispatchClick() call that creates it - never stored
// anywhere JS could still reach after that call returns - so, like Node,
// no finalizer is needed.

struct ClickEventData {
    bool prevented = false;
    Element* target = nullptr;
};

static JSClassID js_event_class_id = 0;
static JSClassDef js_event_class_def = { "Event", nullptr };

static JSValue js_event_preventDefault(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    if (auto* data = static_cast<ClickEventData*>(JS_GetOpaque(this_val, js_event_class_id)))
        data->prevented = true;
    return JS_UNDEFINED;
}

static JSValue js_event_get_target(JSContext* ctx, JSValueConst this_val) {
    auto* data = static_cast<ClickEventData*>(JS_GetOpaque(this_val, js_event_class_id));
    return data ? wrapNode(ctx, data->target) : JS_NULL;
}

static const JSCFunctionListEntry js_event_proto_funcs[] = {
    JS_CFUNC_DEF("preventDefault", 0, js_event_preventDefault),
    JS_CGETSET_DEF("target", js_event_get_target, nullptr),
};

bool dispatchClick(JSContext* ctx, Element* target) {
    DOMBindingState* state = bindingState(ctx);
    if (!state || !target) return false;

    ClickEventData data;
    data.target = target;
    JSValue eventObj = JS_NewObjectClass(ctx, js_event_class_id);
    if (JS_IsException(eventObj)) return false;
    JS_SetOpaque(eventObj, &data);

    // Bubble through every ancestor regardless of preventDefault() (which
    // only suppresses the default action, not propagation - stopPropagation
    // itself isn't supported here, a documented gap).
    for (Element* el = target; el; el = el->parent) {
        auto it = state->listeners->click.find(el);
        if (it == state->listeners->click.end()) continue;

        // Copy: a listener could register/unregister another one on this
        // same element during dispatch, which would otherwise invalidate
        // the map iterator/vector this loop is walking.
        std::vector<JSValue> fns = it->second;
        for (JSValue fn : fns) {
            ArmScriptWatchdog(ctx);
            JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, 1, &eventObj);
            if (JS_IsException(result)) JS_FreeValue(ctx, JS_GetException(ctx)); // swallow; keep bubbling
            JS_FreeValue(ctx, result);
            runPendingJobs(ctx); // this listener's promise callbacks, like a microtask checkpoint
        }
    }

    bool prevented = data.prevented;
    JS_FreeValue(ctx, eventObj);
    return prevented;
}

// --- setTimeout / setInterval / clearTimeout / clearInterval -----------
// Globals, not Node methods - registered directly on the global object in
// installDOMBindings below. setTimeout and setInterval share one magic-
// tagged function (magic 0 = one-shot, 1 = repeating), the same mechanism
// already used for id/className (js_get_attr_magic/js_set_attr_magic)
// below, and the same pattern quickjs-ng's own quickjs-libc.c uses for its
// os.setTimeout/os.setInterval. clearTimeout/clearInterval both map to one
// function, matching quickjs-libc.c doing the same for os.clearTimeout/
// os.clearInterval.

static JSValue js_setTimer(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv, int magic) {
    DOMBindingState* state = bindingState(ctx);
    if (!state || argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);

    double delayMs = 0;
    if (argc >= 2) JS_ToFloat64(ctx, &delayMs, argv[1]);
    double delaySeconds = std::max(delayMs, 0.0) / 1000.0;

    TimerStorage& ts = *state->timers;
    int id = ts.nextId++;
    ts.timers.push_back({ id, JS_DupValue(ctx, argv[0]), delaySeconds, ts.now + delaySeconds, magic == 1 });
    return JS_NewInt32(ctx, id);
}

static JSValue js_clearTimer(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    DOMBindingState* state = bindingState(ctx);
    if (!state || argc < 1) return JS_UNDEFINED;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);

    auto& v = state->timers->timers;
    auto it = std::find_if(v.begin(), v.end(), [&](const Timer& t) { return t.id == id; });
    if (it != v.end()) {
        JS_FreeValue(ctx, it->callback);
        v.erase(it);
    }
    return JS_UNDEFINED;
}

void fireDueTimers(JSContext* ctx, double nowSeconds) {
    DOMBindingState* state = bindingState(ctx);
    if (!state) return;
    TimerStorage& ts = *state->timers;
    ts.now = nowSeconds;

    // Snapshot which timers are due first, then re-look-up each by id right
    // before calling it - same iterate-a-copy defense dispatchClick uses
    // above, since a callback can cancel another pending timer (or itself)
    // mid-batch. A timer scheduled by a callback during this pass (e.g.
    // setTimeout(fn, 0) called from inside another timer) is never in this
    // snapshot, so it can't fire until a later frame - no same-frame
    // recursion risk.
    std::vector<int> dueIds;
    for (auto& t : ts.timers) if (t.nextFire <= nowSeconds) dueIds.push_back(t.id);

    for (int id : dueIds) {
        auto it = std::find_if(ts.timers.begin(), ts.timers.end(), [&](const Timer& t) { return t.id == id; });
        if (it == ts.timers.end()) continue; // cancelled by an earlier callback in this same batch

        JSValue fn = JS_DupValue(ctx, it->callback); // survives `it` being erased/reallocated below
        bool repeating = it->repeating;
        double period = it->periodSeconds;

        ArmScriptWatchdog(ctx);
        JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, 0, nullptr);
        bool killedByWatchdog = false;
        if (JS_IsException(result)) {
            JSValue exc = JS_GetException(ctx);
            killedByWatchdog = JS_IsUncatchableError(exc); // vs. an ordinary script throw
            JS_FreeValue(ctx, exc); // swallow; keep firing the rest
        }
        JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, fn);
        runPendingJobs(ctx); // this callback's promise continuations, before the next timer

        it = std::find_if(ts.timers.begin(), ts.timers.end(), [&](const Timer& t) { return t.id == id; });
        if (it == ts.timers.end()) continue; // cleared itself (or was cleared) during its own call
        if (killedByWatchdog) {
            // A watchdog-killed callback would just hang again next fire -
            // repeating it would burn kScriptTimeout worth of CPU forever
            // instead of the one-time freeze this whole mechanism exists to
            // prevent. Drop it instead of rescheduling.
            JS_FreeValue(ctx, it->callback);
            ts.timers.erase(it);
        }
        else if (repeating) it->nextFire = nowSeconds + period; // resync to now, not backlog-catch-up
        else {
            JS_FreeValue(ctx, it->callback);
            ts.timers.erase(it);
        }
    }
}

// --- location: reading the page's URL, and JS-driven navigation --------

// Resolves `href` against the page's own URL (so a relative href works,
// same as an <a href> click) and queues it for Engine::takeNavigation to
// pick up next frame - the same "set a flag, let the main loop act on it"
// pattern already used for a form submission. Silently does nothing for
// an unresolvable href (a fragment, javascript:, a relative href on a
// local-file page, ...), same as a plain link click in that situation.
static void queueNavigation(JSContext* ctx, const std::wstring& href, bool replace) {
    DOMBindingState* state = bindingState(ctx);
    if (!state) return;
    std::wstring resolved = resolveUrl(state->pageUrl, href);
    if (resolved.empty()) return;
    state->navigationUrl = resolved;
    state->navigationReplace = replace;
    state->navigationPending = true;
}

static JSValue js_location_get_href(JSContext* ctx, JSValueConst /*this_val*/) {
    DOMBindingState* state = bindingState(ctx);
    return jsStr(ctx, state ? state->pageUrl : L"");
}

static JSValue js_location_set_href(JSContext* ctx, JSValueConst /*this_val*/, JSValueConst val) {
    const char* s = JS_ToCString(ctx, val);
    std::wstring href = utf8ToWide(s);
    JS_FreeCString(ctx, s);
    queueNavigation(ctx, href, false);
    return JS_UNDEFINED;
}

static JSValue js_location_replace(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    queueNavigation(ctx, argStr(ctx, argc, argv, 0), true); // like real replace(): no Back-button stop
    return JS_UNDEFINED;
}

static JSValue js_location_assign(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    queueNavigation(ctx, argStr(ctx, argc, argv, 0), false);
    return JS_UNDEFINED;
}

static JSValue js_location_reload(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    if (DOMBindingState* state = bindingState(ctx)) queueNavigation(ctx, state->pageUrl, true);
    return JS_UNDEFINED;
}

static JSValue js_location_toString(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    return js_location_get_href(ctx, this_val);
}

static const JSCFunctionListEntry js_location_proto_funcs[] = {
    JS_CGETSET_DEF("href", js_location_get_href, js_location_set_href),
    JS_CFUNC_DEF("replace", 1, js_location_replace),
    JS_CFUNC_DEF("assign", 1, js_location_assign),
    JS_CFUNC_DEF("reload", 0, js_location_reload),
    JS_CFUNC_DEF("toString", 0, js_location_toString),
};

// window.location / the bare global `location` (window aliases the global
// object, so these are the same property): built fresh on every read as a
// plain object with the function list above installed directly onto it.
// No dedicated JS class is needed - unlike Node/Event, none of these
// methods read `this_val`'s own opaque data, only bindingState(ctx) - so
// there's nothing that needs to survive between one read of `location`
// and the next.
static JSValue js_get_location(JSContext* ctx, JSValueConst /*this_val*/) {
    JSValue obj = JS_NewObject(ctx);
    if (!JS_IsException(obj))
        JS_SetPropertyFunctionList(ctx, obj, js_location_proto_funcs, countof(js_location_proto_funcs));
    return obj;
}

// `window.location = "..."` / a bare `location = "..."` assignment -
// coerces the whole right-hand side to a URL string and navigates, the
// same as setting location.href.
static JSValue js_set_location(JSContext* ctx, JSValueConst /*this_val*/, JSValueConst val) {
    const char* s = JS_ToCString(ctx, val);
    std::wstring href = utf8ToWide(s);
    JS_FreeCString(ctx, s);
    queueNavigation(ctx, href, false);
    return JS_UNDEFINED;
}

// --- value / checked (form controls) ------------------------------------
// The engine keeps all form state in the DOM - an input's text in its
// `value` attribute, a checkbox's state as a `checked` attribute, the
// chosen <option> marked `selected` (see EngineForms.cpp) - so these are
// plain reads/writes of those attributes. Engine::render notices a JS
// change to the focused field's value and reloads its editor from it.

static std::vector<Element*> optionsOf(Element* selectEl) {
    std::vector<Element*> out;
    for (auto& c : selectEl->children)
        if (c->type == Node::ELEMENT && static_cast<Element*>(c.get())->tag == L"option")
            out.push_back(static_cast<Element*>(c.get()));
    return out;
}

static std::wstring optionValueOf(Element* opt) {
    auto it = opt->attrs.find(L"value");
    if (it != opt->attrs.end()) return it->second;
    std::wstring text;
    gatherText(opt, text);
    return text;
}

static JSValue js_get_value(JSContext* ctx, JSValueConst this_val) {
    Element* el = unwrapElement(this_val);
    if (!el) return JS_UNDEFINED;
    if (el->tag == L"input" || el->tag == L"button") {
        auto it = el->attrs.find(L"value");
        if (it != el->attrs.end()) return jsStr(ctx, it->second);
        auto type = el->attrs.find(L"type");
        bool checkable = type != el->attrs.end() && (type->second == L"checkbox" || type->second == L"radio");
        return jsStr(ctx, checkable ? L"on" : L""); // the HTML default for a checkbox/radio with no value
    }
    if (el->tag == L"textarea") {
        std::wstring text;
        gatherText(el, text);
        return jsStr(ctx, text);
    }
    if (el->tag == L"option") return jsStr(ctx, optionValueOf(el));
    if (el->tag == L"select") {
        auto opts = optionsOf(el);
        for (Element* o : opts) if (o->attrs.count(L"selected")) return jsStr(ctx, optionValueOf(o));
        return jsStr(ctx, opts.empty() ? L"" : optionValueOf(opts[0])); // unmarked = first, as EngineForms draws it
    }
    return JS_UNDEFINED;
}

static JSValue js_set_value(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    Element* el = unwrapElement(this_val);
    if (!el) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, val);
    std::wstring v = utf8ToWide(s);
    JS_FreeCString(ctx, s);

    if (el->tag == L"textarea") {
        el->children.clear();
        if (!v.empty()) el->children.push_back(std::make_shared<TextNode>(v));
    }
    else if (el->tag == L"select") {
        // Selects the first option with that value; none matching leaves
        // nothing marked (the engine then shows the first option).
        bool found = false;
        for (Element* o : optionsOf(el)) {
            if (!found && optionValueOf(o) == v) { o->attrs[L"selected"] = L""; found = true; }
            else o->attrs.erase(L"selected");
        }
    }
    else {
        el->attrs[L"value"] = v;
    }
    markDirty(ctx);
    return JS_UNDEFINED;
}

static JSValue js_get_checked(JSContext* ctx, JSValueConst this_val) {
    Element* el = unwrapElement(this_val);
    return JS_NewBool(ctx, el && el->attrs.count(L"checked") > 0);
}

static JSValue js_set_checked(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    if (Element* el = unwrapElement(this_val)) {
        if (JS_ToBool(ctx, val)) el->attrs[L"checked"] = L"";
        else el->attrs.erase(L"checked");
        markDirty(ctx);
    }
    return JS_UNDEFINED;
}

// --- title ----------------------------------------------------------------
// document.title reads/writes the page's <title>; on any other element,
// `title` is its title="" attribute (the tooltip text), as in a real DOM.

static Element* findTitleElement(Element* el) {
    if (!el || el->tag == L"svg") return nullptr; // an <svg>'s own <title> isn't the document's
    if (el->tag == L"title") return el;
    for (auto& c : el->children) {
        if (c->type != Node::ELEMENT) continue;
        if (Element* t = findTitleElement(static_cast<Element*>(c.get()))) return t;
    }
    return nullptr;
}

// `document` wraps <body>; the <title> normally lives in <head>, a sibling,
// so the search starts from the top of the tree.
static Element* topOf(Element* el) {
    while (el && el->parent) el = el->parent;
    return el;
}

std::wstring documentTitle(Element* root) {
    Element* t = findTitleElement(root);
    if (!t) return L"";
    std::wstring raw, out;
    gatherText(t, raw);
    for (wchar_t c : raw) {
        if (iswspace(c)) { if (!out.empty() && out.back() != L' ') out.push_back(L' '); }
        else out.push_back(c);
    }
    if (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

static JSValue js_get_title(JSContext* ctx, JSValueConst this_val) {
    Element* el = unwrapElement(this_val);
    if (!el) return jsStr(ctx, L"");
    DOMBindingState* state = bindingState(ctx);
    if (state && el == state->documentEl) return jsStr(ctx, documentTitle(topOf(el)));
    auto it = el->attrs.find(L"title");
    return jsStr(ctx, it == el->attrs.end() ? L"" : it->second);
}

static JSValue js_set_title(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    Element* el = unwrapElement(this_val);
    if (!el) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, val);
    std::wstring v = utf8ToWide(s);
    JS_FreeCString(ctx, s);

    DOMBindingState* state = bindingState(ctx);
    if (!state || el != state->documentEl) {
        el->attrs[L"title"] = v;
        markDirty(ctx);
        return JS_UNDEFINED;
    }

    // document.title = ...: rewrite the existing <title>, or create one in
    // <head> (or at the top of the tree if there's no <head> either). The
    // window title picks it up on the next frame (Engine::title()).
    Element* top = topOf(el);
    Element* t = findTitleElement(top);
    if (!t) {
        Element* head = nullptr;
        for (auto& c : top->children)
            if (c->type == Node::ELEMENT && static_cast<Element*>(c.get())->tag == L"head")
                head = static_cast<Element*>(c.get());
        Element* parent = head ? head : top;
        auto created = std::make_shared<Element>(L"title");
        created->parent = parent;
        parent->children.insert(parent->children.begin(), created);
        t = created.get();
    }
    t->children.clear();
    if (!v.empty()) t->children.push_back(std::make_shared<TextNode>(v));
    return JS_UNDEFINED;
}

// --- fetch ----------------------------------------------------------------
// The public fetch() is defined in kBootstrapJS below (option parsing,
// Headers, the Response object); it calls this native half to actually
// send the request: __wtFetch(url, method, [name, value, ...], body).

// Resolves a fetch() URL. From an http(s) page, this is the same resolution
// a link gets. From a page loaded off disk, a relative URL is resolved
// against the page file's own folder, so a local test page can fetch a
// sibling file.
static std::wstring resolveFetchUrl(const std::wstring& pageUrl, const std::wstring& href) {
    bool hrefIsHttp = href.rfind(L"http://", 0) == 0 || href.rfind(L"https://", 0) == 0;
    bool pageIsHttp = pageUrl.rfind(L"http://", 0) == 0 || pageUrl.rfind(L"https://", 0) == 0;
    if (hrefIsHttp || pageIsHttp) return resolveUrl(pageUrl, href);
    if (href.empty() || href.find(L':') != std::wstring::npos) return L""; // data:, blob:, a drive path, ...
    size_t slash = pageUrl.find_last_of(L"\\/");
    std::wstring dir = slash == std::wstring::npos ? L"" : pageUrl.substr(0, slash + 1);
    std::wstring path = dir + href;
    for (auto& c : path) if (c == L'/') c = L'\\';
    return path;
}

static JSValue js_native_fetch(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    DOMBindingState* state = bindingState(ctx);
    if (!state) return JS_EXCEPTION;

    HttpRequest req;
    req.url = resolveFetchUrl(state->pageUrl, argStr(ctx, argc, argv, 0));
    req.method = wideToUtf8(argStr(ctx, argc, argv, 1));
    if (argc > 2 && JS_IsArray(argv[2])) {
        int64_t len = 0;
        JS_GetLength(ctx, argv[2], &len);
        for (int64_t i = 0; i + 1 < len; i += 2) {
            JSValue name = JS_GetPropertyInt64(ctx, argv[2], i);
            JSValue value = JS_GetPropertyInt64(ctx, argv[2], i + 1);
            const char* n = JS_ToCString(ctx, name);
            const char* v = JS_ToCString(ctx, value);
            if (n && v) req.headers.emplace_back(n, v);
            JS_FreeCString(ctx, n);
            JS_FreeCString(ctx, v);
            JS_FreeValue(ctx, name);
            JS_FreeValue(ctx, value);
        }
    }
    req.body = wideToUtf8(argStr(ctx, argc, argv, 3));

    if (req.url.empty())
        return JS_ThrowTypeError(ctx, "Failed to fetch: unsupported or invalid URL");

    JSValue funcs[2];
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) return promise;

    auto slot = std::make_shared<FetchSlot>();
    state->fetches->pending.push_back({ slot, funcs[0], funcs[1] });
    fetchHttpAsync(std::move(req), [slot](HttpResponse res) {
        // On Fetcher's network thread - only hand the result over.
        std::lock_guard<std::mutex> lock(slot->mutex);
        slot->response = std::move(res);
        slot->done = true;
    });
    return promise;
}

void pollFetches(JSContext* ctx) {
    DOMBindingState* state = bindingState(ctx);
    if (!state || state->fetches->pending.empty()) return;

    // Take the finished ones out first: settling a promise can run JS that
    // starts another fetch(), which appends to `pending`.
    std::vector<PendingFetch> finished;
    auto& pending = state->fetches->pending;
    for (auto it = pending.begin(); it != pending.end();) {
        bool done;
        { std::lock_guard<std::mutex> lock(it->slot->mutex); done = it->slot->done; }
        if (done) { finished.push_back(*it); it = pending.erase(it); }
        else ++it;
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue makeResponse = JS_GetPropertyStr(ctx, global, "__wtResponse");
    for (auto& f : finished) {
        HttpResponse& res = f.slot->response; // done: the worker thread no longer touches it
        JSValue result;
        JSValue settle;
        if (res.ok) {
            JSValue args[4] = {
                JS_NewInt32(ctx, res.status),
                jsStr(ctx, res.finalUrl),
                JS_NewStringLen(ctx, res.body.data(), res.body.size()),
                JS_NewStringLen(ctx, res.contentType.data(), res.contentType.size()),
            };
            result = JS_Call(ctx, makeResponse, JS_UNDEFINED, 4, args);
            for (JSValue& a : args) JS_FreeValue(ctx, a);
            settle = f.resolve;
        } else {
            // Throw-then-catch is the simplest way to get a real TypeError
            // instance (instanceof TypeError), as browsers reject with.
            JS_ThrowTypeError(ctx, "Failed to fetch: %s", wideToUtf8(res.error).c_str());
            result = JS_GetException(ctx);
            settle = f.reject;
        }
        if (!JS_IsException(result)) {
            ArmScriptWatchdog(ctx);
            JSValue r = JS_Call(ctx, settle, JS_UNDEFINED, 1, &result);
            JS_FreeValue(ctx, r);
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));
        }
        JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, f.resolve);
        JS_FreeValue(ctx, f.reject);
        runPendingJobs(ctx); // the .then() chain this just unblocked
    }
    JS_FreeValue(ctx, makeResponse);
    JS_FreeValue(ctx, global);
}

// The parts of the API that are simplest in JS itself: fetch()'s option
// handling, Headers, the Response object, and element.style (a Proxy over
// the style="" attribute - so every write goes through setAttribute, which
// already marks the DOM dirty for a relayout).
static const char kBootstrapJS[] = R"JS(
(function () {
  const g = globalThis;

  class Headers {
    constructor(init) {
      this._m = {};
      if (init instanceof Headers) init = init._m;
      if (Array.isArray(init)) { for (const [k, v] of init) this.set(k, v); }
      else if (init) { for (const k of Object.keys(init)) this.set(k, init[k]); }
    }
    get(n) { const v = this._m[String(n).toLowerCase()]; return v === undefined ? null : v; }
    has(n) { return String(n).toLowerCase() in this._m; }
    set(n, v) { this._m[String(n).toLowerCase()] = String(v); }
    append(n, v) { const k = String(n).toLowerCase(); this._m[k] = k in this._m ? this._m[k] + ', ' + v : String(v); }
    delete(n) { delete this._m[String(n).toLowerCase()]; }
    forEach(cb, self) { for (const k of Object.keys(this._m)) cb.call(self, this._m[k], k, this); }
  }
  g.Headers = Headers;

  g.__wtResponse = function (status, url, body, contentType) {
    let used = false;
    const take = () => {
      if (used) return Promise.reject(new TypeError('Body has already been consumed'));
      used = true;
      return Promise.resolve(body);
    };
    return {
      ok: status >= 200 && status < 300, status, statusText: '', url,
      redirected: false, type: 'basic',
      headers: new Headers(contentType ? { 'content-type': contentType } : {}),
      get bodyUsed() { return used; },
      text: () => take(),
      json: () => take().then(JSON.parse),
      clone: () => g.__wtResponse(status, url, body, contentType),
    };
  };

  g.fetch = function (input, init) {
    try {
      init = init || {};
      const url = String(input && typeof input === 'object' && 'url' in input ? input.url : input);
      const method = String(init.method || 'GET').toUpperCase();
      const flat = [];
      new Headers(init.headers).forEach((v, k) => flat.push(k, v));
      let body = init.body == null ? '' : init.body;
      if (typeof body !== 'string') body = String(body);
      return __wtFetch(url, method, flat, body);
    } catch (e) {
      return Promise.reject(e);
    }
  };

  const nodeProto = Object.getPrototypeOf(document);
  const kebab = p => p === 'cssFloat' ? 'float' : p.replace(/[A-Z]/g, m => '-' + m.toLowerCase());
  const parse = el => {
    const out = [];
    for (const d of (el.getAttribute('style') || '').split(';')) {
      const i = d.indexOf(':');
      if (i < 0) continue;
      const k = d.slice(0, i).trim().toLowerCase();
      if (k) out.push([k, d.slice(i + 1).trim()]);
    }
    return out;
  };
  const read = (el, name) => { const d = parse(el).find(([k]) => k === name); return d ? d[1] : ''; };
  const write = (el, name, value) => {
    const decls = parse(el).filter(([k]) => k !== name);
    value = value == null ? '' : String(value).trim();
    if (value !== '') decls.push([name, value]);
    el.setAttribute('style', decls.map(([k, v]) => k + ': ' + v).join('; '));
  };
  Object.defineProperty(nodeProto, 'style', {
    configurable: true,
    get() {
      const el = this;
      return new Proxy({}, {
        get(_, p) {
          if (typeof p !== 'string') return undefined;
          switch (p) {
            case 'cssText': return el.getAttribute('style') || '';
            case 'length': return parse(el).length;
            case 'getPropertyValue': return n => read(el, String(n).toLowerCase());
            case 'setProperty': return (n, v) => write(el, String(n).toLowerCase(), v);
            case 'removeProperty': return n => { const k = String(n).toLowerCase(); const old = read(el, k); write(el, k, ''); return old; };
          }
          return read(el, kebab(p));
        },
        set(_, p, v) {
          if (p === 'cssText') el.setAttribute('style', String(v));
          else if (typeof p === 'string') write(el, kebab(p), v);
          return true;
        },
      });
    },
  });
})();
)JS";

// ---------------------------------------------------------------------

static const JSCFunctionListEntry js_node_proto_funcs[] = {
    JS_CGETSET_DEF("textContent", js_get_textContent, js_set_textContent),
    JS_CGETSET_DEF("innerHTML", nullptr, js_set_innerHTML),
    JS_CGETSET_DEF("tagName", js_get_tagName, nullptr),
    JS_CGETSET_MAGIC_DEF("id", js_get_attr_magic, js_set_attr_magic, 0),
    JS_CGETSET_MAGIC_DEF("className", js_get_attr_magic, js_set_attr_magic, 1),
    JS_CGETSET_DEF("value", js_get_value, js_set_value),
    JS_CGETSET_DEF("checked", js_get_checked, js_set_checked),
    JS_CGETSET_DEF("title", js_get_title, js_set_title),
    JS_CFUNC_DEF("getAttribute", 1, js_getAttribute),
    JS_CFUNC_DEF("setAttribute", 2, js_setAttribute),
    JS_CFUNC_DEF("hasAttribute", 1, js_hasAttribute),
    JS_CFUNC_DEF("removeAttribute", 1, js_removeAttribute),
    JS_CFUNC_DEF("appendChild", 1, js_appendChild),
    JS_CFUNC_DEF("removeChild", 1, js_removeChild),
    JS_CFUNC_DEF("getElementById", 1, js_getElementById),
    JS_CFUNC_DEF("getElementsByTagName", 1, js_getElementsByTagName),
    JS_CFUNC_DEF("querySelector", 1, js_querySelector),
    JS_CFUNC_DEF("querySelectorAll", 1, js_querySelectorAll),
    JS_CFUNC_DEF("createElement", 1, js_createElement),
    JS_CFUNC_DEF("createTextNode", 1, js_createTextNode),
    JS_CFUNC_DEF("addEventListener", 2, js_addEventListener),
};

static const JSCFunctionListEntry js_global_funcs[] = {
    JS_CFUNC_MAGIC_DEF("setTimeout", 2, js_setTimer, 0),
    JS_CFUNC_MAGIC_DEF("setInterval", 2, js_setTimer, 1),
    JS_CFUNC_DEF("clearTimeout", 1, js_clearTimer),
    JS_CFUNC_DEF("clearInterval", 1, js_clearTimer),
    JS_CGETSET_DEF("location", js_get_location, js_set_location),
    JS_CFUNC_DEF("__wtFetch", 4, js_native_fetch), // the native half of fetch() - see kBootstrapJS
};

void installDOMBindings(JSContext* ctx, Element* documentRoot, DOMBindingState* state) {
    JS_SetContextOpaque(ctx, state);
    state->listeners->ctx = ctx; // so ~ListenerStorage can free stored callbacks
    state->timers->ctx = ctx;    // so ~TimerStorage can free stored callbacks
    state->fetches->ctx = ctx;   // so ~FetchStorage can free pending promise functions
    state->documentEl = documentRoot;

    JSRuntime* rt = JS_GetRuntime(ctx);
    JS_NewClassID(rt, &js_node_class_id);
    JS_NewClass(rt, js_node_class_id, &js_node_class_def);
    JS_NewClassID(rt, &js_event_class_id);
    JS_NewClass(rt, js_event_class_id, &js_event_class_def);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_node_proto_funcs, countof(js_node_proto_funcs));
    JS_SetClassProto(ctx, js_node_class_id, proto);

    JSValue eventProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, eventProto, js_event_proto_funcs, countof(js_event_proto_funcs));
    JS_SetClassProto(ctx, js_event_class_id, eventProto);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "document", wrapNode(ctx, documentRoot));
    JS_SetPropertyFunctionList(ctx, global, js_global_funcs, countof(js_global_funcs));
    // Real browsers make `window` and the global object the same thing -
    // `window.foo` and a bare global `foo` read/write the identical
    // property, and top-level `var`s become properties of both. Aliasing it
    // this way (rather than a separate wrapper object) gets that for free:
    // window.document, window.setTimeout, etc. all already work since
    // they're already global properties, and `window.onload = fn` at least
    // no longer throws (it just isn't fired - documented gap, no "page
    // finished loading" event exists yet).
    JS_SetPropertyStr(ctx, global, "window", JS_DupValue(ctx, global));
    // WTEngine has no <iframe>/frame support, so every page is its own
    // top-level window - self/parent/top all legitimately equal window
    // itself here, same as they would for a real un-framed page. This is
    // what lets a script like `window.parent.location.replace(url)` work -
    // a common redirect-trampoline pattern (DuckDuckGo's own search-result
    // click-through links use exactly this): window.parent resolves to
    // window, whose .location is the accessor set up above.
    JS_SetPropertyStr(ctx, global, "self", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "parent", JS_DupValue(ctx, global));
    JS_SetPropertyStr(ctx, global, "top", JS_DupValue(ctx, global));
    JS_FreeValue(ctx, global);

    // Last, since it builds on `document` and the natives above.
    ArmScriptWatchdog(ctx);
    JSValue boot = JS_Eval(ctx, kBootstrapJS, sizeof(kBootstrapJS) - 1, "<wtengine-bootstrap>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(boot)) {
        JSValue exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        OutputDebugStringA("WTEngine bootstrap JS failed: ");
        OutputDebugStringA(msg ? msg : "?");
        OutputDebugStringA("\n");
        JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
    }
    JS_FreeValue(ctx, boot);
}
