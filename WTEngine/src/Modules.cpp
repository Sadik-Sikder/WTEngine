// Modules.cpp
#define NOMINMAX
#include "Modules.h"
#include "JSEngine.h"
#include "DevConsole.h"
#include "quickjs.h"
#include <windows.h>
#include <cwctype>
#include <algorithm>

// quickjs speaks UTF-8; WTEngine wstring (see JSEngine.cpp's copies).
static std::string utf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

static std::wstring wide(const char* s) {
    if (!s || !*s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring out(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    return out;
}

static std::wstring wide(JSContext* ctx, JSValueConst v) {
    const char* s = JS_ToCString(ctx, v);
    std::wstring out = wide(s);
    JS_FreeCString(ctx, s);
    return out;
}

// How many modules a page's imports may start fetching ahead. Static
// imports always are (the script can't run without them); this caps what
// import("...") calls - code a page may never ask for - add on top.
static constexpr size_t kMaxPrefetch = 400;

// How long a module quickjs asks for that wasn't fetched ahead may take.
static constexpr std::chrono::milliseconds kLoadTimeout{ 20000 };

// --- Scanning a module for its imports ---------------------------------

static bool isIdStart(wchar_t c) { return iswalpha(c) || c == L'_' || c == L'$' || c > 0x7F; }
static bool isIdPart(wchar_t c) { return isIdStart(c) || iswdigit(c); }

namespace {
struct ImportScanner {
    const std::wstring& s;
    size_t i = 0;
    std::vector<size_t> templates; // brace depth at each open ${ ... }
    int depth = 0;

    void skipSpace() {
        while (i < s.size()) {
            if (iswspace(s[i])) { i++; continue; }
            if (s[i] == L'/' && i + 1 < s.size() && s[i + 1] == L'/') {
                while (i < s.size() && s[i] != L'\n') i++;
                continue;
            }
            if (s[i] == L'/' && i + 1 < s.size() && s[i + 1] == L'*') {
                size_t end = s.find(L"*/", i + 2);
                i = end == std::wstring::npos ? s.size() : end + 2;
                continue;
            }
            break;
        }
    }
    bool atQuote() const { return i < s.size() && (s[i] == L'"' || s[i] == L'\''); }
    std::wstring string() {
        wchar_t quote = s[i++];
        std::wstring value;
        while (i < s.size() && s[i] != quote && s[i] != L'\n') {
            if (s[i] == L'\\' && i + 1 < s.size()) { value += s[i + 1]; i += 2; continue; }
            value += s[i++];
        }
        i++;
        return value;
    }
    // Inside a template literal: to its end, or into a ${ ... }.
    void templateText() {
        while (i < s.size()) {
            if (s[i] == L'\\') { i += 2; continue; }
            if (s[i] == L'`') { i++; return; }
            if (s[i] == L'$' && i + 1 < s.size() && s[i + 1] == L'{') {
                templates.push_back(depth++);
                i += 2;
                return;
            }
            i++;
        }
    }
    void regex() {
        i++;
        bool inClass = false;
        while (i < s.size() && s[i] != L'\n') {
            wchar_t c = s[i];
            if (c == L'\\') { i += 2; continue; }
            if (c == L'[') inClass = true;
            else if (c == L']') inClass = false;
            else if (c == L'/' && !inClass) { i++; break; }
            i++;
        }
        while (i < s.size() && isIdPart(s[i])) i++; // flags
    }
};
}

// Words after which a "/" starts a regular expression, not a division.
static bool regexMayFollow(const std::wstring& word) {
    static const wchar_t* words[] = { L"return", L"typeof", L"instanceof", L"in", L"of", L"new", L"delete",
                                      L"void", L"throw", L"case", L"do", L"else", L"yield", L"await" };
    for (const wchar_t* w : words) if (word == w) return true;
    return false;
}

void scanImports(const std::wstring& source, std::vector<std::wstring>& statics, std::vector<std::wstring>& dynamics) {
    ImportScanner sc{ source };
    const std::wstring& s = source;
    bool regexAllowed = true;
    wchar_t last = 0; // the last punctuator, or 'a' after a word or value
    while (true) {
        sc.skipSpace();
        if (sc.i >= s.size()) break;
        wchar_t c = s[sc.i];
        if (sc.atQuote()) { sc.string(); regexAllowed = false; last = L'a'; continue; }
        if (c == L'`') { sc.i++; sc.templateText(); regexAllowed = false; last = L'a'; continue; }
        if (c == L'{') { sc.depth++; sc.i++; regexAllowed = true; last = c; continue; }
        if (c == L'}') {
            sc.depth--;
            sc.i++;
            if (!sc.templates.empty() && (size_t)sc.depth == sc.templates.back()) {
                sc.templates.pop_back();
                sc.templateText(); // the rest of the template
                regexAllowed = false;
                last = L'a';
            } else {
                regexAllowed = true;
                last = c;
            }
            continue;
        }
        if (c == L'/') {
            if (regexAllowed) { sc.regex(); regexAllowed = false; last = L'a'; }
            else { sc.i++; regexAllowed = true; last = c; }
            continue;
        }
        if (isIdStart(c)) {
            size_t start = sc.i;
            while (sc.i < s.size() && isIdPart(s[sc.i])) sc.i++;
            std::wstring word = s.substr(start, sc.i - start);
            bool member = last == L'.'; // obj.import, Array.from
            last = L'a';
            regexAllowed = regexMayFollow(word);
            if (member) continue;
            if (word == L"import") {
                sc.skipSpace();
                if (sc.atQuote()) { statics.push_back(sc.string()); continue; } // import "x"
                if (sc.i < s.size() && s[sc.i] == L'(') {                        // import("x")
                    size_t open = sc.i++;
                    sc.skipSpace();
                    if (sc.atQuote()) {
                        std::wstring spec = sc.string();
                        sc.skipSpace();
                        if (sc.i < s.size() && (s[sc.i] == L')' || s[sc.i] == L',')) dynamics.push_back(spec);
                    } else {
                        sc.i = open; // import(expression): scan on through it
                    }
                }
            } else if (word == L"from") {                                        // ... from "x"
                sc.skipSpace();
                if (sc.atQuote()) statics.push_back(sc.string());
            }
            continue;
        }
        if (iswdigit(c)) {
            while (sc.i < s.size() && (isIdPart(s[sc.i]) || s[sc.i] == L'.')) sc.i++;
            regexAllowed = false;
            last = L'a';
            continue;
        }
        sc.i++;
        regexAllowed = c != L')' && c != L']';
        last = c;
    }
}

// --- quickjs's hooks -----------------------------------------------------

// The loader each runtime was installed with (for import.meta.resolve,
// which quickjs doesn't hand an opaque pointer).
static std::unordered_map<JSRuntime*, ModuleLoader*> g_loaders;

static char* normalizeHook(JSContext* ctx, const char* base, const char* name, void* opaque) {
    return static_cast<ModuleLoader*>(opaque)->normalize(ctx, base, name);
}

static JSModuleDef* loadHook(JSContext* ctx, const char* name, void* opaque, JSValueConst attributes) {
    return static_cast<ModuleLoader*>(opaque)->load(ctx, name, attributes);
}

// import ... with { type: "json" } is the only import attribute there is
// support for (no CSS module scripts).
static int checkAttributesHook(JSContext* ctx, void*, JSValueConst attributes) {
    JSValue type = JS_GetPropertyStr(ctx, attributes, "type");
    std::wstring t = JS_IsUndefined(type) ? L"" : wide(ctx, type);
    JS_FreeValue(ctx, type);
    if (t.empty() || t == L"json") return 0;
    JS_ThrowTypeError(ctx, "Import attribute type \"%s\" is not supported", utf8(t).c_str());
    return -1;
}

static JSValue metaResolve(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv, int, JSValueConst* data) {
    auto it = g_loaders.find(JS_GetRuntime(ctx));
    std::wstring spec = argc > 0 ? wide(ctx, argv[0]) : L"undefined";
    std::wstring url = it == g_loaders.end() ? L"" : it->second->resolve(spec, wide(ctx, data[0]));
    if (url.empty())
        return JS_ThrowTypeError(ctx, "Failed to resolve module specifier \"%s\"", utf8(spec).c_str());
    return JS_NewString(ctx, utf8(url).c_str());
}

// import.meta: the module's URL, and resolve() relative to it.
static void setImportMeta(JSContext* ctx, JSModuleDef* m, const std::wstring& url) {
    JSValue meta = JS_GetImportMeta(ctx, m);
    if (JS_IsException(meta)) { JS_FreeValue(ctx, JS_GetException(ctx)); return; }
    std::string u = utf8(url);
    JSValue data = JS_NewString(ctx, u.c_str());
    JS_DefinePropertyValueStr(ctx, meta, "url", JS_NewString(ctx, u.c_str()), JS_PROP_C_W_E);
    JS_DefinePropertyValueStr(ctx, meta, "resolve", JS_NewCFunctionData(ctx, metaResolve, 1, 0, 1, &data), JS_PROP_C_W_E);
    JS_FreeValue(ctx, data);
    JS_FreeValue(ctx, meta);
}

// A JSON module's text as a JS string literal (for JSON.parse - the text
// must never run as code).
static std::string jsStringLiteral(const std::string& text) {
    static const char* hex = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 0x20) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += (char)c;
    }
    return out + "\"";
}

static JSValue reportModuleError(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    consoleLog().add(LogLevel::Error, L"script", describeException(ctx, argc > 0 ? argv[0] : JS_UNDEFINED));
    return JS_UNDEFINED;
}

// A module script's evaluation promise: a rejection is the script's error.
static void reportRejection(JSContext* ctx, JSValue promise) {
    JSValue then = JS_GetPropertyStr(ctx, promise, "then");
    JSValue args[2] = { JS_UNDEFINED, JS_NewCFunction(ctx, reportModuleError, "", 1) };
    JS_FreeValue(ctx, JS_Call(ctx, then, promise, 2, args));
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, then);
    JS_FreeValue(ctx, promise);
}

// --- ModuleLoader --------------------------------------------------------

void ModuleLoader::reset(const std::wstring& pageUrl) {
    pageUrl_ = pageUrl;
    fetches_.start({});
    modules_.clear();
    unscanned_.clear();
    bases_.clear();
    imports_.clear();
    integrity_.clear();
    scopes_.clear();
}

void ModuleLoader::install(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    JS_SetModuleLoaderFunc2(rt, normalizeHook, loadHook, checkAttributesHook, this);
    g_loaders[rt] = this;
}

static bool urlLike(const std::wstring& s) {
    return s.rfind(L"/", 0) == 0 || s.rfind(L"./", 0) == 0 || s.rfind(L"../", 0) == 0;
}

static bool absoluteUrl(const std::wstring& s) {
    return s.rfind(L"http://", 0) == 0 || s.rfind(L"https://", 0) == 0;
}

// The URL a specifier is as a URL (relative to `base`), or empty if it's a
// bare name.
static std::wstring asUrl(const std::wstring& spec, const std::wstring& base) {
    if (absoluteUrl(spec)) return spec;
    if (urlLike(spec)) return resolveUrl(base, spec);
    return L"";
}

void ModuleLoader::addImportMap(JSContext* ctx, const std::wstring& json) {
    std::string text = utf8(json);
    JSValue map = JS_ParseJSON(ctx, text.c_str(), text.size(), "<importmap>");
    if (JS_IsException(map)) {
        consoleLog().add(LogLevel::Error, L"script", L"Failed to parse import map: " +
                         describeException(ctx, JS_GetException(ctx)));
        return;
    }
    // Each "specifier": "url" of an object, keys normalized like specifiers.
    auto readMap = [&](JSValueConst obj, SpecifierMap& into) {
        JSPropertyEnum* props = nullptr;
        uint32_t count = 0;
        if (!JS_IsObject(obj) || JS_GetOwnPropertyNames(ctx, &props, &count, obj, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0)
            return;
        for (uint32_t k = 0; k < count; k++) {
            const char* keyC = JS_AtomToCString(ctx, props[k].atom);
            std::wstring key = wide(keyC);
            JS_FreeCString(ctx, keyC);
            JSValue v = JS_GetProperty(ctx, obj, props[k].atom);
            std::wstring value = JS_IsString(v) ? wide(ctx, v) : L"";
            JS_FreeValue(ctx, v);
            std::wstring keyUrl = asUrl(key, pageUrl_);
            if (!keyUrl.empty()) key = keyUrl;
            std::wstring target = asUrl(value, pageUrl_);
            if (key.empty() || target.empty()) continue;
            if (key.back() == L'/' && target.back() != L'/') continue; // a prefix must map to a prefix
            bool known = false;
            for (auto& e : into) known = known || e.first == key;
            if (!known) into.emplace_back(key, target); // an earlier map's entry wins
        }
        JS_FreePropertyEnum(ctx, props, count);
    };
    JSValue imports = JS_GetPropertyStr(ctx, map, "imports");
    readMap(imports, imports_);
    JS_FreeValue(ctx, imports);

    // "integrity": { "<url>": "sha384-..." } - what a module fetched from
    // that URL must hash to (an earlier map's entry wins).
    JSValue integrity = JS_GetPropertyStr(ctx, map, "integrity");
    JSPropertyEnum* entries = nullptr;
    uint32_t entryCount = 0;
    if (JS_IsObject(integrity) &&
        JS_GetOwnPropertyNames(ctx, &entries, &entryCount, integrity, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) >= 0) {
        for (uint32_t k = 0; k < entryCount; k++) {
            const char* keyC = JS_AtomToCString(ctx, entries[k].atom);
            std::wstring url = asUrl(wide(keyC), pageUrl_);
            JS_FreeCString(ctx, keyC);
            JSValue v = JS_GetProperty(ctx, integrity, entries[k].atom);
            if (!url.empty() && JS_IsString(v)) integrity_.emplace(url, wide(ctx, v));
            JS_FreeValue(ctx, v);
        }
        JS_FreePropertyEnum(ctx, entries, entryCount);
    }
    JS_FreeValue(ctx, integrity);

    JSValue scopes = JS_GetPropertyStr(ctx, map, "scopes");
    JSPropertyEnum* props = nullptr;
    uint32_t count = 0;
    if (JS_IsObject(scopes) && JS_GetOwnPropertyNames(ctx, &props, &count, scopes, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) >= 0) {
        for (uint32_t k = 0; k < count; k++) {
            const char* keyC = JS_AtomToCString(ctx, props[k].atom);
            std::wstring prefix = resolveUrl(pageUrl_, wide(keyC));
            JS_FreeCString(ctx, keyC);
            if (prefix.empty()) continue;
            JSValue v = JS_GetProperty(ctx, scopes, props[k].atom);
            auto it = std::find_if(scopes_.begin(), scopes_.end(), [&](auto& s) { return s.first == prefix; });
            if (it == scopes_.end()) { scopes_.emplace_back(prefix, SpecifierMap{}); it = scopes_.end() - 1; }
            readMap(v, it->second);
            JS_FreeValue(ctx, v);
        }
        JS_FreePropertyEnum(ctx, props, count);
        std::stable_sort(scopes_.begin(), scopes_.end(), [](auto& a, auto& b) { return a.first.size() > b.first.size(); });
    }
    JS_FreeValue(ctx, scopes);
    JS_FreeValue(ctx, map);
}

const std::wstring* ModuleLoader::lookup(const SpecifierMap& map, const std::wstring& key, std::wstring& out) {
    const std::pair<std::wstring, std::wstring>* best = nullptr;
    for (auto& e : map) {
        if (e.first == key) { out = e.second; return &out; }
        if (e.first.back() == L'/' && key.rfind(e.first, 0) == 0 && (!best || e.first.size() > best->first.size()))
            best = &e;
    }
    if (!best) return nullptr;
    out = best->second + key.substr(best->first.size());
    return &out;
}

std::wstring ModuleLoader::resolve(const std::wstring& specifier, const std::wstring& base) const {
    std::wstring url = asUrl(specifier, base);
    const std::wstring& key = url.empty() ? specifier : url;
    std::wstring mapped;
    for (auto& [prefix, map] : scopes_) {
        bool inScope = prefix.back() == L'/' ? base.rfind(prefix, 0) == 0 : base == prefix;
        if (inScope && lookup(map, key, mapped)) return mapped;
    }
    if (lookup(imports_, key, mapped)) return mapped;
    return url;
}

std::wstring ModuleLoader::baseOf(const std::string& moduleName) const {
    auto it = bases_.find(moduleName);
    if (it != bases_.end()) return it->second;
    // A classic script's URL, or something with no URL ("<inline script
    // 2>", an event handler): the page.
    std::wstring name = wide(moduleName.c_str());
    return resolveUrl(name, L"x").empty() ? pageUrl_ : name;
}

ModuleLoader::Module* ModuleLoader::find(const std::wstring& url) {
    auto it = modules_.find(url);
    return it == modules_.end() ? nullptr : &it->second;
}

ModuleLoader::Module& ModuleLoader::request(const std::wstring& url, const std::wstring& integrity) {
    if (Module* m = find(url)) return *m;
    Module& m = modules_[url];
    auto mapped = integrity_.find(url);
    m.fetch = fetches_.add(url, FetchDest::Script, pageUrl_,
                           !integrity.empty() || mapped == integrity_.end() ? integrity : mapped->second);
    unscanned_.push_back(url);
    return m;
}

void ModuleLoader::prefetch(const std::wstring& url, const std::wstring& integrity) {
    if (!find(url) && modules_.size() < kMaxPrefetch) request(url, integrity);
}

void ModuleLoader::scan(const std::wstring& url, Module& m) {
    m.scanned = true;
    const FetchResult& res = fetches_.result(m.fetch);
    if (!res.ok) return;
    std::vector<std::wstring> statics, dynamics;
    scanImports(res.html, statics, dynamics);
    const std::wstring& base = res.finalUrl.empty() ? url : res.finalUrl;
    for (auto& spec : statics) {
        std::wstring dep = resolve(spec, base);
        if (dep.empty()) continue; // an error when it runs
        m.deps.push_back(dep);
        request(dep);
    }
    for (auto& spec : dynamics) {
        std::wstring dep = resolve(spec, base);
        if (!dep.empty()) prefetch(dep);
    }
}

void ModuleLoader::poll() {
    std::vector<std::wstring> waiting;
    waiting.swap(unscanned_);
    for (auto& url : waiting) {
        Module* m = find(url);
        if (!m || m->scanned) continue;
        if (fetches_.ready(m->fetch)) scan(url, *m);
        else unscanned_.push_back(url);
    }
}

bool ModuleLoader::graphReady(const std::wstring& url) {
    std::vector<std::wstring> stack{ url };
    std::unordered_set<std::wstring> seen{ url };
    while (!stack.empty()) {
        std::wstring u = std::move(stack.back());
        stack.pop_back();
        Module& m = request(u);
        if (!fetches_.ready(m.fetch)) return false;
        if (!m.scanned) scan(u, m);
        for (auto& dep : m.deps)
            if (seen.insert(dep).second) stack.push_back(dep);
    }
    return true;
}

bool ModuleLoader::failed(const std::wstring& url, std::wstring* error) {
    Module* m = find(url);
    if (!m || !fetches_.ready(m->fetch)) return false;
    const FetchResult& res = fetches_.result(m->fetch);
    if (res.ok) return false;
    if (error) *error = res.error;
    return true;
}

std::vector<std::wstring> ModuleLoader::inlineImports(const std::wstring& code) {
    std::vector<std::wstring> statics, dynamics, urls;
    scanImports(code, statics, dynamics);
    for (auto& spec : statics) {
        std::wstring url = resolve(spec, pageUrl_);
        if (url.empty()) continue;
        request(url);
        urls.push_back(url);
    }
    for (auto& spec : dynamics) {
        std::wstring url = resolve(spec, pageUrl_);
        if (!url.empty()) prefetch(url);
    }
    return urls;
}

char* ModuleLoader::normalize(JSContext* ctx, const char* base, const char* name) {
    std::wstring url = resolve(wide(name), baseOf(base ? base : ""));
    if (url.empty()) {
        JS_ThrowTypeError(ctx, "Failed to resolve module specifier \"%s\". Relative references must start "
                               "with either \"/\", \"./\", or \"../\".", name);
        return nullptr;
    }
    return js_strdup(ctx, utf8(url).c_str());
}

JSModuleDef* ModuleLoader::load(JSContext* ctx, const char* name, JSValueConst attributes) {
    std::wstring url = wide(name);
    Module& m = request(url);
    if (!fetches_.ready(m.fetch)) {
        // Not fetched ahead (a computed import() specifier): the script
        // waits for it here. That time isn't the script's own.
        auto start = std::chrono::steady_clock::now();
        bool arrived = fetches_.wait(m.fetch, kLoadTimeout);
        ExtendScriptWatchdog(ctx, std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start));
        if (!arrived) {
            JS_ThrowTypeError(ctx, "Failed to fetch module %s: timed out", name);
            return nullptr;
        }
    }
    const FetchResult& res = fetches_.result(m.fetch);
    if (!res.ok) {
        JS_ThrowTypeError(ctx, "Failed to fetch module %s: %s", name, utf8(res.error).c_str());
        return nullptr;
    }

    JSValue type = JS_IsObject(attributes) ? JS_GetPropertyStr(ctx, attributes, "type") : JS_UNDEFINED;
    bool json = JS_IsString(type) && wide(ctx, type) == L"json";
    JS_FreeValue(ctx, type);
    std::string src = utf8(res.html);
    if (json) src = "export default JSON.parse(" + jsStringLiteral(src) + ");";

    JSValue compiled = JS_Eval(ctx, src.c_str(), src.size(), name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (JS_IsException(compiled)) return nullptr;
    auto* def = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
    std::wstring base = res.finalUrl.empty() ? url : res.finalUrl;
    bases_[name] = base;
    setImportMeta(ctx, def, base);
    JS_FreeValue(ctx, compiled);
    return def;
}

void ModuleLoader::runInline(JSContext* ctx, const std::wstring& code, const std::wstring& name) {
    std::string n = utf8(name), src = utf8(code);
    bases_[n] = pageUrl_;
    ArmScriptWatchdog(ctx, kPageScriptTimeout);
    JSValue compiled = JS_Eval(ctx, src.c_str(), src.size(), n.c_str(), JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    if (!JS_IsException(compiled)) {
        setImportMeta(ctx, static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled)), pageUrl_);
        if (JS_ResolveModule(ctx, compiled) < 0) {
            JS_FreeValue(ctx, compiled);
        } else {
            JSValue promise = JS_EvalFunction(ctx, compiled); // takes `compiled`
            if (!JS_IsException(promise)) reportRejection(ctx, promise);
        }
    }
    if (JS_HasException(ctx)) reportException(ctx, JS_GetException(ctx), L"script");
    runPendingJobs(ctx);
}

void ModuleLoader::runUrl(JSContext* ctx, const std::wstring& url) {
    ArmScriptWatchdog(ctx, kPageScriptTimeout);
    JSValue promise = JS_LoadModule(ctx, utf8(pageUrl_).c_str(), utf8(url).c_str());
    if (JS_IsException(promise)) reportException(ctx, JS_GetException(ctx), L"script");
    else reportRejection(ctx, promise);
    runPendingJobs(ctx);
}
