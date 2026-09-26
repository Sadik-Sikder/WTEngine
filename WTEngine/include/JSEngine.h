// JSEngine.h
#pragma once
#include <string>
#include <chrono>

// How long a single script/timer-callback/listener/promise-job run may run
// before the watchdog (see ArmScriptWatchdog below) kills it. Generous
// enough for real page scripts, short enough to recover well before
// Windows would flag the window "Not Responding".
constexpr std::chrono::milliseconds kScriptTimeout{ 2000 };

// Resets the watchdog deadline to now() + kScriptTimeout. Every real entry
// point into JS (JSEngine::eval, runPendingJobs, and JSBinding.cpp's
// fireDueTimers/dispatchClick) must call this immediately before its
// JS_Eval/JS_Call/JS_ExecutePendingJob - the interrupt handler is polled
// per JSRuntime, not per call, so this is what makes the timeout measure
// "how long did this one script/callback run" instead of wall clock since
// page load.
void ArmScriptWatchdog(struct JSContext* ctx);

// Thin wrapper around a quickjs-ng runtime + context. This is the
// foundation later phases (per-page script execution, DOM binding, events,
// timers) build on; for now it only proves the engine is vendored and runs
// correctly, via eval() and runJSEngineSmokeTest() below.
class JSEngine {
public:
    JSEngine();
    ~JSEngine();
    JSEngine(const JSEngine&) = delete;
    JSEngine& operator=(const JSEngine&) = delete;

    // Evaluates `code` as a standalone global script. `filename` is what
    // error stacks name as its location (a script's URL, "<inline script
    // 2>", ...). On success, `text` is the result coerced to a string; if
    // it threw, `ok` is false and `text` describes the error (see
    // describeException) - the caller decides whether to log it.
    struct Result { bool ok; std::wstring text; };
    Result eval(const std::wstring& code, const char* filename = "<eval>");

    // Exposes the underlying context so external code (DOM bindings) can
    // install itself into it. JSEngine itself stays DOM-agnostic - it
    // doesn't know what a binding is, just hands out the context.
    struct JSContext* context() const { return ctx; }

private:
    struct JSRuntime* rt = nullptr;
    struct JSContext* ctx = nullptr;
    // The watchdog deadline and not-yet-handled promise rejections, reachable
    // from anywhere holding just a JSContext* via JS_GetRuntimeOpaque.
    // Defined in JSEngine.cpp, since it holds JSValues.
    struct RuntimeState* state_ = nullptr;
};

// Runs every queued promise job (.then/.catch callbacks, async/await
// continuations) until the queue is empty, like a browser's microtask
// checkpoint. quickjs never runs these on its own, so call this after
// anything that can run JS: a script's eval, a timer callback, an event
// listener. Then reports to the console any promise rejected during all
// that which still has no handler ("Uncaught (in promise) ..."), as
// browsers do at the same point.
void runPendingJobs(struct JSContext* ctx);

// A readable description of a thrown value for the console: an Error's
// "Name: message" plus its stack (which names the script and line), or any
// other thrown value formatted like console.log would. A watchdog kill is
// spelled out as such.
std::wstring describeException(struct JSContext* ctx, struct JSValue exc);

// Logs a caught exception to the console as an error from `source`
// ("click handler", "timer", ...) and frees it.
void reportException(struct JSContext* ctx, struct JSValue exc, const wchar_t* source);

// Phase 0 acceptance check: evaluates a trivial expression and reports the
// result via stdout and OutputDebugStringW. Superseded once a later phase
// adds real per-page script execution.
void runJSEngineSmokeTest();
