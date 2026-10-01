#define NOMINMAX
#include <GLFW/glfw3.h>
#include <windows.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h> // glfwGetWin32Window, for sizingWindowProc
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#include <string>
#include <algorithm>
#include <cmath>
#include "AddressBar.h"
#include "DevConsole.h"
#include "FindBar.h"
#include "Engine.h"
#include "Fetcher.h"
#include "JSEngine.h"
#include "OpenGLRenderer.h"
#include "PageHistory.h"
#include "PageLoader.h"

static const wchar_t* kDefaultPage =
    L"<html><head><title>WTEngine</title></head><body>"
    L"<div style='background:#cfe8ff;padding:8px'>Hello OpenGL!</div>"
    L"<div style='padding:8px'>Type a URL above, click a link, or try the form:</div>"
    L"<div><a href='https://example.com/'>https://example.com/</a></div>"
    L"<form action='https://duckduckgo.com/html/' method='get'>"
    L"<div>Search DuckDuckGo:</div>"
    L"<input name='q' placeholder='Type a query and press Enter'>"
    L"<input type='submit' value='Search'>"
    L"</form>"
    L"</body></html>";

struct App {
    GLFWwindow* window = nullptr;
    Engine* engine = nullptr;
    OpenGLRenderer* renderer = nullptr;
    AddressBar bar;
    std::wstring currentUrl;
    std::wstring pendingUrl; // set by input callbacks, handled in the main loop
    int pendingHistory = 0;  // -1 = go back, +1 = go forward (also handled in the main loop)
    bool pendingReload = false; // Reload button / F5 / Ctrl+R (also handled in the main loop)
    bool pendingStop = false;   // Stop button / Esc while loading (also handled in the main loop)
    PageHistory history;
    PageLoader pageLoader; // fetches navigate()'s target in the background; see applyFinishedNavigation
    std::wstring shownTitle; // what the window title bar currently says - see updateWindowTitle
    DevConsolePanel console; // F12 - see DevConsole.h
    FindBar find;            // Ctrl+F - see FindBar.h
    GLFWcursor* handCursor = nullptr;  // standard cursors, created in wmain
    GLFWcursor* ibeamCursor = nullptr;
    GLFWcursor* shownCursor = nullptr; // what's set now; nullptr = default arrow
    bool drawing = false;              // inside drawFrame - see there
};

static std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

static std::wstring fromUtf8(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0); // includes the terminator
    if (n <= 1) return L"";
    std::wstring out(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), n);
    return out;
}

static std::wstring escapeHtml(const std::wstring& in) {
    std::wstring out;
    for (wchar_t c : in) {
        if (c == L'<') out += L"&lt;";
        else if (c == L'>') out += L"&gt;";
        else if (c == L'&') out += L"&amp;";
        else out.push_back(c);
    }
    return out;
}

// Sets the window title from the page's <title> ("Title - WTEngine"),
// falling back to its URL when it has none. Called every frame, so a
// script's document.title = ... shows up too; only touches the window
// when the text actually changes.
static void updateWindowTitle(App& app) {
    std::wstring title = app.engine->title();
    std::wstring full;
    if (title == L"WTEngine") full = title; // the built-in demo page
    else if (!title.empty()) full = title + L" - WTEngine";
    else if (!app.currentUrl.empty()) full = app.currentUrl + L" - WTEngine";
    else full = L"WTEngine";
    if (full == app.shownTitle) return;
    app.shownTitle = full;
    glfwSetWindowTitle(app.window, toUtf8(full).c_str());
}

// Displays a history entry: loads its page, restores its scroll position, and
// updates the window title and address bar.
static void showEntry(App& app, const HistoryEntry& entry) {
    app.currentUrl = entry.url;
    app.engine->loadHTML(entry.html, entry.url);
    app.engine->scroll(-1000000000); // to the top...
    app.engine->scroll(entry.scrollY); // ...then to where the user was (clamped)
    updateWindowTitle(app);
    app.bar.setText(entry.url);
}

// Visits a brand-new page: remembers where we were scrolled on the page we're
// leaving, pushes the new page onto the history, and shows it.
static void visitPage(App& app, const std::wstring& url, const std::wstring& html) {
    app.history.saveScroll(app.engine->getScrollY());
    app.history.visit(HistoryEntry{ url, html, 0 });
    showEntry(app, *app.history.current());
}

// Like visitPage, but replaces the current history entry instead of pushing
// a new one - for a navigation that shouldn't be a Back-button stop of its
// own (JS location.replace()/.reload() - see PageHistory::replaceCurrent).
static void replacePage(App& app, const std::wstring& url, const std::wstring& html) {
    app.history.saveScroll(app.engine->getScrollY());
    app.history.replaceCurrent(HistoryEntry{ url, html, 0 });
    showEntry(app, *app.history.current());
}

// Shows a re-fetched copy of the current page (see reload()), keeping both
// Back and Forward, and the scroll position of what's on screen right now
// - the user may have scrolled while it loaded. (Restored once the new
// page is laid out; content still loading below, like images, can make
// the page shorter at that moment, which pulls the position up.)
static void reloadPage(App& app, const std::wstring& url, const std::wstring& html) {
    app.history.reloadCurrent(HistoryEntry{ url, html, app.engine->getScrollY() });
    showEntry(app, *app.history.current());
}

// Starts fetching `url` in the background (as a POST if `postBody` is
// given) - see PageLoader. Superseding whatever was previously in flight
// (a second navigate() call, or goHistory()'s Back/Forward) is fine and
// expected; nothing shows until applyFinishedNavigation picks up a result.
// `kind` says how the result updates the history.
static void navigate(App& app, const std::wstring& url, const std::string* postBody = nullptr,
                     NavigationKind kind = NavigationKind::Visit) {
    app.pageLoader.start(url, postBody, kind);
}

// Re-fetches the page being shown (Reload button, F5, Ctrl+R) - from the
// network or disk, not the HTML cached in history the way Back/Forward
// redisplay it, so edits to a local file show up too. Always a GET: a page
// that came from a form POST is reloaded by URL rather than re-submitting
// the form (a browser would ask first; re-sending could e.g. repeat an order).
static void reload(App& app) {
    const HistoryEntry* current = app.history.current();
    if (!current) return;
    if (current->url.empty()) {
        // The built-in start page has nothing to fetch; showing it again
        // still re-runs its scripts, like a reload.
        app.pageLoader.cancel();
        app.history.saveScroll(app.engine->getScrollY());
        showEntry(app, *app.history.current());
        return;
    }
    navigate(app, current->url, nullptr, NavigationKind::Reload);
}

// Called once per frame: shows a navigate()-started fetch's result the
// moment it's ready - the actual page, or a "Failed to load" page if the
// fetch failed. Does nothing while no fetch has finished (including while
// none is in flight at all).
static void applyFinishedNavigation(App& app) {
    FetchResult res;
    std::wstring url;
    NavigationKind kind = NavigationKind::Visit;
    if (!app.pageLoader.poll(res, url, kind)) return;

    auto show = kind == NavigationKind::Reload ? reloadPage
              : kind == NavigationKind::Replace ? replacePage
              : visitPage;
    if (res.ok) {
        show(app, res.finalUrl.empty() ? url : res.finalUrl, res.html);
    }
    else {
        show(app, url,
             L"<html><body><div style='background:#ffd6d6;padding:8px'>Failed to load "
             + escapeHtml(url) + L"</div><div>" + escapeHtml(res.error) + L"</div></body></html>");
    }
}

// Back (-1) or forward (+1) through the history; does nothing at either end.
// Cancels any in-flight background navigation first - Back/Forward always
// wins over a load that was already on its way, and redisplays cached
// HTML instantly regardless, so there's nothing worth waiting for anyway.
static void goHistory(App& app, int direction) {
    app.pageLoader.cancel();
    app.history.saveScroll(app.engine->getScrollY());
    const HistoryEntry* entry = direction < 0 ? app.history.back() : app.history.forward();
    if (entry) showEntry(app, *entry);
}

// Sends a submitted form: GET puts the fields in the query string, POST in the body.
static void submitForm(App& app, const FormSubmission& form) {
    std::wstring target = form.action.empty() ? app.currentUrl : resolveUrl(app.currentUrl, form.action);
    if (target.rfind(L"http://", 0) != 0 && target.rfind(L"https://", 0) != 0)
        return; // forms need a web page to send to (not a local file)

    if (form.post) navigate(app, target, &form.body);
    else navigate(app, withQuery(target, form.body));
}

// Carries out what a key or click in the find bar asked for.
static void applyFindAction(App& app, FindBar::Action action) {
    switch (action) {
    case FindBar::Action::Edited:   app.engine->findText(app.find.query()); break; // search as you type
    case FindBar::Action::Next:     app.engine->findNext(false); break;
    case FindBar::Action::Previous: app.engine->findNext(true); break;
    case FindBar::Action::Close:    app.engine->clearFind(); break;
    case FindBar::Action::None:     break;
    }
}

// Page zoom steps - the same ones Chrome uses.
static const float kZoomLevels[] = { 0.25f, 0.33f, 0.5f, 0.67f, 0.75f, 0.8f, 0.9f, 1.0f,
                                     1.1f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f, 4.0f, 5.0f };

// One zoom step in (+1) or out (-1) from the current level.
static void stepZoom(App& app, int direction) {
    const int count = (int)(sizeof(kZoomLevels) / sizeof(kZoomLevels[0]));
    float current = app.engine->zoom();
    int nearest = 0;
    for (int i = 1; i < count; i++)
        if (std::fabs(kZoomLevels[i] - current) < std::fabs(kZoomLevels[nearest] - current)) nearest = i;
    int next = std::clamp(nearest + direction, 0, count - 1);
    app.engine->setZoom(kZoomLevels[next]);
}

// Cursor position in framebuffer pixels (matches the engine's coordinate space).
static void cursorInFramebuffer(GLFWwindow* window, int& x, int& y) {
    double cx, cy;
    glfwGetCursorPos(window, &cx, &cy);
    int winW, winH, fbW, fbH;
    glfwGetWindowSize(window, &winW, &winH);
    glfwGetFramebufferSize(window, &fbW, &fbH);
    double sx = winW > 0 ? (double)fbW / winW : 1.0;
    double sy = winH > 0 ? (double)fbH / winH : 1.0;
    x = (int)(cx * sx);
    y = (int)(cy * sy);
}

static void onMouseButton(GLFWwindow* window, int button, int action, int) {
    App* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    if (action == GLFW_RELEASE && button == GLFW_MOUSE_BUTTON_LEFT) {
        app->engine->endScrollbarDrag();
        return;
    }
    if (action != GLFW_PRESS) return;

    // Mouse side buttons: back / forward
    if (button == GLFW_MOUSE_BUTTON_4) { app->pendingHistory = -1; return; }
    if (button == GLFW_MOUSE_BUTTON_5) { app->pendingHistory = 1; return; }
    if (button != GLFW_MOUSE_BUTTON_LEFT) return;

    int x, y;
    cursorInFramebuffer(window, x, y);

    // Click on the address bar: Back / Forward / Reload buttons, or focus the field
    if (y < AddressBar::kHeight) {
        switch (app->bar.navButtonAt(x, y)) {
        case AddressBar::NavButton::Back:    app->pendingHistory = -1; return;
        case AddressBar::NavButton::Forward: app->pendingHistory = 1; return;
        case AddressBar::NavButton::Reload:  app->pendingReload = true; return;
        case AddressBar::NavButton::Stop:    app->pendingStop = true; return;
        case AddressBar::NavButton::ConsoleBadge: app->console.show(); return;
        case AddressBar::NavButton::ZoomReset: app->engine->setZoom(1.0f); return;
        case AddressBar::NavButton::None:    break;
        }
        app->engine->blurInput();
        app->console.blur();
        app->find.blur();
        app->bar.onClick(x, *app->renderer, glfwGetTime());
        return;
    }

    // Click anywhere else: leave the bar and discard unsent edits
    if (app->bar.focused()) {
        app->bar.blur();
        app->bar.setText(app->currentUrl);
    }

    // Click on the find bar: its buttons, or its search field
    int fbW, fbH;
    glfwGetFramebufferSize(window, &fbW, &fbH);
    if (app->find.contains(x, y, fbW)) {
        app->engine->blurInput();
        app->console.blur();
        applyFindAction(*app, app->find.onClick(x, y, fbW, *app->renderer, glfwGetTime()));
        return;
    }
    app->find.blur(); // stays open, with its highlights, until closed

    // Click on the developer console: its buttons, or its input line
    if (app->console.contains(y, fbH)) {
        app->engine->blurInput();
        app->console.onClick(x, y, fbW, fbH, *app->renderer, glfwGetTime());
        return;
    }
    app->console.blur(); // a click on the page

    // The scrollbar, overlaid on the page's right edge: drag the thumb, or
    // click the track to move a screenful towards the click.
    switch (app->engine->scrollbarAt(x, y)) {
    case Engine::ScrollbarPart::Thumb: app->engine->beginScrollbarDrag(y); return;
    case Engine::ScrollbarPart::Track: app->engine->pageTowards(y); return;
    case Engine::ScrollbarPart::None:  break;
    }

    // Form controls first (focus a field, toggle a checkbox, press a button).
    // onClick runs their JS click listeners itself, so a control hit ends here.
    if (app->engine->onClick(x, y, glfwGetTime(), *app->renderer)) return;

    std::wstring href = app->engine->linkAt(x, y, *app->renderer);

    // addEventListener('click', ...) listeners next; a listener calling
    // event.preventDefault() suppresses the link navigation below (real
    // sites routinely intercept nav clicks with a JS router this way).
    bool prevented = app->engine->dispatchClick(x, y);
    if (href.empty() || prevented) return;

    std::wstring target = resolveUrl(app->currentUrl, href);
    if (!target.empty()) app->pendingUrl = target;
}

static void onChar(GLFWwindow* window, unsigned int codepoint) {
    App* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    if (app->bar.focused()) app->bar.onChar(codepoint);
    else if (app->find.focused()) applyFindAction(*app, app->find.onChar(codepoint));
    else if (app->console.focused()) app->console.onChar(codepoint);
    else app->engine->onChar(codepoint);
}

static void onKey(GLFWwindow* window, int key, int, int action, int mods) {
    if (action == GLFW_RELEASE) return; // PRESS and REPEAT both edit
    App* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    AddressBar& bar = app->bar;
    Engine& engine = *app->engine;
    bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;

    // Alt+Left / Alt+Right: back / forward (works even while typing in a field)
    if ((mods & GLFW_MOD_ALT) && (key == GLFW_KEY_LEFT || key == GLFW_KEY_RIGHT)) {
        app->pendingHistory = key == GLFW_KEY_LEFT ? -1 : 1;
        return;
    }

    // F5 / Ctrl+R: reload (also works while typing in a field, as in browsers)
    if (key == GLFW_KEY_F5 || (ctrl && key == GLFW_KEY_R)) {
        if (action == GLFW_PRESS) app->pendingReload = true; // not on key-repeat: holding F5 shouldn't reload 30 times a second
        return;
    }

    // Ctrl+= / Ctrl+- / Ctrl+0 (and the numpad's + - 0): zoom in / out /
    // reset. Like F5, works even while typing in a field.
    if (ctrl) {
        if (key == GLFW_KEY_EQUAL || key == GLFW_KEY_KP_ADD) { stepZoom(*app, 1); return; }
        if (key == GLFW_KEY_MINUS || key == GLFW_KEY_KP_SUBTRACT) { stepZoom(*app, -1); return; }
        if (key == GLFW_KEY_0 || key == GLFW_KEY_KP_0) { engine.setZoom(1.0f); return; }
    }

    // Ctrl+L / F6 jump to the address bar from anywhere
    if ((ctrl && key == GLFW_KEY_L) || key == GLFW_KEY_F6) {
        engine.blurInput();
        app->console.blur();
        app->find.blur();
        bar.focus();
        return;
    }

    // Ctrl+F: open (or return to) the find bar, its text selected so typing
    // replaces it; an existing query is searched again on this page.
    FindBar& find = app->find;
    if (ctrl && key == GLFW_KEY_F) {
        if (bar.focused()) { bar.blur(); bar.setText(app->currentUrl); }
        engine.blurInput();
        app->console.blur();
        find.openAndFocus();
        if (!find.query().empty()) engine.findText(find.query());
        return;
    }

    // F3 / Shift+F3, Ctrl+G / Ctrl+Shift+G: next / previous match, from anywhere
    if (key == GLFW_KEY_F3 || (ctrl && key == GLFW_KEY_G)) {
        if (find.open()) applyFindAction(*app, (mods & GLFW_MOD_SHIFT) ? FindBar::Action::Previous : FindBar::Action::Next);
        return;
    }

    // Keys for the find bar's field, while it has focus
    if (find.focused()) {
        if (ctrl && key == GLFW_KEY_V) {
            if (const char* clip = glfwGetClipboardString(window)) applyFindAction(*app, find.insert(fromUtf8(clip)));
        }
        else if (ctrl && key == GLFW_KEY_C) {
            std::wstring selection = find.selectedText();
            if (!selection.empty()) glfwSetClipboardString(window, toUtf8(selection).c_str());
        }
        else if (ctrl && key == GLFW_KEY_A) {
            find.selectAll();
        }
        else {
            applyFindAction(*app, find.onKey(key, (mods & GLFW_MOD_SHIFT) != 0));
        }
        return;
    }

    // F12: open/close the developer console. Opening it also focuses its
    // input line, ready to type.
    DevConsolePanel& console = app->console;
    if (key == GLFW_KEY_F12) {
        if (action != GLFW_PRESS) return;
        console.toggle();
        if (console.open()) {
            if (bar.focused()) { bar.blur(); bar.setText(app->currentUrl); }
            engine.blurInput();
            find.blur();
            int fbW, fbH;
            glfwGetFramebufferSize(window, &fbW, &fbH);
            console.onClick(0, fbH - 1, fbW, fbH, *app->renderer, glfwGetTime()); // same as clicking its input line
        }
        return;
    }

    // Keys for the console's input line, while it has focus
    if (console.focused()) {
        if (ctrl && key == GLFW_KEY_V) {
            if (const char* clip = glfwGetClipboardString(window)) console.insert(fromUtf8(clip));
        }
        else if (ctrl && key == GLFW_KEY_C) {
            std::wstring selection = console.selectedText();
            if (!selection.empty()) glfwSetClipboardString(window, toUtf8(selection).c_str());
        }
        else if (ctrl && key == GLFW_KEY_A) {
            console.selectAll();
        }
        else {
            std::wstring code = console.onKey(key);
            if (!code.empty()) engine.consoleEval(code);
        }
        return;
    }

    // Keys go to whichever text field has focus: the address bar or a page input
    bool inBar = bar.focused();
    if (!inBar && !engine.hasFocusedInput()) {
        // Nothing has keyboard focus, so keys act on the page itself.
        bool shift = (mods & GLFW_MOD_SHIFT) != 0;
        switch (key) {
        case GLFW_KEY_DOWN:      engine.scrollLines(1); break;
        case GLFW_KEY_UP:        engine.scrollLines(-1); break;
        case GLFW_KEY_PAGE_DOWN: engine.scrollPages(1); break;
        case GLFW_KEY_PAGE_UP:   engine.scrollPages(-1); break;
        case GLFW_KEY_SPACE:     engine.scrollPages(shift ? -1 : 1); break; // as in browsers
        case GLFW_KEY_HOME:      engine.scrollToEdge(false); break;
        case GLFW_KEY_END:       engine.scrollToEdge(true); break;
        case GLFW_KEY_ESCAPE:
            // Esc stops a page load, like a browser's. (In a field, Esc
            // keeps its meaning - blur / discard edits - below.)
            if (action == GLFW_PRESS && app->pageLoader.loading()) app->pendingStop = true;
            break;
        default: break;
        }
        return;
    }

    if (key == GLFW_KEY_TAB && !inBar) {
        engine.focusNextInput((mods & GLFW_MOD_SHIFT) != 0);
    }
    else if (ctrl && key == GLFW_KEY_V) {
        const char* clip = glfwGetClipboardString(window);
        if (clip) {
            if (inBar) bar.insert(fromUtf8(clip));
            else engine.insertText(fromUtf8(clip));
        }
    }
    else if (ctrl && key == GLFW_KEY_A) {
        if (inBar) bar.selectAll();
        else engine.selectAllInput();
    }
    else if (ctrl && key == GLFW_KEY_C) {
        std::wstring selection = inBar ? (bar.hasSelection() ? bar.selectedText() : L"")
                                       : engine.focusedSelection();
        if (!selection.empty()) glfwSetClipboardString(window, toUtf8(selection).c_str());
    }
    else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) {
        if (inBar) {
            std::wstring target = AddressBar::normalizeInput(bar.text());
            bar.blur();
            if (!target.empty()) app->pendingUrl = target;
        }
        else {
            engine.submitFocused(); // queues the form; the main loop sends it
        }
    }
    else if (key == GLFW_KEY_ESCAPE) {
        if (inBar) {
            bar.blur();
            bar.setText(app->currentUrl); // discard edits
        }
        else {
            engine.blurInput();
        }
    }
    else {
        if (inBar) bar.onEditKey(key);
        else engine.onEditKey(key);
    }
}

static void onScroll(GLFWwindow* window, double, double yoffset) {
    App* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    int x, y, fbW, fbH;
    cursorInFramebuffer(window, x, y);
    glfwGetFramebufferSize(window, &fbW, &fbH);
    bool ctrl = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS ||
                glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS;
    if (app->console.contains(y, fbH)) app->console.onScroll(yoffset); // wheel over the console scrolls its log
    else if (ctrl) stepZoom(*app, yoffset > 0 ? 1 : -1);               // Ctrl+wheel zooms, as in browsers
    else app->engine->scroll(static_cast<int>(-yoffset * 40));
}

// Draws one frame: the page, then the browser UI over it, then the cursor
// shape, then presents it. Called once per main-loop iteration - and also
// from the window-refresh callback (onWindowRefresh), because while the
// user drags a window edge, Windows runs its own modal sizing loop inside
// glfwPollEvents() and the main loop doesn't get to run until the mouse is
// released. Without that, the window would show stale, stretched content
// for the whole drag.
static void drawFrame(App& app) {
    if (app.drawing) return; // a refresh delivered from inside this same frame
    app.drawing = true;
    GLFWwindow* window = app.window;
    Engine& engine = *app.engine;
    OpenGLRenderer& renderer = *app.renderer;
    GLFWcursor* handCursor = app.handCursor;
    GLFWcursor* ibeamCursor = app.ibeamCursor;
    GLFWcursor*& shownCursor = app.shownCursor;

    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    glViewport(0, 0, width, height);

    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    engine.onResize(width, height);
    engine.setBottomInset(app.console.height(height)); // the page scrolls within what the console leaves
    renderer.beginFrame(width, height, 0);
    engine.render(renderer, glfwGetTime());
    updateWindowTitle(app);
    app.bar.setNavEnabled(app.history.canGoBack(), app.history.canGoForward(), app.history.current() != nullptr,
                          app.pageLoader.loading());
    app.bar.setErrorCount(consoleLog().errorCount());
    app.bar.setZoomPercent((int)std::lround(engine.zoom() * 100));
    app.bar.draw(renderer, width, glfwGetTime()); // after the page so it covers overscroll
    app.console.draw(renderer, width, height, glfwGetTime()); // likewise covers the page's bottom
    app.find.draw(renderer, width, engine.findCurrentIndex(), engine.findMatchCount(), glfwGetTime());

    // I-beam over the address bar and text fields, hand over links and
    // buttons, arrow elsewhere
    int cx, cy;
    cursorInFramebuffer(window, cx, cy);
    GLFWcursor* wanted = nullptr;
    bool overConsole = app.console.contains(cy, height);
    bool overFind = app.find.contains(cx, cy, width);
    if (engine.draggingScrollbar()) engine.dragScrollbar(cy); // follows the mouse until the button is released
    if (cy < AddressBar::kHeight) {
        wanted = app.bar.navButtonAt(cx, cy) != AddressBar::NavButton::None ? handCursor : ibeamCursor;
    }
    else if (overFind) {
        wanted = nullptr; // its own small UI; the page beneath doesn't get hover
    }
    else if (overConsole) {
        wanted = cy >= height - 28 ? ibeamCursor : nullptr; // I-beam on its input line
    }
    else if (engine.draggingScrollbar() || engine.scrollbarAt(cx, cy) != Engine::ScrollbarPart::None) {
        wanted = nullptr; // plain arrow over the scrollbar, not whatever page content is beneath it
    }
    else {
        switch (engine.cursorAt(cx, cy, renderer)) {
        case Engine::Cursor::IBeam: wanted = ibeamCursor; break;
        case Engine::Cursor::Hand:  wanted = handCursor;  break;
        default: break;
        }
    }
    // Off the page (over the address bar, the find bar or the console) hovers nothing.
    if (cy < AddressBar::kHeight || overConsole || overFind) engine.updateHover(-1, -1);
    else engine.updateHover(cx, cy);

    if (wanted != shownCursor) {
        glfwSetCursor(window, wanted);
        shownCursor = wanted;
    }

    glfwSwapBuffers(window);
    app.drawing = false;
}

static void onWindowRefresh(GLFWwindow* window) {
    drawFrame(*static_cast<App*>(glfwGetWindowUserPointer(window)));
}

// GLFW doesn't report when the user starts or stops dragging a window edge,
// but Engine::onResize wants to know (a drag defers expensive relayouts
// until it pauses; a maximize or snap shouldn't wait). So GLFW's window
// procedure is wrapped to catch Windows' WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE;
// every message still goes on to GLFW unchanged.
static WNDPROC g_glfwWindowProc = nullptr;
static Engine* g_sizingEngine = nullptr;

static LRESULT CALLBACK sizingWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_sizingEngine) {
        if (msg == WM_ENTERSIZEMOVE) g_sizingEngine->setLiveResize(true);
        else if (msg == WM_EXITSIZEMOVE) g_sizingEngine->setLiveResize(false);
    }
    return CallWindowProcW(g_glfwWindowProc, hwnd, msg, wp, lp);
}

// The loop otherwise redraws as fast as it possibly can, spinning a CPU core
// for no reason while sitting idle (and relying on whatever vsync default the
// driver happens to pick). Capping it here makes the rate explicit and
// independent of that.
constexpr double kTargetFrameSeconds = 1.0 / 60.0;

int wmain(int argc, wchar_t** argv) {
    // Windows' default scheduler tick is ~15.6ms, so a short Sleep() below
    // tends to overshoot to the next tick; this asks for 1ms resolution so
    // the frame cap actually lands near its target instead of running slow.
    timeBeginPeriod(1);

    runJSEngineSmokeTest(); // Phase 0 check: quickjs-ng is vendored and runs correctly

    if (!glfwInit()) { timeEndPeriod(1); return -1; }

    GLFWwindow* window = glfwCreateWindow(900, 600, "WTEngine", nullptr, nullptr);
    if (!window) { glfwTerminate(); timeEndPeriod(1); return -1; }

    glfwMakeContextCurrent(window);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Engine engine(900, 600);
    OpenGLRenderer renderer;

    engine.setRenderer(&renderer);

    // See sizingWindowProc.
    HWND hwnd = glfwGetWin32Window(window);
    g_sizingEngine = &engine;
    g_glfwWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(sizingWindowProc)));

    App app;
    app.window = window;
    app.engine = &engine;
    app.renderer = &renderer;
    glfwSetWindowUserPointer(window, &app);
    glfwSetMouseButtonCallback(window, onMouseButton);
    glfwSetScrollCallback(window, onScroll);
    glfwSetCharCallback(window, onChar);
    glfwSetKeyCallback(window, onKey);
    glfwSetWindowRefreshCallback(window, onWindowRefresh); // keeps drawing during a live resize - see drawFrame

    engine.setTopInset(AddressBar::kHeight); // page starts below the address bar

    app.handCursor = glfwCreateStandardCursor(GLFW_HAND_CURSOR);
    app.ibeamCursor = glfwCreateStandardCursor(GLFW_IBEAM_CURSOR);

    // A command-line URL starts loading in the background (see navigate());
    // the window opens blank and applyFinishedNavigation shows it once
    // ready, same as any other navigation. The built-in default page needs
    // no fetch, so it shows immediately.
    if (argc > 1) navigate(app, argv[1]);
    else visitPage(app, L"", kDefaultPage);

    while (!glfwWindowShouldClose(window)) {
        double frameStart = glfwGetTime();

        if (app.pendingHistory != 0) {
            int direction = app.pendingHistory;
            app.pendingHistory = 0;
            goHistory(app, direction);
        }

        if (app.pendingStop) {
            app.pendingStop = false;
            app.pageLoader.cancel(); // the current page just stays up, as in a browser
            // ...so the bar should name that page again, not the abandoned
            // target - unless the user is mid-edit in it.
            if (!app.bar.focused()) app.bar.setText(app.currentUrl);
        }

        if (app.pendingReload) {
            app.pendingReload = false;
            reload(app);
        }

        if (!app.pendingUrl.empty()) {
            std::wstring url = std::move(app.pendingUrl);
            app.pendingUrl.clear();
            navigate(app, url);
        }

        FormSubmission form;
        if (engine.takeSubmission(form)) submitForm(app, form);

        std::wstring navUrl;
        bool navReplace = false;
        if (engine.takeNavigation(navUrl, navReplace))
            navigate(app, navUrl, nullptr, navReplace ? NavigationKind::Replace : NavigationKind::Visit);

        applyFinishedNavigation(app); // shows a navigate()-started fetch's result once it's ready

        drawFrame(app);
        glfwPollEvents();

        // Sleep off whatever's left of the 1/60s budget, so the loop settles
        // at ~60 iterations/sec instead of spinning as fast as it can.
        double remaining = kTargetFrameSeconds - (glfwGetTime() - frameStart);
        if (remaining > 0) Sleep(static_cast<DWORD>(remaining * 1000.0));
    }

    glfwDestroyCursor(app.handCursor);
    glfwDestroyCursor(app.ibeamCursor);
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_glfwWindowProc)); // GLFW's own, before it destroys the window
    g_sizingEngine = nullptr;
    glfwTerminate();
    timeEndPeriod(1);
    return 0;
}
