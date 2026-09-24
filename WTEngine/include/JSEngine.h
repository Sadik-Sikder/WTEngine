// JSEngine.h
#pragma once
#include <string>
#include <chrono>

// How long a single script/timer-callback/listener/promise-job run may run
// before the watchdog (see ArmScriptWatchdog below) kills it. Generous
// enough for real page scripts, short enough to recover well before
// Windows would flag the window "Not Responding".
constexpr std::chrono::milliseconds kScriptTimeout{ 2000 };

// Deadline the watchdog interrupt handler checks against. Lives at a
// stable address for the JSRuntime's whole lifetime (a JSEngine member),
// and is reachable from anywhere holding just a JSContext* via
// JS_GetRuntimeOpaque - see JSEngine.cpp.
struct JSWatchdogState {
    std::chrono::steady_clock::time_point deadline;
};

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

    // Evaluates `code` as a standalone global script and returns its
    // result coerced to a string, or "Error: <message>" if it threw (this
    // includes "Error: interrupted" if the watchdog killed it).
    std::wstring eval(const std::wstring& code);

    // Exposes the underlying context so external code (DOM bindings) can
    // install itself into it. JSEngine itself stays DOM-agnostic - it
    // doesn't know what a binding is, just hands out the context.
    struct JSContext* context() const { return ctx; }

private:
    struct JSRuntime* rt = nullptr;
    struct JSContext* ctx = nullptr;
    JSWatchdogState watchdog_;
};

// Runs every queued promise job (.then/.catch callbacks, async/await
// continuations) until the queue is empty, like a browser's microtask
// checkpoint. quickjs never runs these on its own, so call this after
// anything that can run JS: a script's eval, a timer callback, an event
// listener. A job that throws is logged and the rest still run.
void runPendingJobs(struct JSContext* ctx);

// Phase 0 acceptance check: evaluates a trivial expression and reports the
// result via stdout and OutputDebugStringW. Superseded once a later phase
// adds real per-page script execution.
void runJSEngineSmokeTest();
