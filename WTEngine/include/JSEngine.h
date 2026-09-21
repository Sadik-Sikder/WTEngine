// JSEngine.h
#pragma once
#include <string>

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
    // result coerced to a string, or "Error: <message>" if it threw.
    std::wstring eval(const std::wstring& code);

    // Exposes the underlying context so external code (DOM bindings) can
    // install itself into it. JSEngine itself stays DOM-agnostic - it
    // doesn't know what a binding is, just hands out the context.
    struct JSContext* context() const { return ctx; }

private:
    struct JSRuntime* rt = nullptr;
    struct JSContext* ctx = nullptr;
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
