// JSBinding.cpp
#define NOMINMAX
#include "JSBinding.h"
#include "quickjs.h"
#include "DOM.h"
#include "CSS.h"
#include "HTMLParser.h"
#include <windows.h>
#include <cwctype>
#include <map>

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
// Engine::runScripts() (replacing the pointer destroys the old target).
struct ListenerStorage {
    std::map<Element*, std::vector<JSValue>> click;
    JSContext* ctx = nullptr;
    ~ListenerStorage() {
        if (!ctx) return;
        for (auto& [el, fns] : click)
            for (JSValue fn : fns) JS_FreeValue(ctx, fn);
    }
};

DOMBindingState::DOMBindingState() : listeners(std::make_unique<ListenerStorage>()) {}
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
            JSValue result = JS_Call(ctx, fn, JS_UNDEFINED, 1, &eventObj);
            if (JS_IsException(result)) JS_FreeValue(ctx, JS_GetException(ctx)); // swallow; keep bubbling
            JS_FreeValue(ctx, result);
        }
    }

    bool prevented = data.prevented;
    JS_FreeValue(ctx, eventObj);
    return prevented;
}

// ---------------------------------------------------------------------

static const JSCFunctionListEntry js_node_proto_funcs[] = {
    JS_CGETSET_DEF("textContent", js_get_textContent, js_set_textContent),
    JS_CGETSET_DEF("innerHTML", nullptr, js_set_innerHTML),
    JS_CGETSET_DEF("tagName", js_get_tagName, nullptr),
    JS_CGETSET_MAGIC_DEF("id", js_get_attr_magic, js_set_attr_magic, 0),
    JS_CGETSET_MAGIC_DEF("className", js_get_attr_magic, js_set_attr_magic, 1),
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

void installDOMBindings(JSContext* ctx, Element* documentRoot, DOMBindingState* state) {
    JS_SetContextOpaque(ctx, state);
    state->listeners->ctx = ctx; // so ~ListenerStorage can free stored callbacks

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
    JS_FreeValue(ctx, global);
}
