// JSBinding.cpp
#define NOMINMAX
#include "JSBinding.h"
#include "JSEngine.h"
#include "quickjs.h"
#include "DOM.h"
#include "CSS.h"
#include "DevConsole.h"
#include "HTMLParser.h"
#include "Fetcher.h"
#include "WebStorage.h"
#include "CookieJar.h"

#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <map>
#include <set>
#include <mutex>
#include <deque>
#include <chrono>

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
// Registered event listeners, keyed by target element (nullptr = window),
// in registration order. Owns a JSValue per callback (JS_DupValue'd when
// registered), so it needs the JSContext to free them again - set once by
// installDOMBindings. Freeing happens on removal, or for whatever's left
// in the destructor, which unique_ptr<ListenerStorage>'s own
// move-assignment already invokes correctly for both DOMBindingState's
// full destruction and its per-navigation `= DOMBindingState{}` reset in
// Engine::beginScripts() (replacing the pointer destroys the old target).
//
// An on* handler (el.onclick = fn, or an inline onclick="..." attribute)
// is stored as a listener too, flagged `handler`, so it runs in the order
// it was first set relative to addEventListener ones, as in browsers.
struct Listener {
    std::wstring type;
    JSValue callback;      // a function, or an object with a handleEvent method
    bool capture = false;
    bool once = false;
    bool handler = false;  // an on* handler: a `false` return value cancels the event
    bool removed = false;  // a dispatch already under way skips it; callback already freed
};
struct ListenerStorage {
    std::map<Element*, std::vector<std::shared_ptr<Listener>>> byTarget;
    // (element, type) pairs whose inline on<type>="..." attribute has been
    // compiled already - or overridden by a script setting el.on<type> -
    // so it's never compiled (or a compile error reported) twice.
    std::set<std::pair<Element*, std::wstring>> compiledAttrs;
    JSContext* ctx = nullptr;
    ~ListenerStorage() {
        if (!ctx) return;
        for (auto& [el, list] : byTarget)
            for (auto& l : list) JS_FreeValue(ctx, l->callback);
    }
};

// ---------------------------------------------------------------------
// The one JS wrapper object per node (see JSBinding.h), plus a shared_ptr
// keeping each wrapped node alive for as long as the wrapper might be used.
struct NodeWrappers {
    struct Entry { JSValue obj; std::shared_ptr<Node> keepAlive; };
    std::map<Node*, Entry> byNode;
    JSContext* ctx = nullptr;
    ~NodeWrappers() {
        if (!ctx) return;
        for (auto& [node, e] : byNode) JS_FreeValue(ctx, e.obj);
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
// A requestAnimationFrame callback. Its id is in a separate space from
// timer ids, as in browsers.
struct FrameCallback {
    int id;
    JSValue callback;
};
struct TimerStorage {
    std::vector<Timer> timers;
    int nextId = 1;
    // requestAnimationFrame: callbacks for the next frame, and the batch
    // runAnimationFrames is running now (cancelAnimationFrame can still
    // cancel one of those - its callback becomes undefined).
    std::vector<FrameCallback> frameCallbacks, runningFrame;
    int nextFrameId = 1;
    // Tasks queued from JS (__wtQueueTask - postMessage deliveries), run in
    // order by runQueuedTasks.
    std::deque<JSValue> tasks;
    // The page's clock - for timers and performance.now() alike: seconds
    // since this page's bindings were installed.
    std::chrono::steady_clock::time_point origin = std::chrono::steady_clock::now();
    double seconds() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - origin).count(); }
    JSContext* ctx = nullptr;
    ~TimerStorage() {
        if (!ctx) return;
        for (auto& t : timers) JS_FreeValue(ctx, t.callback);
        for (auto& f : frameCallbacks) JS_FreeValue(ctx, f.callback);
        for (auto& f : runningFrame) JS_FreeValue(ctx, f.callback);
        for (auto& t : tasks) JS_FreeValue(ctx, t);
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
      fetches(std::make_unique<FetchStorage>()), wrappers(std::make_unique<NodeWrappers>()) {}
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
// DOMBindingState::detachedNodes); NodeWrappers keeps them alive, so this
// wrapper itself never frees anything and needs no finalizer.
static JSClassDef js_node_class_def = { "Node", nullptr };

static DOMBindingState* bindingState(JSContext* ctx) {
    return static_cast<DOMBindingState*>(JS_GetContextOpaque(ctx));
}

// The shared_ptr that owns `node`: its parent's child slot, or its
// detachedNodes entry. Empty for the top of the tree, which Document owns
// (and no script can free), and for a text node inside the tree - only an
// element knows its parent, and text nodes are only ever wrapped when
// created by createTextNode, while they're still in detachedNodes.
static std::shared_ptr<Node> owningPtr(DOMBindingState* state, Node* node) {
    if (node->type == Node::ELEMENT) {
        if (Element* p = static_cast<Element*>(node)->parent) {
            for (auto& c : p->children) if (c.get() == node) return c;
        }
    }
    for (auto& d : state->detachedNodes) if (d.get() == node) return d;
    return {};
}

// Returns the node's one wrapper object, creating it on first use.
static JSValue wrapNode(JSContext* ctx, Node* node) {
    if (!node) return JS_NULL;
    DOMBindingState* state = bindingState(ctx);
    if (state) {
        auto it = state->wrappers->byNode.find(node);
        if (it != state->wrappers->byNode.end()) return JS_DupValue(ctx, it->second.obj);
    }
    JSValue obj = JS_NewObjectClass(ctx, js_node_class_id);
    if (JS_IsException(obj)) return obj;
    JS_SetOpaque(obj, node);
    if (state) state->wrappers->byNode[node] = { JS_DupValue(ctx, obj), owningPtr(state, node) };
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

// --- appendChild / insertBefore / replaceChild / removeChild / remove ---
// Inserting a node that's already somewhere - in the page or inside
// another detached subtree - moves it, as in browsers: the same Node
// object changes parent, so every raw pointer to it (layout boxes, focus,
// listeners, its JS wrapper) stays valid.

// Throws a DOMException (defined in kBootstrapJS) with the given name.
static JSValue throwDOMException(JSContext* ctx, const char* name, const char* message) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ctor = JS_GetPropertyStr(ctx, global, "DOMException");
    JSValue args[2] = { JS_NewString(ctx, message), JS_NewString(ctx, name) };
    JSValue exc = JS_CallConstructor(ctx, ctor, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, ctor);
    JS_FreeValue(ctx, global);
    if (JS_IsException(exc)) return exc;
    return JS_Throw(ctx, exc);
}

// The element whose children include `node`, searching from `root` - for
// a text node, which (unlike an element) doesn't know its parent.
static Element* findParentOf(Element* root, Node* node) {
    for (auto& c : root->children) {
        if (c.get() == node) return root;
        if (c->type == Node::ELEMENT)
            if (Element* p = findParentOf(static_cast<Element*>(c.get()), node)) return p;
    }
    return nullptr;
}

static Element* topOf(Element* el);

static Element* parentOf(DOMBindingState* state, Node* node) {
    if (node->type == Node::ELEMENT) return static_cast<Element*>(node)->parent;
    if (Element* p = state->documentEl ? findParentOf(topOf(state->documentEl), node) : nullptr) return p;
    for (auto& d : state->detachedNodes)
        if (d->type == Node::ELEMENT)
            if (Element* p = findParentOf(static_cast<Element*>(d.get()), node)) return p;
    return nullptr;
}

// Takes `node` out of wherever it is now - its parent's children, or
// detachedNodes - and returns the shared_ptr that owned it, for the caller
// to attach elsewhere. Empty if nothing owns it (the top of the tree).
static std::shared_ptr<Node> takeNode(DOMBindingState* state, Node* node) {
    if (Element* parent = parentOf(state, node)) {
        auto& kids = parent->children;
        for (size_t i = 0; i < kids.size(); i++) {
            if (kids[i].get() != node) continue;
            std::shared_ptr<Node> owned = kids[i];
            kids.erase(kids.begin() + i);
            if (node->type == Node::ELEMENT) static_cast<Element*>(node)->parent = nullptr;
            return owned;
        }
    }
    auto& d = state->detachedNodes;
    for (size_t i = 0; i < d.size(); i++) {
        if (d[i].get() != node) continue;
        std::shared_ptr<Node> owned = d[i];
        d.erase(d.begin() + i);
        return owned;
    }
    return {};
}

// Checks that `node` may be inserted under `parent`, throwing if not: a
// node can't go inside itself or its own descendants.
static bool checkInsertable(JSContext* ctx, Element* parent, Node* node) {
    for (Element* a = parent; a; a = a->parent) {
        if (a == node) {
            throwDOMException(ctx, "HierarchyRequestError", "The new child element contains the parent.");
            return false;
        }
    }
    return true;
}

// Inserts `node` into `parent` before `ref` (nullptr = at the end), moving
// it from wherever it was. `ref` must be a child of `parent`.
static JSValue insertNode(JSContext* ctx, Element* parent, JSValueConst nodeVal, Node* ref) {
    DOMBindingState* state = bindingState(ctx);
    Node* node = unwrapNode(nodeVal);
    if (!state || !node)
        return JS_ThrowTypeError(ctx, "Failed to execute 'insertBefore' on 'Node': parameter 1 is not of type 'Node'.");
    if (!checkInsertable(ctx, parent, node)) return JS_EXCEPTION;
    if (ref == node) return JS_DupValue(ctx, nodeVal); // already exactly there

    std::shared_ptr<Node> owned = takeNode(state, node);
    if (!owned) return throwDOMException(ctx, "HierarchyRequestError", "The node can't be moved.");
    auto& kids = parent->children;
    auto pos = kids.end();
    if (ref) pos = std::find_if(kids.begin(), kids.end(), [&](const auto& c) { return c.get() == ref; });
    kids.insert(pos, owned);
    if (node->type == Node::ELEMENT) static_cast<Element*>(node)->parent = parent;
    markDirty(ctx);
    return JS_DupValue(ctx, nodeVal);
}

static bool isChildOf(Element* parent, Node* node) {
    for (auto& c : parent->children) if (c.get() == node) return true;
    return false;
}

static JSValue js_appendChild(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    Element* parent = unwrapElement(this_val);
    if (!parent || argc < 1) return JS_NULL;
    return insertNode(ctx, parent, argv[0], nullptr);
}

static JSValue js_insertBefore(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    Element* parent = unwrapElement(this_val);
    if (!parent || argc < 1) return JS_NULL;
    Node* ref = argc >= 2 ? unwrapNode(argv[1]) : nullptr; // null/undefined: append
    if (ref && !isChildOf(parent, ref))
        return throwDOMException(ctx, "NotFoundError", "The node before which the new node is to be inserted is not a child of this node.");
    return insertNode(ctx, parent, argv[0], ref);
}

// Detaches (not destroys) a node: its shared_ptr moves into detachedNodes,
// so it stays alive - and still usable from JS, matching real removeChild
// returning the removed node - until the page navigates away, even though
// nothing in the visible tree references it anymore.
static void detachNode(JSContext* ctx, DOMBindingState* state, Node* node) {
    if (std::shared_ptr<Node> owned = takeNode(state, node)) {
        state->detachedNodes.push_back(std::move(owned));
        markDirty(ctx);
    }
}

static JSValue js_replaceChild(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    Element* parent = unwrapElement(this_val);
    DOMBindingState* state = bindingState(ctx);
    if (!parent || !state || argc < 2) return JS_NULL;
    Node* old = unwrapNode(argv[1]);
    if (!old || !isChildOf(parent, old))
        return throwDOMException(ctx, "NotFoundError", "The node to be replaced is not a child of this node.");
    if (unwrapNode(argv[0]) == old) return JS_DupValue(ctx, argv[1]);

    JSValue r = insertNode(ctx, parent, argv[0], old);
    if (JS_IsException(r)) return r;
    JS_FreeValue(ctx, r);
    detachNode(ctx, state, old);
    return JS_DupValue(ctx, argv[1]);
}

static JSValue js_removeChild(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    Element* parent = unwrapElement(this_val);
    DOMBindingState* state = bindingState(ctx);
    if (!parent || !state || argc < 1) return JS_NULL;
    Node* child = unwrapNode(argv[0]);
    if (!child || !isChildOf(parent, child))
        return throwDOMException(ctx, "NotFoundError", "The node to be removed is not a child of this node.");
    detachNode(ctx, state, child);
    return JS_DupValue(ctx, argv[0]);
}

// node.remove(): detaches it from its parent; does nothing if it has none.
static JSValue js_remove(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    Node* node = unwrapNode(this_val);
    DOMBindingState* state = bindingState(ctx);
    if (node && state && parentOf(state, node)) detachNode(ctx, state, node);
    return JS_UNDEFINED;
}

static JSValue js_get_parentNode(JSContext* ctx, JSValueConst this_val) {
    Node* node = unwrapNode(this_val);
    DOMBindingState* state = bindingState(ctx);
    return node && state ? wrapNode(ctx, parentOf(state, node)) : JS_NULL;
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

// --- Events -------------------------------------------------------------
// Event objects themselves are plain JS (the Event class family in
// kBootstrapJS), so a page can also make its own - new Event(...), new
// CustomEvent(...) - and send them with dispatchEvent. The dispatch itself
// lives here: walking the target's ancestors through the capture, target
// and bubble phases, calling what ListenerStorage holds for each.

static ListenerStorage* listenersOf(JSContext* ctx) {
    DOMBindingState* s = bindingState(ctx);
    return s ? s->listeners.get() : nullptr;
}

static bool truthyProp(JSContext* ctx, JSValueConst obj, const char* name) {
    JSValue v = JS_GetPropertyStr(ctx, obj, name);
    bool b = JS_ToBool(ctx, v) > 0;
    JS_FreeValue(ctx, v);
    return b;
}

// An event target's JS object: the element's wrapper, or (nullptr) window.
static JSValue wrapTarget(JSContext* ctx, Element* target) {
    return target ? wrapNode(ctx, target) : JS_GetGlobalObject(ctx);
}

// addEventListener's third argument: a boolean (capture), or an options
// object with capture/once (passive and signal are accepted and ignored).
static void parseListenerOptions(JSContext* ctx, int argc, JSValueConst* argv, bool& capture, bool& once) {
    capture = once = false;
    if (argc < 3) return;
    if (JS_IsObject(argv[2])) {
        capture = truthyProp(ctx, argv[2], "capture");
        once = truthyProp(ctx, argv[2], "once");
    }
    else {
        capture = JS_ToBool(ctx, argv[2]) > 0;
    }
}

static void removeListener(JSContext* ctx, ListenerStorage& ls, Element* target, std::shared_ptr<Listener> l) {
    l->removed = true;
    JS_FreeValue(ctx, l->callback);
    l->callback = JS_UNDEFINED;
    auto& list = ls.byTarget[target];
    list.erase(std::remove(list.begin(), list.end(), l), list.end());
}

static JSValue addListener(JSContext* ctx, Element* target, int argc, JSValueConst* argv) {
    ListenerStorage* ls = listenersOf(ctx);
    if (!ls || argc < 2) return JS_UNDEFINED;
    if (!JS_IsFunction(ctx, argv[1]) && !JS_IsObject(argv[1])) return JS_UNDEFINED; // e.g. null: ignored, as in browsers

    auto l = std::make_shared<Listener>();
    l->type = argStr(ctx, argc, argv, 0);
    parseListenerOptions(ctx, argc, argv, l->capture, l->once);
    auto& list = ls->byTarget[target];
    for (auto& e : list) { // adding the same listener twice registers it once
        if (!e->handler && e->type == l->type && e->capture == l->capture && JS_IsStrictEqual(ctx, e->callback, argv[1]))
            return JS_UNDEFINED;
    }
    l->callback = JS_DupValue(ctx, argv[1]);
    list.push_back(std::move(l));
    return JS_UNDEFINED;
}

static JSValue removeListenerCall(JSContext* ctx, Element* target, int argc, JSValueConst* argv) {
    ListenerStorage* ls = listenersOf(ctx);
    if (!ls || argc < 2) return JS_UNDEFINED;
    std::wstring type = argStr(ctx, argc, argv, 0);
    bool capture, once;
    parseListenerOptions(ctx, argc, argv, capture, once);
    auto it = ls->byTarget.find(target);
    if (it == ls->byTarget.end()) return JS_UNDEFINED;
    for (auto& e : it->second) {
        if (!e->handler && e->type == type && e->capture == capture && JS_IsStrictEqual(ctx, e->callback, argv[1])) {
            removeListener(ctx, *ls, target, e); // takes its own reference: `e` is about to be erased
            break;
        }
    }
    return JS_UNDEFINED;
}

static JSValue js_addEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    Element* el = unwrapElement(this_val);
    return el ? addListener(ctx, el, argc, argv) : JS_UNDEFINED;
}

static JSValue js_removeEventListener(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    Element* el = unwrapElement(this_val);
    return el ? removeListenerCall(ctx, el, argc, argv) : JS_UNDEFINED;
}

// window.addEventListener / removeEventListener: `window` is the global
// object, so these are plain global functions with nullptr as the target.
static JSValue js_window_addEventListener(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    return addListener(ctx, nullptr, argc, argv);
}

static JSValue js_window_removeEventListener(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    return removeListenerCall(ctx, nullptr, argc, argv);
}

// --- on* handlers: el.onclick = fn, and inline onclick="..." -------------

static const char* const kHandlerTypes[] = {
    "click", "dblclick", "mousedown", "mouseup", "mousemove", "mouseover", "mouseout",
    "mouseenter", "mouseleave", "keydown", "keyup", "keypress", "input", "change",
    "submit", "reset", "focus", "blur", "load", "error", "scroll", "readystatechange",
};

static std::shared_ptr<Listener> findHandler(ListenerStorage& ls, Element* target, const std::wstring& type) {
    auto it = ls.byTarget.find(target);
    if (it == ls.byTarget.end()) return {};
    for (auto& l : it->second) if (l->handler && l->type == type) return l;
    return {};
}

// Replaces the target's on<type> handler; anything but a function removes it.
static void setHandler(JSContext* ctx, ListenerStorage& ls, Element* target, const std::wstring& type, JSValueConst fn) {
    std::shared_ptr<Listener> existing = findHandler(ls, target, type);
    if (!JS_IsFunction(ctx, fn)) {
        if (existing) removeListener(ctx, ls, target, existing);
        return;
    }
    if (existing) {
        JS_FreeValue(ctx, existing->callback);
        existing->callback = JS_DupValue(ctx, fn);
        return;
    }
    auto l = std::make_shared<Listener>();
    l->type = type;
    l->handler = true;
    l->callback = JS_DupValue(ctx, fn);
    ls.byTarget[target].push_back(std::move(l));
}

static JSValue js_get_handler(JSContext* ctx, JSValueConst this_val, int magic) {
    Element* el = unwrapElement(this_val);
    ListenerStorage* ls = listenersOf(ctx);
    if (!el || !ls) return JS_NULL;
    std::shared_ptr<Listener> h = findHandler(*ls, el, utf8ToWide(kHandlerTypes[magic]));
    return h ? JS_DupValue(ctx, h->callback) : JS_NULL;
}

static JSValue js_set_handler(JSContext* ctx, JSValueConst this_val, JSValueConst val, int magic) {
    Element* el = unwrapElement(this_val);
    ListenerStorage* ls = listenersOf(ctx);
    if (el && ls) {
        std::wstring type = utf8ToWide(kHandlerTypes[magic]);
        ls->compiledAttrs.insert({ el, type }); // a script's handler replaces the attribute's for good
        setHandler(ctx, *ls, el, type, val);
    }
    return JS_UNDEFINED;
}

// Compiles `source`'s inline on<type>="..." attribute into a function
// taking `event`, the first time an event of that type reaches it.
static JSValue compileAttributeHandler(JSContext* ctx, ListenerStorage& ls, Element* source, const std::wstring& type) {
    if (!ls.compiledAttrs.insert({ source, type }).second) return JS_UNDEFINED; // done before (even if it failed)
    auto it = source->attrs.find(L"on" + type);
    if (it == source->attrs.end()) return JS_UNDEFINED;
    std::string code = "(function (event) {\n" + wideToUtf8(it->second) + "\n})";
    std::string name = "<on" + wideToUtf8(type) + " attribute>";
    JSValue fn = JS_Eval(ctx, code.c_str(), code.size(), name.c_str(), JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) {
        reportException(ctx, JS_GetException(ctx), L"inline handler");
        return JS_UNDEFINED;
    }
    return fn;
}

// --- Dispatch -------------------------------------------------------------

// Calls one listener with `event`. An on* handler returning false cancels
// the event. An exception is reported to the console and dispatch goes on
// to the next listener, as in browsers. `fromEngine`: the engine started
// this dispatch, so each listener gets the full script time limit and a
// microtask checkpoint after it - unlike dispatchEvent() called by a script
// that's still running.
static void callListener(JSContext* ctx, JSValueConst callback, JSValueConst thisObj, JSValueConst event,
                         bool isHandler, bool fromEngine, const std::wstring& type) {
    JSValue fn, self;
    if (JS_IsFunction(ctx, callback)) {
        fn = JS_DupValue(ctx, callback);
        self = JS_DupValue(ctx, thisObj);
    }
    else { // an object with handleEvent(), called on that object
        fn = JS_GetPropertyStr(ctx, callback, "handleEvent");
        self = JS_DupValue(ctx, callback);
    }
    if (JS_IsFunction(ctx, fn)) {
        if (fromEngine) ArmScriptWatchdog(ctx);
        JSValue result = JS_Call(ctx, fn, self, 1, const_cast<JSValueConst*>(&event));
        if (JS_IsException(result)) {
            reportException(ctx, JS_GetException(ctx), (type + L" listener").c_str());
        }
        else if (isHandler && JS_IsBool(result) && !JS_ToBool(ctx, result)) {
            JSValue pd = JS_GetPropertyStr(ctx, event, "preventDefault");
            JSValue r = JS_Call(ctx, pd, event, 0, nullptr);
            JS_FreeValue(ctx, r);
            JS_FreeValue(ctx, pd);
        }
        JS_FreeValue(ctx, result);
        if (fromEngine) runPendingJobs(ctx);
    }
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, self);
}

// Runs the listeners on one node of the event's path for one phase (1
// capture, 2 at target, 3 bubble). Returns false once propagation has been
// stopped. The listener list is snapshotted first, so one added during
// dispatch doesn't run this time, and one removed is skipped.
static bool invokeAt(JSContext* ctx, ListenerStorage& ls, Element* node, const std::wstring& type,
                     JSValueConst event, int phase, bool fromEngine) {
    DOMBindingState* state = bindingState(ctx);
    if (node) {
        JSValue fn = compileAttributeHandler(ctx, ls, node, type);
        if (JS_IsFunction(ctx, fn)) setHandler(ctx, ls, node, type, fn);
        JS_FreeValue(ctx, fn);
    }

    JSValue current = wrapTarget(ctx, node);
    JS_SetPropertyStr(ctx, event, "currentTarget", JS_DupValue(ctx, current));
    JS_SetPropertyStr(ctx, event, "eventPhase", JS_NewInt32(ctx, phase));

    std::vector<std::shared_ptr<Listener>> snapshot;
    auto it = ls.byTarget.find(node);
    if (it != ls.byTarget.end()) {
        for (auto& l : it->second) {
            if (l->type != type) continue;
            if ((phase == 1 && !l->capture) || (phase == 3 && l->capture)) continue;
            snapshot.push_back(l);
        }
    }
    for (auto& l : snapshot) {
        if (l->removed) continue;
        JSValue callback = JS_DupValue(ctx, l->callback); // outlives a removal during the call
        bool isHandler = l->handler;
        if (l->once) removeListener(ctx, ls, node, l);
        callListener(ctx, callback, current, event, isHandler, fromEngine, type);
        JS_FreeValue(ctx, callback);
        if (truthyProp(ctx, event, "__wtStopImmediate")) break;
    }

    // window's on* handlers are ordinary properties of the global object
    // (window.onload = fn); <body onload="..."> sets window.onload, as in
    // browsers.
    if (!node && phase != 1 && !truthyProp(ctx, event, "__wtStopImmediate")) {
        std::string prop = "on" + wideToUtf8(type);
        if (type == L"load" && state && state->documentEl) {
            JSValue fn = compileAttributeHandler(ctx, ls, state->documentEl, type);
            JSValue existing = JS_GetPropertyStr(ctx, current, prop.c_str());
            if (JS_IsFunction(ctx, fn) && !JS_IsFunction(ctx, existing))
                JS_SetPropertyStr(ctx, current, prop.c_str(), JS_DupValue(ctx, fn));
            JS_FreeValue(ctx, existing);
            JS_FreeValue(ctx, fn);
        }
        JSValue h = JS_GetPropertyStr(ctx, current, prop.c_str());
        if (JS_IsFunction(ctx, h)) callListener(ctx, h, current, event, true, fromEngine, type);
        JS_FreeValue(ctx, h);
    }

    JS_FreeValue(ctx, current);
    return !truthyProp(ctx, event, "__wtStop");
}

// Sends `event` to `target` (nullptr = window): capture phase from window
// down to the target's parent, then the target itself, then - if the event
// bubbles - back up to window. Returns whether preventDefault() was called.
static bool dispatch(JSContext* ctx, Element* target, JSValueConst event, bool fromEngine) {
    DOMBindingState* state = bindingState(ctx);
    if (!state) return false;
    ListenerStorage& ls = *state->listeners;

    JSValue typeVal = JS_GetPropertyStr(ctx, event, "type");
    const char* typeStr = JS_ToCString(ctx, typeVal);
    std::wstring type = utf8ToWide(typeStr);
    JS_FreeCString(ctx, typeStr);
    JS_FreeValue(ctx, typeVal);

    // The path is fixed before any listener runs, as in browsers. Its
    // elements are held alive until the dispatch ends, even if a listener
    // removes them from the page (innerHTML = ...) along the way.
    std::vector<Element*> path;
    std::vector<std::shared_ptr<Node>> keepAlive;
    for (Element* el = target; el; el = el->parent) {
        path.push_back(el);
        if (auto owner = owningPtr(state, el)) keepAlive.push_back(std::move(owner));
    }
    path.push_back(nullptr); // window
    if (!target) path.resize(1);

    JS_SetPropertyStr(ctx, event, "target", wrapTarget(ctx, target));
    bool bubbles = truthyProp(ctx, event, "bubbles");

    bool go = true;
    for (size_t i = path.size() - 1; go && i > 0; i--) go = invokeAt(ctx, ls, path[i], type, event, 1, fromEngine);
    if (go) go = invokeAt(ctx, ls, path[0], type, event, 2, fromEngine);
    if (bubbles)
        for (size_t i = 1; go && i < path.size(); i++) go = invokeAt(ctx, ls, path[i], type, event, 3, fromEngine);

    JS_SetPropertyStr(ctx, event, "currentTarget", JS_NULL);
    JS_SetPropertyStr(ctx, event, "eventPhase", JS_NewInt32(ctx, 0));
    return truthyProp(ctx, event, "defaultPrevented");
}

// el.dispatchEvent(event) / window.dispatchEvent(event): returns false if
// a listener called preventDefault().
static JSValue dispatchFromScript(JSContext* ctx, Element* target, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "Failed to execute 'dispatchEvent': parameter 1 is not of type 'Event'.");
    return JS_NewBool(ctx, !dispatch(ctx, target, argv[0], false));
}

static JSValue js_dispatchEvent(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    Element* el = unwrapElement(this_val);
    return el ? dispatchFromScript(ctx, el, argc, argv) : JS_TRUE;
}

static JSValue js_window_dispatchEvent(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    return dispatchFromScript(ctx, nullptr, argc, argv);
}

// --- Firing events from the engine (see JSBinding.h) ----------------------

// A trusted `new <ctor>(type, { bubbles, cancelable })`, made by
// kBootstrapJS's __wtMakeEvent. Undefined if that failed (reported).
static JSValue makeEvent(JSContext* ctx, const char* ctor, const wchar_t* type, bool bubbles, bool cancelable) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue make = JS_GetPropertyStr(ctx, global, "__wtMakeEvent");
    JSValue init = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, init, "bubbles", JS_NewBool(ctx, bubbles));
    JS_SetPropertyStr(ctx, init, "cancelable", JS_NewBool(ctx, cancelable));
    JSValue args[3] = { JS_NewString(ctx, ctor), jsStr(ctx, type), init };
    JSValue ev = JS_Call(ctx, make, JS_UNDEFINED, 3, args);
    for (JSValue& a : args) JS_FreeValue(ctx, a);
    JS_FreeValue(ctx, make);
    JS_FreeValue(ctx, global);
    if (JS_IsException(ev)) {
        reportException(ctx, JS_GetException(ctx), L"engine"); // a WTEngine bug, not the page's
        return JS_UNDEFINED;
    }
    return ev;
}

static bool fireAndFree(JSContext* ctx, Element* target, JSValue ev) {
    if (JS_IsUndefined(ev)) return false;
    bool prevented = dispatch(ctx, target, ev, true);
    JS_FreeValue(ctx, ev);
    return prevented;
}

static void setNum(JSContext* ctx, JSValueConst obj, const char* name, double v) {
    JS_SetPropertyStr(ctx, obj, name, JS_NewFloat64(ctx, v));
}

static void setBool(JSContext* ctx, JSValueConst obj, const char* name, bool v) {
    JS_SetPropertyStr(ctx, obj, name, JS_NewBool(ctx, v));
}

bool fireEvent(JSContext* ctx, Element* target, const wchar_t* type, bool bubbles, bool cancelable) {
    return fireAndFree(ctx, target, makeEvent(ctx, "Event", type, bubbles, cancelable));
}

bool fireMouseEvent(JSContext* ctx, Element* target, const wchar_t* type, const MouseInfo& info, Element* relatedTarget) {
    std::wstring t = type;
    bool boundary = t == L"mouseenter" || t == L"mouseleave"; // these neither bubble nor cancel
    JSValue ev = makeEvent(ctx, "MouseEvent", type, !boundary, !boundary);
    if (JS_IsUndefined(ev)) return false;
    setNum(ctx, ev, "clientX", info.clientX);
    setNum(ctx, ev, "clientY", info.clientY);
    setNum(ctx, ev, "screenX", info.clientX);
    setNum(ctx, ev, "screenY", info.clientY);
    setNum(ctx, ev, "pageX", info.pageX);
    setNum(ctx, ev, "pageY", info.pageY);
    setNum(ctx, ev, "button", info.button);
    setNum(ctx, ev, "detail", t == L"click" ? 1 : 0);
    JS_SetPropertyStr(ctx, ev, "relatedTarget", wrapNode(ctx, relatedTarget));
    return fireAndFree(ctx, target, ev);
}

bool fireKeyboardEvent(JSContext* ctx, Element* target, const wchar_t* type, const KeyInfo& info) {
    JSValue ev = makeEvent(ctx, "KeyboardEvent", type, true, true);
    if (JS_IsUndefined(ev)) return false;
    JS_SetPropertyStr(ctx, ev, "key", jsStr(ctx, info.key));
    JS_SetPropertyStr(ctx, ev, "code", jsStr(ctx, info.code));
    setNum(ctx, ev, "keyCode", info.keyCode);
    setNum(ctx, ev, "which", info.keyCode);
    setNum(ctx, ev, "location", info.location);
    setBool(ctx, ev, "ctrlKey", info.ctrl);
    setBool(ctx, ev, "shiftKey", info.shift);
    setBool(ctx, ev, "altKey", info.alt);
    setBool(ctx, ev, "metaKey", info.meta);
    setBool(ctx, ev, "repeat", info.repeat);
    return fireAndFree(ctx, target, ev);
}

void fireInputEvent(JSContext* ctx, Element* target, const wchar_t* inputType, const std::wstring& data) {
    JSValue ev = makeEvent(ctx, "InputEvent", L"input", true, false);
    if (JS_IsUndefined(ev)) return;
    JS_SetPropertyStr(ctx, ev, "inputType", jsStr(ctx, inputType));
    JS_SetPropertyStr(ctx, ev, "data", data.empty() ? JS_NULL : jsStr(ctx, data));
    fireAndFree(ctx, target, ev);
}

bool fireSubmitEvent(JSContext* ctx, Element* form, Element* submitter) {
    JSValue ev = makeEvent(ctx, "SubmitEvent", L"submit", true, true);
    if (JS_IsUndefined(ev)) return false;
    JS_SetPropertyStr(ctx, ev, "submitter", wrapNode(ctx, submitter));
    return fireAndFree(ctx, form, ev);
}

void advanceReadyState(JSContext* ctx, const wchar_t* readyState) {
    DOMBindingState* state = bindingState(ctx);
    if (!state) return;
    state->readyState = readyState;
    fireEvent(ctx, state->documentEl, L"readystatechange", false, false);
}

static JSValue js_get_readyState(JSContext* ctx, JSValueConst this_val) {
    DOMBindingState* state = bindingState(ctx);
    Element* el = unwrapElement(this_val);
    if (!state || !el || el != state->documentEl) return JS_UNDEFINED;
    return jsStr(ctx, state->readyState);
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
    ts.timers.push_back({ id, JS_DupValue(ctx, argv[0]), delaySeconds, ts.seconds() + delaySeconds, magic == 1 });
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

void fireDueTimers(JSContext* ctx) {
    DOMBindingState* state = bindingState(ctx);
    if (!state) return;
    TimerStorage& ts = *state->timers;
    const double nowSeconds = ts.seconds();

    // Snapshot which timers are due first, then re-look-up each by id right
    // before calling it - same iterate-a-copy defense event dispatch uses
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
            reportException(ctx, exc, L"timer"); // then keep firing the rest
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

// --- requestAnimationFrame, queued tasks, performance.now ---------------

// Calls `fn` the way every engine-driven callback is called: a fresh
// watchdog budget, an exception reported to the console under `source`,
// then a microtask checkpoint.
static void callCallback(JSContext* ctx, JSValueConst fn, int argc, JSValueConst* argv, const wchar_t* source) {
    ArmScriptWatchdog(ctx);
    JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, argc, argv);
    if (JS_IsException(result)) reportException(ctx, JS_GetException(ctx), source);
    JS_FreeValue(ctx, result);
    runPendingJobs(ctx);
}

static double performanceNow(const TimerStorage& ts) {
    return ts.seconds() * 1000.0;
}

static JSValue js_requestAnimationFrame(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    DOMBindingState* state = bindingState(ctx);
    if (!state) return JS_NewInt32(ctx, 0);
    if (argc < 1 || !JS_IsFunction(ctx, argv[0]))
        return JS_ThrowTypeError(ctx, "Failed to execute 'requestAnimationFrame' on 'Window': The callback provided as parameter 1 is not a function.");
    TimerStorage& ts = *state->timers;
    int id = ts.nextFrameId++;
    ts.frameCallbacks.push_back({ id, JS_DupValue(ctx, argv[0]) });
    return JS_NewInt32(ctx, id);
}

static JSValue js_cancelAnimationFrame(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    DOMBindingState* state = bindingState(ctx);
    if (!state || argc < 1) return JS_UNDEFINED;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    TimerStorage& ts = *state->timers;
    auto it = std::find_if(ts.frameCallbacks.begin(), ts.frameCallbacks.end(), [&](const FrameCallback& f) { return f.id == id; });
    if (it != ts.frameCallbacks.end()) {
        JS_FreeValue(ctx, it->callback);
        ts.frameCallbacks.erase(it);
    }
    // One still waiting its turn in the batch running now.
    for (auto& f : ts.runningFrame)
        if (f.id == id) { JS_FreeValue(ctx, f.callback); f.callback = JS_UNDEFINED; }
    return JS_UNDEFINED;
}

// __wtQueueTask(fn): runs fn as a task of its own, after the current one
// and its microtasks - what postMessage deliveries go through.
static JSValue js_native_queueTask(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    DOMBindingState* state = bindingState(ctx);
    if (state && argc >= 1 && JS_IsFunction(ctx, argv[0])) state->timers->tasks.push_back(JS_DupValue(ctx, argv[0]));
    return JS_UNDEFINED;
}

// __wtNow(): performance.now() - milliseconds since the page started, with
// sub-millisecond precision.
static JSValue js_native_now(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    DOMBindingState* state = bindingState(ctx);
    return JS_NewFloat64(ctx, state ? performanceNow(*state->timers) : 0.0);
}

// __wtReportError(error, source): reports an error the page didn't catch -
// one thrown by a queueMicrotask callback, say - to the developer console.
static JSValue js_native_reportError(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    std::wstring source = L"script";
    if (argc >= 2) {
        const char* s = JS_ToCString(ctx, argv[1]);
        if (s) { source = std::wstring(s, s + strlen(s)); JS_FreeCString(ctx, s); }
    }
    reportException(ctx, JS_DupValue(ctx, argc >= 1 ? argv[0] : JS_UNDEFINED), source.c_str());
    return JS_UNDEFINED;
}

void runAnimationFrames(JSContext* ctx) {
    DOMBindingState* state = bindingState(ctx);
    if (!state) return;
    TimerStorage& ts = *state->timers;
    if (ts.frameCallbacks.empty()) return;
    // This frame's batch: anything requested while it runs waits for the
    // next frame, as in browsers. Every callback gets the same timestamp.
    ts.runningFrame.swap(ts.frameCallbacks);
    JSValue now = JS_NewFloat64(ctx, performanceNow(ts));
    for (size_t i = 0; i < ts.runningFrame.size(); i++) {
        JSValue fn = ts.runningFrame[i].callback;
        if (JS_IsUndefined(fn)) continue; // cancelled earlier in this batch
        ts.runningFrame[i].callback = JS_UNDEFINED;
        callCallback(ctx, fn, 1, &now, L"animation frame");
        JS_FreeValue(ctx, fn);
    }
    ts.runningFrame.clear();
}

void runQueuedTasks(JSContext* ctx, double budgetSeconds) {
    DOMBindingState* state = bindingState(ctx);
    if (!state) return;
    TimerStorage& ts = *state->timers;
    // Tasks queued while these run (a scheduler posting itself its next
    // slice of work) run in the same frame, until the budget is spent -
    // then the rest wait for the next frame, so the page keeps painting.
    auto start = std::chrono::steady_clock::now();
    while (!ts.tasks.empty()) {
        JSValue fn = ts.tasks.front();
        ts.tasks.pop_front();
        callCallback(ctx, fn, 0, nullptr, L"message");
        JS_FreeValue(ctx, fn);
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= budgetSeconds) break;
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
    std::wstring text, out;
    gatherText(opt, text);
    for (wchar_t c : text) { // whitespace collapsed and trimmed, as browsers do
        if (!iswspace(c)) out.push_back(c);
        else if (!out.empty() && out.back() != L' ') out.push_back(L' ');
    }
    if (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
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

// Resolves a fetch() URL the same way a link is resolved - including, on a
// page loaded off disk, against the page file's own folder, so a local test
// page can fetch a sibling file.
static std::wstring resolveFetchUrl(const std::wstring& pageUrl, const std::wstring& href) {
    return resolveUrl(pageUrl, href);
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
    req.referrer = state->pageUrl;

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
            // The headers as a flat [name, value, ...] array.
            JSValue headers = JS_NewArray(ctx);
            uint32_t h = 0;
            for (const auto& [name, value] : res.headers) {
                JS_SetPropertyUint32(ctx, headers, h++, JS_NewStringLen(ctx, name.data(), name.size()));
                JS_SetPropertyUint32(ctx, headers, h++, JS_NewStringLen(ctx, value.data(), value.size()));
            }
            JSValue args[6] = {
                JS_NewInt32(ctx, res.status),
                jsStr(ctx, res.finalUrl),
                JS_NewStringLen(ctx, res.body.data(), res.body.size()),
                JS_NewStringLen(ctx, res.contentType.data(), res.contentType.size()),
                headers,
                JS_NewStringLen(ctx, res.statusText.data(), res.statusText.size()),
            };
            result = JS_Call(ctx, makeResponse, JS_UNDEFINED, 6, args);
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
            reportException(ctx, JS_GetException(ctx), L"fetch"); // building the Response failed
        }
        JS_FreeValue(ctx, result);
        JS_FreeValue(ctx, f.resolve);
        JS_FreeValue(ctx, f.reject);
        runPendingJobs(ctx); // the .then() chain this just unblocked
    }
    JS_FreeValue(ctx, makeResponse);
    JS_FreeValue(ctx, global);
}

// --- document.body / head / documentElement -----------------------------
// `document` wraps <body> (see installDOMBindings), so on it these return
// <body> itself, the <head> beside it, and the top of the tree; on any
// other element they're undefined.

static bool isDocument(JSContext* ctx, JSValueConst this_val) {
    DOMBindingState* state = bindingState(ctx);
    Element* el = unwrapElement(this_val);
    return state && el && el == state->documentEl;
}

static JSValue js_get_body(JSContext* ctx, JSValueConst this_val) {
    return isDocument(ctx, this_val) ? wrapNode(ctx, bindingState(ctx)->documentEl) : JS_UNDEFINED;
}

static JSValue js_get_documentElement(JSContext* ctx, JSValueConst this_val) {
    return isDocument(ctx, this_val) ? wrapNode(ctx, topOf(bindingState(ctx)->documentEl)) : JS_UNDEFINED;
}

static JSValue js_get_head(JSContext* ctx, JSValueConst this_val) {
    if (!isDocument(ctx, this_val)) return JS_UNDEFINED;
    Element* top = topOf(bindingState(ctx)->documentEl);
    for (auto& c : top->children)
        if (c->type == Node::ELEMENT && static_cast<Element*>(c.get())->tag == L"head")
            return wrapNode(ctx, static_cast<Element*>(c.get()));
    return JS_NULL;
}

// --- localStorage / sessionStorage 
// --- document.cookie ---------------------------------------------------------
// The page's cookies from the same jar requests use (CookieJar), minus
// HttpOnly ones; assigning one stores it as if the page's server had set it
// (but can't create or replace an HttpOnly cookie). Only on `document`, and
// only for an http(s) page - a page from disk has no cookies.

// Splits an http(s) URL into what the jar keys cookies by.
static bool cookieTarget(const std::wstring& url, bool& https, std::wstring& host, std::wstring& path) {
    size_t start;
    if (url.rfind(L"https://", 0) == 0) { https = true; start = 8; }
    else if (url.rfind(L"http://", 0) == 0) { https = false; start = 7; }
    else return false;
    size_t end = url.find_first_of(L"/?#", start);
    host = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    size_t at = host.rfind(L'@');
    if (at != std::wstring::npos) host.erase(0, at + 1);                  // user:password@
    if (!host.empty() && host[0] != L'[') host = host.substr(0, host.find(L':')); // :port
    path = end == std::wstring::npos ? L"/" : url.substr(end);
    return !host.empty();
}

static JSValue js_get_cookie(JSContext* ctx, JSValueConst this_val) {
    if (!isDocument(ctx, this_val)) return JS_UNDEFINED;
    bool https;
    std::wstring host, path;
    if (!cookieTarget(bindingState(ctx)->pageUrl, https, host, path)) return JS_NewString(ctx, "");
    std::string header = CookieJar::instance().cookieHeader(https, host, path, true);
    return JS_NewStringLen(ctx, header.data(), header.size());
}

static JSValue js_set_cookie(JSContext* ctx, JSValueConst this_val, JSValueConst val) {
    if (!isDocument(ctx, this_val)) return JS_UNDEFINED;
    bool https;
    std::wstring host, path;
    if (!cookieTarget(bindingState(ctx)->pageUrl, https, host, path)) return JS_UNDEFINED;
    const char* s = JS_ToCString(ctx, val);
    if (s) CookieJar::instance().setCookie(s, https, host, path, true);
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

// The natives behind the Storage objects kBootstrapJS builds. The first
// argument is the kind: 0 = localStorage, 1 = sessionStorage. The area is
// looked up from the page's origin on every call (see WebStorage.h).

static StorageArea* storageFor(JSContext* ctx, int argc, JSValueConst* argv) {
    DOMBindingState* state = bindingState(ctx);
    if (!state || argc < 1) return nullptr;
    int32_t kind = 0;
    JS_ToInt32(ctx, &kind, argv[0]);
    return &storageArea(storageOrigin(state->pageUrl), kind == 1);
}

static JSValue js_storage_get(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    StorageArea* area = storageFor(ctx, argc, argv);
    const std::wstring* v = area ? area->get(argStr(ctx, argc, argv, 1)) : nullptr;
    return v ? jsStr(ctx, *v) : JS_NULL;
}

// Returns false if the value didn't fit in the quota (the JS side throws).
static JSValue js_storage_set(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    StorageArea* area = storageFor(ctx, argc, argv);
    return JS_NewBool(ctx, area && area->set(argStr(ctx, argc, argv, 1), argStr(ctx, argc, argv, 2)));
}

static JSValue js_storage_remove(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (StorageArea* area = storageFor(ctx, argc, argv)) area->remove(argStr(ctx, argc, argv, 1));
    return JS_UNDEFINED;
}

static JSValue js_storage_clear(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    if (StorageArea* area = storageFor(ctx, argc, argv)) area->clear();
    return JS_UNDEFINED;
}

static JSValue js_storage_length(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    StorageArea* area = storageFor(ctx, argc, argv);
    return JS_NewInt64(ctx, area ? (int64_t)area->length() : 0);
}

// Every key, in the same order key(i) numbers them.
static JSValue js_storage_keys(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    JSValue arr = JS_NewArray(ctx);
    if (StorageArea* area = storageFor(ctx, argc, argv)) {
        uint32_t i = 0;
        for (auto& [k, v] : area->items()) JS_SetPropertyUint32(ctx, arr, i++, jsStr(ctx, k));
    }
    return arr;
}

static JSValue js_storage_key(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    StorageArea* area = storageFor(ctx, argc, argv);
    int64_t i = -1;
    if (argc > 1) JS_ToInt64(ctx, &i, argv[1]);
    if (!area || i < 0 || i >= (int64_t)area->length()) return JS_NULL;
    return jsStr(ctx, std::next(area->items().begin(), (ptrdiff_t)i)->first);
}

// --- The developer console --------------------------------------------
// __wtConsole(level, text) is the native half of console.log/warn/error
// (the bootstrap below formats the arguments; level 0 log, 1 warn, 2 error).

static JSValue js_native_console(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    int32_t level = 0;
    if (argc > 0) JS_ToInt32(ctx, &level, argv[0]);
    consoleLog().add(level == 2 ? LogLevel::Error : level == 1 ? LogLevel::Warn : LogLevel::Log,
                     L"console", argStr(ctx, argc, argv, 1));
    return JS_UNDEFINED;
}

void evaluateInConsole(JSContext* ctx, const std::wstring& code) {
    consoleLog().add(LogLevel::Input, L"input", code);

    std::string src = wideToUtf8(code);
    ArmScriptWatchdog(ctx);
    JSValue result = JS_Eval(ctx, src.c_str(), src.size(), "<console>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        reportException(ctx, JS_GetException(ctx), L"input");
    } else {
        // Formatted as a REPL shows values: strings quoted, objects expanded.
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue inspect = JS_GetPropertyStr(ctx, global, "__wtInspect");
        JSValue text = JS_Call(ctx, inspect, JS_UNDEFINED, 1, &result);
        if (JS_IsException(text)) reportException(ctx, JS_GetException(ctx), L"input");
        else {
            const char* s = JS_ToCString(ctx, text);
            consoleLog().add(LogLevel::Result, L"input", utf8ToWide(s));
            JS_FreeCString(ctx, s);
        }
        JS_FreeValue(ctx, text);
        JS_FreeValue(ctx, inspect);
        JS_FreeValue(ctx, global);
    }
    JS_FreeValue(ctx, result);
    runPendingJobs(ctx);
    markDirty(ctx); // the line may well have changed the page
}

// The parts of the API that are simplest in JS itself: fetch()'s option
// handling, Headers, the Response object, DOMException, the Event classes,
// localStorage/sessionStorage, and element.style (a Proxy over
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

  class DOMException extends Error {
    constructor(message, name) {
      super(message === undefined ? '' : String(message));
      this.name = name === undefined ? 'Error' : String(name);
    }
    get code() { return DOMException.codes[this.name] || 0; }
  }
  DOMException.codes = { IndexSizeError: 1, HierarchyRequestError: 3, WrongDocumentError: 4,
    InvalidCharacterError: 5, NotFoundError: 8, NotSupportedError: 9, InvalidStateError: 11,
    SyntaxError: 12, SecurityError: 18, QuotaExceededError: 22 };
  g.DOMException = DOMException;

  // --- Events: the objects only; dispatch is native (see "Events" above).
  // The engine fills in target/currentTarget/eventPhase as plain
  // properties while dispatching, and reads the hidden stop flags.
  const hidden = (o, k, v) => Object.defineProperty(o, k, { value: v, writable: true, configurable: true, enumerable: false });
  class Event {
    constructor(type, init) {
      if (arguments.length < 1) throw new TypeError("Failed to construct 'Event': 1 argument required, but only 0 present.");
      init = init || {};
      this.type = String(type);
      this.bubbles = !!init.bubbles;
      this.cancelable = !!init.cancelable;
      this.composed = !!init.composed;
      this.defaultPrevented = false;
      this.isTrusted = false;
      this.target = null;
      this.currentTarget = null;
      this.eventPhase = 0;
      this.timeStamp = Date.now();
      hidden(this, '__wtStop', false);
      hidden(this, '__wtStopImmediate', false);
    }
    preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
    stopPropagation() { this.__wtStop = true; }
    stopImmediatePropagation() { this.__wtStop = this.__wtStopImmediate = true; }
    get srcElement() { return this.target; }
    get cancelBubble() { return this.__wtStop; }
    set cancelBubble(v) { if (v) this.__wtStop = true; }
    get returnValue() { return !this.defaultPrevented; }
    set returnValue(v) { if (!v) this.preventDefault(); }
    composedPath() {
      if (!this.target) return [];
      if (this.target === g) return [g];
      const path = [];
      for (let n = this.target; n; n = n.parentNode) path.push(n);
      path.push(g);
      return path;
    }
  }
  [['NONE', 0], ['CAPTURING_PHASE', 1], ['AT_TARGET', 2], ['BUBBLING_PHASE', 3]].forEach(([k, v]) => { Event[k] = v; Event.prototype[k] = v; });
  const modifiers = (e, i) => { e.ctrlKey = !!i.ctrlKey; e.shiftKey = !!i.shiftKey; e.altKey = !!i.altKey; e.metaKey = !!i.metaKey; };
  function getModifierState(k) {
    return { Control: this.ctrlKey, Shift: this.shiftKey, Alt: this.altKey, Meta: this.metaKey }[k] || false;
  }
  class UIEvent extends Event {
    constructor(type, init) {
      super(type, init); init = init || {};
      this.view = init.view === undefined ? null : init.view;
      this.detail = init.detail | 0;
    }
  }
  class MouseEvent extends UIEvent {
    constructor(type, init) {
      super(type, init); init = init || {};
      this.screenX = +init.screenX || 0; this.screenY = +init.screenY || 0;
      this.clientX = +init.clientX || 0; this.clientY = +init.clientY || 0;
      this.pageX = this.clientX; this.pageY = this.clientY;
      this.button = init.button | 0; this.buttons = init.buttons | 0;
      this.relatedTarget = init.relatedTarget || null;
      modifiers(this, init);
    }
    get x() { return this.clientX; }
    get y() { return this.clientY; }
    get which() { return this.button + 1; }
  }
  MouseEvent.prototype.getModifierState = getModifierState;
  class KeyboardEvent extends UIEvent {
    constructor(type, init) {
      super(type, init); init = init || {};
      this.key = init.key === undefined ? '' : String(init.key);
      this.code = init.code === undefined ? '' : String(init.code);
      this.location = init.location | 0;
      this.repeat = !!init.repeat;
      this.isComposing = !!init.isComposing;
      this.keyCode = init.keyCode | 0; this.charCode = init.charCode | 0; this.which = (init.which | 0) || this.keyCode;
      modifiers(this, init);
    }
  }
  KeyboardEvent.prototype.getModifierState = getModifierState;
  class InputEvent extends UIEvent {
    constructor(type, init) {
      super(type, init); init = init || {};
      this.data = init.data === undefined ? null : init.data;
      this.inputType = init.inputType ? String(init.inputType) : '';
      this.isComposing = !!init.isComposing;
    }
  }
  class FocusEvent extends UIEvent {
    constructor(type, init) { super(type, init); this.relatedTarget = (init && init.relatedTarget) || null; }
  }
  class CustomEvent extends Event {
    constructor(type, init) { super(type, init); this.detail = init && init.detail !== undefined ? init.detail : null; }
  }
  class SubmitEvent extends Event {
    constructor(type, init) { super(type, init); this.submitter = (init && init.submitter) || null; }
  }
  const eventClasses = { Event, UIEvent, MouseEvent, KeyboardEvent, InputEvent, FocusEvent, CustomEvent, SubmitEvent };
  Object.assign(g, eventClasses);
  g.__wtMakeEvent = (ctor, type, init) => {
    const e = new eventClasses[ctor](type, init);
    e.isTrusted = true;
    return e;
  };

  // --- Scheduling: performance.now(), queueMicrotask, postMessage and
  // MessageChannel. Tasks go through the native __wtQueueTask, which the
  // engine runs each frame (runQueuedTasks) - not setTimeout, whose
  // one-frame wait is what pages use MessageChannel to avoid.
  const timeOrigin = Date.now();
  g.performance = {
    now: () => __wtNow(),
    timeOrigin,
    toJSON() { return { timeOrigin }; },
  };

  g.queueMicrotask = cb => {
    if (typeof cb !== 'function')
      throw new TypeError("Failed to execute 'queueMicrotask' on 'Window': The callback provided as parameter 1 is not a function.");
    Promise.resolve().then(() => {
      try { cb(); } catch (e) { __wtReportError(e, 'microtask'); }
    });
  };

  class MessageEvent extends Event {
    constructor(type, init) {
      super(type, init);
      init = init || {};
      this.data = init.data === undefined ? null : init.data;
      this.origin = init.origin === undefined ? '' : String(init.origin);
      this.lastEventId = init.lastEventId === undefined ? '' : String(init.lastEventId);
      this.source = init.source || null;
      this.ports = init.ports || [];
    }
  }
  g.MessageEvent = eventClasses.MessageEvent = MessageEvent;

  // A plain event target, for the objects defined here that aren't DOM
  // nodes (MessagePort, XMLHttpRequest) - and the global EventTarget a page
  // can extend. No capture or bubbling: there's only ever the one target.
  // An on<type> handler runs before the listeners.
  class EventTarget {
    constructor() { hidden(this, '_listeners', []); }
    addEventListener(type, fn, options) {
      if (!fn || this._listeners.some(l => l.type === type && l.fn === fn)) return;
      const once = !!(options && typeof options === 'object' && options.once);
      this._listeners.push({ type: String(type), fn, once });
    }
    removeEventListener(type, fn) {
      const i = this._listeners.findIndex(l => l.type === type && l.fn === fn);
      if (i >= 0) this._listeners.splice(i, 1);
    }
    dispatchEvent(event) {
      event.target = event.currentTarget = this;
      event.eventPhase = 2;
      const call = fn => {
        try { typeof fn === 'function' ? fn.call(this, event) : fn.handleEvent(event); }
        catch (e) { __wtReportError(e, event.type + ' listener'); }
      };
      const handler = this['on' + event.type];
      if (typeof handler === 'function') call(handler);
      for (const l of this._listeners.slice()) {
        if (event.__wtStopImmediate) break;
        if (l.type !== event.type || !this._listeners.includes(l)) continue;
        if (l.once) this.removeEventListener(l.type, l.fn);
        call(l.fn);
      }
      event.currentTarget = null;
      event.eventPhase = 0;
      return !event.defaultPrevented;
    }
  }
  g.EventTarget = EventTarget;

  // One end of a MessageChannel. Messages are delivered as tasks, in order,
  // once the port is started - by start(), or by setting onmessage.
  // Simplified: data isn't cloned (the receiver gets the same object) and
  // ports can't be transferred.
  class MessagePort extends EventTarget {
    constructor() {
      super();
      hidden(this, '_other', null);
      hidden(this, '_started', false);
      hidden(this, '_closed', false);
      hidden(this, '_queue', []);
      hidden(this, '_onmessage', null);
      this.onmessageerror = null;
    }
    get onmessage() { return this._onmessage; }
    set onmessage(fn) { this._onmessage = typeof fn === 'function' ? fn : null; this.start(); }
    postMessage(data) {
      const to = this._other;
      if (this._closed || !to || to._closed) return;
      __wtQueueTask(() => to._receive(data));
    }
    start() {
      if (this._started) return;
      this._started = true;
      const queued = this._queue.splice(0);
      for (const data of queued) __wtQueueTask(() => this._deliver(data));
    }
    close() { this._closed = true; }
    _receive(data) {
      if (this._closed) return;
      if (this._started) this._deliver(data);
      else this._queue.push(data);
    }
    _deliver(data) {
      if (!this._closed) this.dispatchEvent(new MessageEvent('message', { data }));
    }
  }
  class MessageChannel {
    constructor() {
      this.port1 = new MessagePort();
      this.port2 = new MessagePort();
      this.port1._other = this.port2;
      this.port2._other = this.port1;
    }
  }
  g.MessagePort = MessagePort;
  g.MessageChannel = MessageChannel;

  // window.postMessage: a 'message' event at window, as a task. Every page
  // is its own top-level window here, so the target origin only has to be
  // valid; the data isn't cloned.
  g.postMessage = (data, targetOrigin) => {
    const m = /^(https?:\/\/[^\/?#]*)/i.exec(String(location.href));
    const origin = m ? m[1].toLowerCase() : 'null'; // a file: page's origin is opaque
    __wtQueueTask(() => g.dispatchEvent(new MessageEvent('message', { data, origin, source: g })));
  };

  // --- localStorage / sessionStorage: a Proxy, so storage.foo and
  // storage['foo'] read and write items as well as getItem/setItem do.
  class Storage { constructor() { throw new TypeError('Illegal constructor'); } }
  g.Storage = Storage;
  const makeStorage = kind => {
    const methods = {
      getItem: k => __wtStorageGet(kind, String(k)),
      setItem: (k, v) => {
        if (!__wtStorageSet(kind, String(k), String(v)))
          throw new DOMException("Failed to execute 'setItem' on 'Storage': Setting the value of '" + k + "' exceeded the quota.", 'QuotaExceededError');
      },
      removeItem: k => { __wtStorageRemove(kind, String(k)); },
      clear: () => { __wtStorageClear(kind); },
      key: i => __wtStorageKey(kind, Math.trunc(Number(i)) || 0),
    };
    return new Proxy(Object.create(Storage.prototype), {
      get(t, p) {
        if (typeof p === 'symbol') return p === Symbol.toStringTag ? 'Storage' : undefined;
        if (p === 'length') return __wtStorageLength(kind);
        if (Object.prototype.hasOwnProperty.call(methods, p)) return methods[p];
        if (p in Object.prototype) return Object.prototype[p];
        const v = methods.getItem(p);
        return v === null ? undefined : v;
      },
      set(t, p, v) { if (typeof p === 'string') methods.setItem(p, v); return true; },
      has(t, p) { return typeof p === 'string' && (p in methods || p === 'length' || __wtStorageGet(kind, p) !== null); },
      deleteProperty(t, p) { if (typeof p === 'string') methods.removeItem(p); return true; },
      ownKeys() { return __wtStorageKeys(kind); },
      getOwnPropertyDescriptor(t, p) {
        if (typeof p !== 'string') return undefined;
        const v = __wtStorageGet(kind, p);
        return v === null ? undefined : { value: v, writable: true, enumerable: true, configurable: true };
      },
    });
  };
  Object.defineProperty(g, 'localStorage', { value: makeStorage(0), enumerable: true, configurable: true });
  Object.defineProperty(g, 'sessionStorage', { value: makeStorage(1), enumerable: true, configurable: true });

  // `rawHeaders`: the response's headers as [name, value, ...] (names
  // lowercased, no Set-Cookie) - empty for a local file.
  g.__wtResponse = function (status, url, body, contentType, rawHeaders, statusText) {
    rawHeaders = rawHeaders || [];
    let used = false;
    const take = () => {
      if (used) return Promise.reject(new TypeError('Body has already been consumed'));
      used = true;
      return Promise.resolve(body);
    };
    const headers = new Headers();
    for (let i = 0; i + 1 < rawHeaders.length; i += 2) headers.append(rawHeaders[i], rawHeaders[i + 1]);
    if (contentType && !headers.has('content-type')) headers.set('content-type', contentType);
    return {
      ok: status >= 200 && status < 300, status, statusText: statusText || '', url,
      redirected: false, type: 'basic', headers,
      get bodyUsed() { return used; },
      text: () => take(),
      json: () => take().then(JSON.parse),
      clone: () => g.__wtResponse(status, url, body, contentType, rawHeaders, statusText),
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

  class ProgressEvent extends Event {
    constructor(type, init) {
      super(type, init);
      init = init || {};
      this.lengthComputable = !!init.lengthComputable;
      this.loaded = Number(init.loaded) || 0;
      this.total = Number(init.total) || 0;
    }
  }
  g.ProgressEvent = ProgressEvent;

  // XMLHttpRequest, on the same native request fetch() uses (__wtFetch) -
  // cookies, redirects and decompression included. Simplified:
  // - asynchronous only: open(..., false) throws, since nothing in this
  //   engine can wait on the network;
  // - the response arrives in one piece, so readyState goes 2, 3, 4 back
  //   to back, with one progress event;
  // - responseType '' / 'text' / 'json'; 'arraybuffer', 'blob' and
  //   'document' give a null response; responseXML is always null;
  // - withCredentials is accepted and ignored (cookies go as for fetch()),
  //   and `upload` never fires anything.
  const xhrForbiddenHeaders = new Set(['accept-charset', 'accept-encoding', 'access-control-request-headers',
    'access-control-request-method', 'connection', 'content-length', 'cookie', 'cookie2', 'date', 'dnt',
    'expect', 'host', 'keep-alive', 'origin', 'referer', 'te', 'trailer', 'transfer-encoding', 'upgrade', 'via']);
  class XMLHttpRequestUpload extends EventTarget {}
  class XMLHttpRequest extends EventTarget {
    constructor() {
      super();
      // gen: bumped by open/abort/timeout, so a response for an earlier
      // request is ignored when it arrives.
      hidden(this, '_x', { method: 'GET', url: '', headers: new Headers(), sent: false, gen: 0,
                           response: null, text: '', timer: null });
      this.readyState = 0;
      this.status = 0;
      this.statusText = '';
      this.responseURL = '';
      this.responseType = '';
      this.timeout = 0;
      this.withCredentials = false;
      this.upload = new XMLHttpRequestUpload();
      for (const t of ['readystatechange', 'loadstart', 'progress', 'abort', 'error', 'load', 'timeout', 'loadend'])
        this['on' + t] = null;
    }
    open(method, url, async) {
      if (arguments.length < 2) throw new TypeError("Failed to execute 'open' on 'XMLHttpRequest': 2 arguments required.");
      if (async === false)
        throw new DOMException("Failed to execute 'open' on 'XMLHttpRequest': synchronous requests are not supported.", 'InvalidAccessError');
      const x = this._x;
      clearTimeout(x.timer);
      x.gen++;
      x.method = String(method).toUpperCase();
      x.url = String(url);
      x.headers = new Headers();
      x.sent = false;
      x.response = null;
      x.text = '';
      this.status = 0;
      this.statusText = '';
      this.responseURL = '';
      this._setState(1);
    }
    setRequestHeader(name, value) {
      if (this.readyState !== 1 || this._x.sent)
        throw new DOMException("Failed to execute 'setRequestHeader' on 'XMLHttpRequest': The object's state must be OPENED.", 'InvalidStateError');
      const n = String(name).toLowerCase();
      if (xhrForbiddenHeaders.has(n) || n.startsWith('proxy-') || n.startsWith('sec-')) return; // as browsers: silently dropped
      this._x.headers.append(name, value);
    }
    send(body) {
      const x = this._x;
      if (this.readyState !== 1 || x.sent)
        throw new DOMException("Failed to execute 'send' on 'XMLHttpRequest': The object's state must be OPENED.", 'InvalidStateError');
      x.sent = true;
      let data = '';
      if (x.method !== 'GET' && x.method !== 'HEAD' && body != null) {
        data = typeof body === 'string' ? body : String(body);
        if (!x.headers.has('content-type')) x.headers.set('content-type', 'text/plain;charset=UTF-8');
      }
      const gen = x.gen;
      this._progress('loadstart', 0, 0);
      const flat = [];
      x.headers.forEach((v, k) => flat.push(k, v));
      let request;
      try { request = __wtFetch(x.url, x.method, flat, data); } catch (e) { request = Promise.reject(e); }
      if (this.timeout > 0) {
        x.timer = setTimeout(() => {
          if (gen !== x.gen) return;
          x.gen++;
          this._fail('timeout');
        }, this.timeout);
      }
      request.then(res => res.text().then(text => {
        if (gen !== x.gen) return; // aborted, timed out or reopened meanwhile
        clearTimeout(x.timer);
        x.response = res;
        x.text = text;
        this.status = res.status;
        this.statusText = res.statusText;
        this.responseURL = res.url;
        this._setState(2);
        this._setState(3);
        this._progress('progress', text.length, text.length);
        this._setState(4);
        this._progress('load', text.length, text.length);
        this._progress('loadend', text.length, text.length);
      }), () => {
        if (gen !== x.gen) return;
        clearTimeout(x.timer);
        this._fail('error');
      });
    }
    abort() {
      const x = this._x;
      clearTimeout(x.timer);
      x.gen++;
      if ((this.readyState === 1 && x.sent) || this.readyState === 2 || this.readyState === 3) {
        x.sent = false;
        this._fail('abort');
      }
      if (this.readyState === 4) { this.readyState = 0; this.status = 0; this.statusText = ''; }
    }
    getResponseHeader(name) {
      if (this.readyState < 2 || !this._x.response) return null;
      return this._x.response.headers.get(name);
    }
    getAllResponseHeaders() {
      if (this.readyState < 2 || !this._x.response) return '';
      const lines = [];
      this._x.response.headers.forEach((v, k) => lines.push(k + ': ' + v));
      return lines.sort().map(l => l + '\r\n').join('');
    }
    overrideMimeType() {}
    get responseText() {
      if (this.responseType !== '' && this.responseType !== 'text')
        throw new DOMException("Failed to read the 'responseText' property from 'XMLHttpRequest': The value is only accessible if the object's 'responseType' is '' or 'text'.", 'InvalidStateError');
      return this.readyState >= 3 ? this._x.text : '';
    }
    get response() {
      const t = this.responseType;
      if (t === '' || t === 'text') return this.readyState >= 3 ? this._x.text : '';
      if (this.readyState !== 4 || !this._x.response) return null;
      if (t === 'json') { try { return JSON.parse(this._x.text); } catch (e) { return null; } }
      return null;
    }
    get responseXML() { return null; }
    _setState(s) {
      this.readyState = s;
      this.dispatchEvent(new Event('readystatechange'));
    }
    _progress(type, loaded, total) {
      this.dispatchEvent(new ProgressEvent(type, { lengthComputable: total > 0, loaded, total }));
    }
    // A request that ends without a response: an error, a timeout or an abort.
    _fail(type) {
      const x = this._x;
      x.response = null;
      x.text = '';
      this.status = 0;
      this.statusText = '';
      this._setState(4);
      this._progress(type, 0, 0);
      this._progress('loadend', 0, 0);
    }
  }
  const xhrStates = { UNSENT: 0, OPENED: 1, HEADERS_RECEIVED: 2, LOADING: 3, DONE: 4 };
  Object.assign(XMLHttpRequest, xhrStates);
  Object.assign(XMLHttpRequest.prototype, xhrStates);
  g.XMLHttpRequest = XMLHttpRequest;
  g.XMLHttpRequestUpload = XMLHttpRequestUpload;

  const nodeProto = Object.getPrototypeOf(document);

  // Formats any value for the developer console, roughly as browsers do:
  // strings quoted, elements as <tag#id.class>, arrays/objects expanded a
  // couple of levels deep, errors as "Name: message" plus their stack
  // (which names the script and line).
  const inspect = (v, depth, seen) => {
    if (v === null) return 'null';
    switch (typeof v) {
      case 'undefined': return 'undefined';
      case 'string': return JSON.stringify(v);
      case 'bigint': return v + 'n';
      case 'symbol': return v.toString();
      case 'function': return 'ƒ ' + (v.name || 'anonymous') + '()';
      case 'number': case 'boolean': return String(v);
    }
    if (v instanceof Error) {
      const head = (v.name || 'Error') + ': ' + v.message;
      const stack = typeof v.stack === 'string' ? v.stack.replace(/\s+$/, '') : '';
      return stack ? head + '\n' + stack : head;
    }
    if (Object.getPrototypeOf(v) === nodeProto) {
      const tag = v.tagName;
      if (!tag) return '#text ' + JSON.stringify(v.textContent);
      const id = v.id ? '#' + v.id : '';
      const cls = v.className ? '.' + v.className.trim().split(/\s+/).join('.') : '';
      return '<' + tag.toLowerCase() + id + cls + '>';
    }
    if (seen.includes(v)) return '[Circular]';
    if (depth >= 3) return Array.isArray(v) ? '[…]' : '{…}';
    seen = seen.concat([v]);
    try {
      if (Array.isArray(v)) {
        const items = v.slice(0, 50).map(x => inspect(x, depth + 1, seen));
        if (v.length > 50) items.push('… ' + (v.length - 50) + ' more');
        return '[' + items.join(', ') + ']';
      }
      if (v instanceof Promise) return 'Promise {…}';
      const keys = Object.keys(v);
      const items = keys.slice(0, 30).map(k => (/^[A-Za-z_$][\w$]*$/.test(k) ? k : JSON.stringify(k)) + ': ' + inspect(v[k], depth + 1, seen));
      if (keys.length > 30) items.push('…');
      const name = v.constructor && v.constructor !== Object && v.constructor.name ? v.constructor.name + ' ' : '';
      return name + '{' + items.join(', ') + '}';
    } catch (e) {
      return String(v);
    }
  };
  g.__wtInspect = v => inspect(v, 0, []);

  // console.*: plain strings print as-is, anything else via inspect.
  const fmt = args => args.map(a => typeof a === 'string' ? a : inspect(a, 0, [])).join(' ');
  for (const [name, level] of [['log', 0], ['info', 0], ['debug', 0], ['warn', 1], ['error', 2]])
    console[name] = (...args) => __wtConsole(level, fmt(args));
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

#define HANDLER_DEF(name, i) JS_CGETSET_MAGIC_DEF(name, js_get_handler, js_set_handler, i)

static const JSCFunctionListEntry js_node_proto_funcs[] = {
    JS_CGETSET_DEF("textContent", js_get_textContent, js_set_textContent),
    JS_CGETSET_DEF("innerHTML", nullptr, js_set_innerHTML),
    JS_CGETSET_DEF("tagName", js_get_tagName, nullptr),
    JS_CGETSET_MAGIC_DEF("id", js_get_attr_magic, js_set_attr_magic, 0),
    JS_CGETSET_MAGIC_DEF("className", js_get_attr_magic, js_set_attr_magic, 1),
    JS_CGETSET_DEF("value", js_get_value, js_set_value),
    JS_CGETSET_DEF("checked", js_get_checked, js_set_checked),
    JS_CGETSET_DEF("title", js_get_title, js_set_title),
    JS_CGETSET_DEF("parentNode", js_get_parentNode, nullptr),
    JS_CGETSET_DEF("parentElement", js_get_parentNode, nullptr),
    JS_CGETSET_DEF("readyState", js_get_readyState, nullptr),
    JS_CGETSET_DEF("cookie", js_get_cookie, js_set_cookie),
    JS_CGETSET_DEF("body", js_get_body, nullptr),
    JS_CGETSET_DEF("head", js_get_head, nullptr),
    JS_CGETSET_DEF("documentElement", js_get_documentElement, nullptr),
    JS_CFUNC_DEF("getAttribute", 1, js_getAttribute),
    JS_CFUNC_DEF("setAttribute", 2, js_setAttribute),
    JS_CFUNC_DEF("hasAttribute", 1, js_hasAttribute),
    JS_CFUNC_DEF("removeAttribute", 1, js_removeAttribute),
    JS_CFUNC_DEF("appendChild", 1, js_appendChild),
    JS_CFUNC_DEF("insertBefore", 2, js_insertBefore),
    JS_CFUNC_DEF("replaceChild", 2, js_replaceChild),
    JS_CFUNC_DEF("removeChild", 1, js_removeChild),
    JS_CFUNC_DEF("remove", 0, js_remove),
    JS_CFUNC_DEF("getElementById", 1, js_getElementById),
    JS_CFUNC_DEF("getElementsByTagName", 1, js_getElementsByTagName),
    JS_CFUNC_DEF("querySelector", 1, js_querySelector),
    JS_CFUNC_DEF("querySelectorAll", 1, js_querySelectorAll),
    JS_CFUNC_DEF("createElement", 1, js_createElement),
    JS_CFUNC_DEF("createTextNode", 1, js_createTextNode),
    JS_CFUNC_DEF("addEventListener", 2, js_addEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, js_removeEventListener),
    JS_CFUNC_DEF("dispatchEvent", 1, js_dispatchEvent),
    // on* handler properties - one per kHandlerTypes entry, same order
    HANDLER_DEF("onclick", 0), HANDLER_DEF("ondblclick", 1), HANDLER_DEF("onmousedown", 2),
    HANDLER_DEF("onmouseup", 3), HANDLER_DEF("onmousemove", 4), HANDLER_DEF("onmouseover", 5),
    HANDLER_DEF("onmouseout", 6), HANDLER_DEF("onmouseenter", 7), HANDLER_DEF("onmouseleave", 8),
    HANDLER_DEF("onkeydown", 9), HANDLER_DEF("onkeyup", 10), HANDLER_DEF("onkeypress", 11),
    HANDLER_DEF("oninput", 12), HANDLER_DEF("onchange", 13), HANDLER_DEF("onsubmit", 14),
    HANDLER_DEF("onreset", 15), HANDLER_DEF("onfocus", 16), HANDLER_DEF("onblur", 17),
    HANDLER_DEF("onload", 18), HANDLER_DEF("onerror", 19), HANDLER_DEF("onscroll", 20),
    HANDLER_DEF("onreadystatechange", 21),
};
static_assert(countof(kHandlerTypes) == 22, "add an on* property to js_node_proto_funcs for each handler type");

static const JSCFunctionListEntry js_global_funcs[] = {
    JS_CFUNC_MAGIC_DEF("setTimeout", 2, js_setTimer, 0),
    JS_CFUNC_MAGIC_DEF("setInterval", 2, js_setTimer, 1),
    JS_CFUNC_DEF("clearTimeout", 1, js_clearTimer),
    JS_CFUNC_DEF("clearInterval", 1, js_clearTimer),
    JS_CFUNC_DEF("requestAnimationFrame", 1, js_requestAnimationFrame),
    JS_CFUNC_DEF("cancelAnimationFrame", 1, js_cancelAnimationFrame),
    JS_CFUNC_DEF("__wtQueueTask", 1, js_native_queueTask), // behind postMessage - see kBootstrapJS
    JS_CFUNC_DEF("__wtNow", 0, js_native_now),             // behind performance.now()
    JS_CFUNC_DEF("__wtReportError", 2, js_native_reportError),
    JS_CGETSET_DEF("location", js_get_location, js_set_location),
    JS_CFUNC_DEF("addEventListener", 2, js_window_addEventListener),
    JS_CFUNC_DEF("removeEventListener", 2, js_window_removeEventListener),
    JS_CFUNC_DEF("dispatchEvent", 1, js_window_dispatchEvent),
    JS_CFUNC_DEF("__wtFetch", 4, js_native_fetch), // the native half of fetch() - see kBootstrapJS
    JS_CFUNC_DEF("__wtConsole", 2, js_native_console), // the native half of console.* - see kBootstrapJS
    // the natives behind localStorage/sessionStorage - see kBootstrapJS
    JS_CFUNC_DEF("__wtStorageGet", 2, js_storage_get),
    JS_CFUNC_DEF("__wtStorageSet", 3, js_storage_set),
    JS_CFUNC_DEF("__wtStorageRemove", 2, js_storage_remove),
    JS_CFUNC_DEF("__wtStorageClear", 1, js_storage_clear),
    JS_CFUNC_DEF("__wtStorageLength", 1, js_storage_length),
    JS_CFUNC_DEF("__wtStorageKeys", 1, js_storage_keys),
    JS_CFUNC_DEF("__wtStorageKey", 2, js_storage_key),
};

void installDOMBindings(JSContext* ctx, Element* documentRoot, DOMBindingState* state) {
    JS_SetContextOpaque(ctx, state);
    state->listeners->ctx = ctx; // so ~ListenerStorage can free stored callbacks
    state->timers->ctx = ctx;    // so ~TimerStorage can free stored callbacks
    state->fetches->ctx = ctx;   // so ~FetchStorage can free pending promise functions
    state->wrappers->ctx = ctx;  // so ~NodeWrappers can free the wrapper objects
    state->documentEl = documentRoot;

    JSRuntime* rt = JS_GetRuntime(ctx);
    JS_NewClassID(rt, &js_node_class_id);
    JS_NewClass(rt, js_node_class_id, &js_node_class_def);

    JSValue proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, js_node_proto_funcs, countof(js_node_proto_funcs));
    JS_SetClassProto(ctx, js_node_class_id, proto);

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "document", wrapNode(ctx, documentRoot));
    JS_SetPropertyFunctionList(ctx, global, js_global_funcs, countof(js_global_funcs));
    // Real browsers make `window` and the global object the same thing -
    // `window.foo` and a bare global `foo` read/write the identical
    // property, and top-level `var`s become properties of both. Aliasing it
    // this way (rather than a separate wrapper object) gets that for free:
    // window.document, window.setTimeout, window.addEventListener, etc. all
    // already work since they're already global properties, and
    // `window.onload = fn` is an ordinary property the "load" dispatch reads.
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
    if (JS_IsException(boot)) reportException(ctx, JS_GetException(ctx), L"engine bootstrap"); // a WTEngine bug, not the page's
    JS_FreeValue(ctx, boot);
}
