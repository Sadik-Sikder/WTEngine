// JSEngine.cpp
#define NOMINMAX
#include "JSEngine.h"
#include "quickjs.h"
#include <windows.h>

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

// console.log/warn/error all just print - no separate log levels yet.
// Bound directly (not via eval) since page scripts expect it as a global,
// same as a real browser's devtools console.
static JSValue jsConsoleLog(JSContext* ctx, JSValueConst /*thisVal*/, int argc, JSValueConst* argv) {
    std::wstring line = L"[console]";
    for (int i = 0; i < argc; i++) {
        const char* s = JS_ToCString(ctx, argv[i]);
        line += L" " + utf8ToWide(s);
        JS_FreeCString(ctx, s);
    }
    line += L"\n";
    wprintf(L"%ls", line.c_str());
    OutputDebugStringW(line.c_str());
    return JS_UNDEFINED;
}

// Polled by quickjs-ng from inside the bytecode interpreter every ~10000
// ops (JS_INTERRUPT_COUNTER_INIT in quickjs.c) - frequent enough to catch
// a tight infinite loop well within a couple hundred ms of the deadline.
// Returning nonzero throws an uncatchable "interrupted" error (quickjs's
// own JS_ThrowInterrupted/JS_SetUncatchableError), so a script's own
// try/catch cannot swallow it.
static int watchdogInterruptHandler(JSRuntime* /*rt*/, void* opaque) {
    auto* state = static_cast<JSWatchdogState*>(opaque);
    return std::chrono::steady_clock::now() >= state->deadline;
}

void ArmScriptWatchdog(JSContext* ctx) {
    auto* state = static_cast<JSWatchdogState*>(JS_GetRuntimeOpaque(JS_GetRuntime(ctx)));
    state->deadline = std::chrono::steady_clock::now() + kScriptTimeout;
}

JSEngine::JSEngine() {
    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);
    JS_SetRuntimeOpaque(rt, &watchdog_);
    JS_SetInterruptHandler(rt, watchdogInterruptHandler, &watchdog_);

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, console, "log", JS_NewCFunction(ctx, jsConsoleLog, "log", 1));
    JS_SetPropertyStr(ctx, console, "warn", JS_NewCFunction(ctx, jsConsoleLog, "warn", 1));
    JS_SetPropertyStr(ctx, console, "error", JS_NewCFunction(ctx, jsConsoleLog, "error", 1));
    JS_SetPropertyStr(ctx, global, "console", console);
    JS_FreeValue(ctx, global);
}

JSEngine::~JSEngine() {
    if (ctx) JS_FreeContext(ctx);
    if (rt) JS_FreeRuntime(rt);
}

void runPendingJobs(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);
    for (;;) {
        ArmScriptWatchdog(ctx); // fresh budget per drained job, same as any other JS entry point
        JSContext* jobCtx = nullptr;
        int status = JS_ExecutePendingJob(rt, &jobCtx);
        if (status == 0) break; // queue is empty
        if (status < 0) {       // a job threw; log it and keep draining
            JSValue exc = JS_GetException(jobCtx);
            const char* msg = JS_ToCString(jobCtx, exc);
            std::wstring line = L"[promise] Error: " + utf8ToWide(msg) + L"\n";
            wprintf(L"%ls", line.c_str());
            OutputDebugStringW(line.c_str());
            JS_FreeCString(jobCtx, msg);
            JS_FreeValue(jobCtx, exc);
        }
    }
}

std::wstring JSEngine::eval(const std::wstring& code) {
    std::string src = wideToUtf8(code);
    ArmScriptWatchdog(ctx);
    JSValue result = JS_Eval(ctx, src.c_str(), src.size(), "<eval>", JS_EVAL_TYPE_GLOBAL);

    std::wstring out;
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        out = L"Error: " + utf8ToWide(msg);
        JS_FreeCString(ctx, msg);

        // Error objects quickjs throws carry a "stack" string (source
        // position, call frames) that JS_ToCString(exc) alone doesn't
        // include - append it so a logged error is actually traceable back
        // to the script/line that threw, not just its message.
        JSValue stack = JS_GetPropertyStr(ctx, exc, "stack");
        const char* stackStr = JS_ToCString(ctx, stack);
        if (stackStr && *stackStr) out += L"\n" + utf8ToWide(stackStr);
        JS_FreeCString(ctx, stackStr);
        JS_FreeValue(ctx, stack);

        JS_FreeValue(ctx, exc);
    }
    else {
        const char* str = JS_ToCString(ctx, result);
        out = utf8ToWide(str);
        JS_FreeCString(ctx, str);
    }
    JS_FreeValue(ctx, result);
    runPendingJobs(ctx); // promise callbacks the script queued run right after it, before the next script
    return out;
}

void runJSEngineSmokeTest() {
    JSEngine js;
    std::wstring result = js.eval(L"1 + 2");
    std::wstring line = L"[JSEngine smoke test] 1 + 2 = " + result + L"\n";
    wprintf(L"%ls", line.c_str());
    OutputDebugStringW(line.c_str());
}
