// JSEngine.cpp
#define NOMINMAX
#include "JSEngine.h"
#include "DevConsole.h"
#include "quickjs.h"
#include <windows.h>
#include <algorithm>
#include <vector>

// Every quickjs string is UTF-8; the rest of WTEngine is wstring
// throughout, so this needs its own conversion helpers, the same as
// Fetcher.cpp and main.cpp each keep their own copy of the same pair.
static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

static std::wstring utf8ToWide(const char* s) {
    if (!s || !*s) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0); // includes the terminator
    if (n <= 1) return L"";
    std::wstring out(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    return out;
}

static std::wstring toWide(JSContext* ctx, JSValueConst v) {
    const char* s = JS_ToCString(ctx, v);
    std::wstring out = utf8ToWide(s);
    JS_FreeCString(ctx, s);
    return out;
}

// Per-runtime state reachable from any JSContext* (JS_GetRuntimeOpaque).
struct RuntimeState {
    std::chrono::steady_clock::time_point deadline; // the watchdog's
    // Promises rejected with no handler yet: the tracker below adds one on
    // rejection and removes it again if a handler is attached later, so
    // whatever is left after a microtask checkpoint is truly unhandled.
    struct Rejection { JSValue promise, reason; };
    std::vector<Rejection> rejections;
};

static RuntimeState* runtimeState(JSContext* ctx) {
    return static_cast<RuntimeState*>(JS_GetRuntimeOpaque(JS_GetRuntime(ctx)));
}

// The console as it exists before (or without) the DOM bindings: all
// levels go straight to the log, each argument just string-coerced. Once
// installDOMBindings runs, its bootstrap replaces these with versions that
// format objects readably (see kBootstrapJS in JSBinding.cpp).
static JSValue jsConsole(JSContext* ctx, JSValueConst /*thisVal*/, int argc, JSValueConst* argv, int magic) {
    std::wstring text;
    for (int i = 0; i < argc; i++) text += (i ? L" " : L"") + toWide(ctx, argv[i]);
    consoleLog().add(magic == 2 ? LogLevel::Error : magic == 1 ? LogLevel::Warn : LogLevel::Log, L"console", text);
    return JS_UNDEFINED;
}

// Polled by quickjs-ng from inside the bytecode interpreter every ~10000
// ops (JS_INTERRUPT_COUNTER_INIT in quickjs.c) - frequent enough to catch
// a tight infinite loop well within a couple hundred ms of the deadline.
// Returning nonzero throws an uncatchable "interrupted" error (quickjs's
// own JS_ThrowInterrupted/JS_SetUncatchableError), so a script's own
// try/catch cannot swallow it.
static int watchdogInterruptHandler(JSRuntime* /*rt*/, void* opaque) {
    auto* state = static_cast<RuntimeState*>(opaque);
    return std::chrono::steady_clock::now() >= state->deadline;
}

void ArmScriptWatchdog(JSContext* ctx) {
    runtimeState(ctx)->deadline = std::chrono::steady_clock::now() + kScriptTimeout;
}

// quickjs calls this when a promise is rejected with no handler attached
// (isHandled false), and again if a handler is attached later (true).
static void promiseRejectionTracker(JSContext* ctx, JSValueConst promise, JSValueConst reason,
                                    bool isHandled, void* opaque) {
    auto* state = static_cast<RuntimeState*>(opaque);
    if (!isHandled) {
        state->rejections.push_back({ JS_DupValue(ctx, promise), JS_DupValue(ctx, reason) });
        return;
    }
    auto it = std::find_if(state->rejections.begin(), state->rejections.end(), [&](const RuntimeState::Rejection& r) {
        return JS_VALUE_GET_PTR(r.promise) == JS_VALUE_GET_PTR(promise);
    });
    if (it != state->rejections.end()) {
        JS_FreeValue(ctx, it->promise);
        JS_FreeValue(ctx, it->reason);
        state->rejections.erase(it);
    }
}

JSEngine::JSEngine() {
    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    state_ = new RuntimeState();
    JS_SetRuntimeOpaque(rt, state_);
    JS_SetInterruptHandler(rt, watchdogInterruptHandler, state_);
    JS_SetHostPromiseRejectionTracker(rt, promiseRejectionTracker, state_);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunctionMagic(ctx, jsConsole, "log", 1, JS_CFUNC_generic_magic, 0));
    JS_SetPropertyStr(ctx, console, "warn", JS_NewCFunctionMagic(ctx, jsConsole, "warn", 1, JS_CFUNC_generic_magic, 1));
    JS_SetPropertyStr(ctx, console, "error", JS_NewCFunctionMagic(ctx, jsConsole, "error", 1, JS_CFUNC_generic_magic, 2));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}

JSEngine::~JSEngine() {
    // Rejections still held must be released before the runtime goes -
    // quickjs asserts that no value outlives it.
    for (auto& r : state_->rejections) { JS_FreeValue(ctx, r.promise); JS_FreeValue(ctx, r.reason); }
    if (ctx) JS_FreeContext(ctx);
    if (rt) JS_FreeRuntime(rt);
    delete state_;
}

std::wstring describeException(JSContext* ctx, JSValue exc) {
    if (JS_IsUncatchableError(exc))
        return L"Script stopped: it ran longer than " + std::to_wstring(kScriptTimeout.count()) +
               L" ms without finishing (possible infinite loop)";

    // The bootstrap's __wtInspect formats errors (name, message, stack) and
    // anything else that gets thrown the same way console.log would.
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue inspect = JS_GetPropertyStr(ctx, global, "__wtInspect");
    JS_FreeValue(ctx, global);
    std::wstring out;
    if (JS_IsFunction(ctx, inspect)) {
        JSValue r = JS_Call(ctx, inspect, JS_UNDEFINED, 1, &exc);
        if (JS_IsException(r)) JS_FreeValue(ctx, JS_GetException(ctx));
        else out = toWide(ctx, r);
        JS_FreeValue(ctx, r);
    }
    JS_FreeValue(ctx, inspect);
    if (!out.empty()) return out;

    // No bootstrap (yet): the error's string form plus its stack, which
    // is what names the script and line.
    out = toWide(ctx, exc);
    JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
    if (JS_IsString(stack)) {
        std::wstring s = toWide(ctx, stack);
        if (!s.empty()) out += L"\n" + s;
    }
    JS_FreeValue(ctx, stack);
    return out;
}

void reportException(JSContext* ctx, JSValue exc, const wchar_t* source) {
    consoleLog().add(LogLevel::Error, source, describeException(ctx, exc));
    JS_FreeValue(ctx, exc);
}

void runPendingJobs(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    for (;;) {
        ArmScriptWatchdog(ctx); // fresh budget per drained job, same as any other JS entry point
        JSContext* jobCtx = nullptr;
        int status = JS_ExecutePendingJob(rt, &jobCtx);
        if (status == 0) break; // queue is empty
        if (status < 0) reportException(jobCtx, JS_GetException(jobCtx), L"promise"); // keep draining
    }

    // Whatever is still rejected-and-unhandled now stays that way. Taken
    // out first: formatting a reason runs JS, which could reject more.
    RuntimeState* state = runtimeState(ctx);
    std::vector<RuntimeState::Rejection> unhandled;
    unhandled.swap(state->rejections);
    for (auto& r : unhandled) {
        consoleLog().add(LogLevel::Error, L"promise", L"Uncaught (in promise) " + describeException(ctx, r.reason));
        JS_FreeValue(ctx, r.promise);
        JS_FreeValue(ctx, r.reason);
    }
}

JSEngine::Result JSEngine::eval(const std::wstring& code, const char* filename) {
    std::string src = wideToUtf8(code);
    ArmScriptWatchdog(ctx);
    JSValue result = JS_Eval(ctx, src.c_str(), src.size(), filename, JS_EVAL_TYPE_GLOBAL);

    Result out;
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        out = { false, describeException(ctx, exc) };
        JS_FreeValue(ctx, exc);
    } else {
        out = { true, toWide(ctx, result) };
    }
    JS_FreeValue(ctx, result);
    runPendingJobs(ctx); // promise callbacks the script queued run right after it, before the next script
    return out;
}

void runJSEngineSmokeTest() {
    JSEngine js;
    std::wstring line = L"[JSEngine smoke test] 1 + 2 = " + js.eval(L"1 + 2").text + L"\n";
    wprintf(L"%ls", line.c_str());
    OutputDebugStringW(line.c_str());
}
