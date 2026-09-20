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

JSEngine::JSEngine() {
    rt = JS_NewRuntime();
    ctx = JS_NewContext(rt);

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

std::wstring JSEngine::eval(const std::wstring& code) {
    std::string src = wideToUtf8(code);
    JSValue result = JS_Eval(ctx, src.c_str(), src.size(), "<eval>", JS_EVAL_TYPE_GLOBAL);

    std::wstring out;
    if (JS_IsException(result)) {
        JSValue exc = JS_GetException(ctx);
        const char* msg = JS_ToCString(ctx, exc);
        out = L"Error: " + utf8ToWide(msg);
        JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, exc);
    }
    else {
        const char* str = JS_ToCString(ctx, result);
        out = utf8ToWide(str);
        JS_FreeCString(ctx, str);
    }
    JS_FreeValue(ctx, result);
    return out;
}

void runJSEngineSmokeTest() {
    JSEngine js;
    std::wstring result = js.eval(L"1 + 2");
    std::wstring line = L"[JSEngine smoke test] 1 + 2 = " + result + L"\n";
    wprintf(L"%ls", line.c_str());
    OutputDebugStringW(line.c_str());
}
