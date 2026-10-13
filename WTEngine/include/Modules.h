// Modules.h
#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "ResourceLoader.h"

struct JSContext;

// ES modules: <script type="module">, import/export, import(), import.meta
// and <script type="importmap">.
//
// quickjs asks for an imported module synchronously, while it links the
// importing one - so the whole graph is fetched *before* a module script
// runs: each module that arrives is scanned for the specifiers it imports
// (scanImports), and those are fetched in turn, concurrently, like a
// browser's module map. Engine runs the script once graphReady says all of
// it is here. Anything the scan missed (a computed import() specifier) is
// fetched when quickjs asks for it, waiting for it there.
class ModuleLoader {
public:
    // A new page: forgets the last page's modules and import map.
    void reset(const std::wstring& pageUrl);

    // Makes `ctx`'s runtime resolve and load modules through this loader.
    void install(JSContext* ctx);

    // A <script type="importmap">'s JSON. Its mappings are added to any an
    // earlier import map made (those win). Logs and ignores invalid JSON.
    void addImportMap(JSContext* ctx, const std::wstring& json);

    // The URL `specifier` names when imported by a module at `base`: import
    // map first, then a relative or absolute URL. Empty for a bare name
    // ("react") no import map maps.
    std::wstring resolve(const std::wstring& specifier, const std::wstring& base) const;

    // Starts fetching `url`, unless it's already fetched or on its way.
    // `integrity`: a <script type=module src integrity>'s - the import map's
    // "integrity" entry for the URL otherwise.
    void prefetch(const std::wstring& url, const std::wstring& integrity = L"");

    // Scans modules that arrived since the last call for their imports,
    // and starts fetching those. Called every frame (Engine::pollResources).
    void poll();

    // True once `url` has arrived along with everything it imports
    // (statically - import declarations, not import()).
    bool graphReady(const std::wstring& url);

    // Whether `url`'s fetch failed (for a <script type=module src>'s error
    // event), with the error text. Only meaningful once graphReady(url).
    bool failed(const std::wstring& url, std::wstring* error = nullptr);

    // The resolved URLs an inline module imports - an inline module script
    // is ready once each of these is graphReady.
    std::vector<std::wstring> inlineImports(const std::wstring& code);

    // Runs an inline module script's `code` as a module called `name`
    // (what error stacks show). Errors go to the console.
    void runInline(JSContext* ctx, const std::wstring& code, const std::wstring& name);

    // Runs the module at `url` - an external module script. A module that
    // already ran (another module imported it) doesn't run again.
    void runUrl(JSContext* ctx, const std::wstring& url);

    // For quickjs's callbacks (Modules.cpp).
    char* normalize(JSContext* ctx, const char* base, const char* name);
    struct JSModuleDef* load(JSContext* ctx, const char* name, struct JSValue attributes);
    std::wstring baseOf(const std::string& moduleName) const;

private:
    struct Module {
        size_t fetch = 0;            // index into fetches_
        bool scanned = false;
        std::vector<std::wstring> deps; // statically imported URLs
    };
    Module* find(const std::wstring& url);
    Module& request(const std::wstring& url, const std::wstring& integrity = L"");
    void scan(const std::wstring& url, Module& m);

    std::wstring pageUrl_;
    ResourceLoader fetches_;
    std::unordered_map<std::wstring, Module> modules_;
    std::vector<std::wstring> unscanned_;
    // Module name (its URL, or an inline module's name) -> the URL its
    // relative imports resolve against: the URL after redirects, or the
    // page for an inline module.
    std::unordered_map<std::string, std::wstring> bases_;

    // The import map: specifier (or URL prefix ending in "/") -> URL.
    using SpecifierMap = std::vector<std::pair<std::wstring, std::wstring>>;
    SpecifierMap imports_;
    std::unordered_map<std::wstring, std::wstring> integrity_; // the import map's: URL -> integrity
    std::vector<std::pair<std::wstring, SpecifierMap>> scopes_; // longest prefix first
    static const std::wstring* lookup(const SpecifierMap& map, const std::wstring& key, std::wstring& out);
};

// The module specifiers `source` imports: `statics` from import/export
// declarations, `dynamics` from import("literal") calls. A token scan, not
// a parse - good enough to fetch ahead; ModuleLoader still fetches whatever
// it misses when the module actually asks.
void scanImports(const std::wstring& source, std::vector<std::wstring>& statics, std::vector<std::wstring>& dynamics);
