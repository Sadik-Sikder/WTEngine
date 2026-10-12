// JSEngine.h
#pragma once
#include <string>
#include <chrono>

// How long one run of script may take before the watchdog (see
// ArmScriptWatchdog below) stops it. A page's own <script>s get the most:
// they build the whole page at load, and an interpreter runs them several
// times slower than a browser's JIT would. Timers, listeners and promise
// jobs get kScriptTimeout. Both are short enough that a real infinite loop
// frees the window soon; meanwhile Windows doesn't gray it out as "Not
// Responding" (main.cpp turns ghosting off), it just waits.
constexpr std::chrono::milliseconds kScriptTimeout{ 5000 };
constexpr std::chrono::milliseconds kPageScriptTimeout{ 10000 };

// Resets the watchdog deadline to now() + `budget`. Every real entry point
// into JS (JSEngine::eval, runPendingJobs, and JSBinding.cpp's
// fireDueTimers/event dispatch) must call this immediately before its
// JS_Eval/JS_Call/JS_ExecutePendingJob - the interrupt handler is polled
// per JSRuntime, not per call, so this is what makes the timeout measure
// "how long did this one script/callback run" instead of wall clock since
// page load.
void ArmScriptWatchdog(struct JSContext* ctx, std::chrono::milliseconds budget = kScriptTimeout);

// Moves the current deadline `extra` later: for time a script spent
// waiting on something other than itself (a module import() waits for).
void ExtendScriptWatchdog(struct JSContext* ctx, std::chrono::milliseconds extra);

// Thin wrapper around a quickjs-ng runtime + context: the foundation that
// per-page script execution, DOM binding, events and timers build on.
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
    // `nested`: run from inside another script (one it inserted) - that
    // script's watchdog deadline stands, and promise jobs wait until the
    // outer one is done, as microtasks do.
    struct Result { bool ok; std::wstring text; };
    Result eval(const std::wstring& code, const char* filename = "<eval>", bool nested = false);

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
// ("click listener", "timer", ...) and frees it.
void reportException(struct JSContext* ctx, struct JSValue exc, const wchar_t* source);
