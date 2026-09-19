#define NOMINMAX
#include <GLFW/glfw3.h>
#include <windows.h>
#include <string>
#include "AddressBar.h"
#include "Engine.h"
#include "Fetcher.h"
#include "OpenGLRenderer.h"

static const wchar_t* kDefaultPage =
    L"<html><head><title>WTEngine</title></head><body>"
    L"<div style='background:#cfe8ff;padding:8px'>Hello OpenGL!</div>"
    L"<div style='padding:8px'>Pass a URL as the first argument, or click a link:</div>"
    L"<div><a href='https://example.com/'>https://example.com/</a></div>"
    L"</body></html>";

struct App {
    GLFWwindow* window = nullptr;
    Engine* engine = nullptr;
    OpenGLRenderer* renderer = nullptr;
    AddressBar bar;
    std::wstring currentUrl;
    std::wstring pendingUrl; // set by input callbacks, handled in the main loop
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

// Loads `url` into the engine, or shows an error page if the fetch fails.
static void navigate(App& app, const std::wstring& url) {
    FetchResult res = fetchPage(url);
    if (res.ok) {
        app.currentUrl = res.finalUrl.empty() ? url : res.finalUrl;
        app.engine->loadHTML(res.html);
    }
    else {
        app.currentUrl = url;
        app.engine->loadHTML(L"<html><body><div style='background:#ffd6d6;padding:8px'>Failed to load "
                             + escapeHtml(url) + L"</div><div>" + escapeHtml(res.error) + L"</div></body></html>");
    }
    app.engine->scroll(-1000000000); // back to the top
    glfwSetWindowTitle(app.window, toUtf8(app.currentUrl + L" - WTEngine").c_str());
    app.bar.setText(app.currentUrl);
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
    if (button != GLFW_MOUSE_BUTTON_LEFT || action != GLFW_PRESS) return;
    App* app = static_cast<App*>(glfwGetWindowUserPointer(window));

    int x, y;
    cursorInFramebuffer(window, x, y);

    // Click on the address bar: focus it / move the caret
    if (y < AddressBar::kHeight) {
        app->bar.onClick(x, *app->renderer);
        return;
    }

    // Click anywhere else: leave the bar and discard unsent edits
    if (app->bar.focused()) {
        app->bar.blur();
        app->bar.setText(app->currentUrl);
    }

    std::wstring href = app->engine->linkAt(x, y, *app->renderer);
    if (href.empty()) return;

    std::wstring target = resolveUrl(app->currentUrl, href);
    if (!target.empty()) app->pendingUrl = target;
}

static void onChar(GLFWwindow* window, unsigned int codepoint) {
    App* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    if (app->bar.focused()) app->bar.onChar(codepoint);
}

static void onKey(GLFWwindow* window, int key, int, int action, int mods) {
    if (action == GLFW_RELEASE) return; // PRESS and REPEAT both edit
    App* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    AddressBar& bar = app->bar;
    bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;

    // Ctrl+L / F6 jump to the address bar from anywhere
    if ((ctrl && key == GLFW_KEY_L) || key == GLFW_KEY_F6) {
        bar.focus();
        return;
    }
    if (!bar.focused()) return;

    if (ctrl && key == GLFW_KEY_V) {
        const char* clip = glfwGetClipboardString(window);
        if (clip) bar.insert(fromUtf8(clip));
    }
    else if (ctrl && key == GLFW_KEY_A) {
        bar.selectAll();
    }
    else if (ctrl && key == GLFW_KEY_C) {
        if (bar.allSelected()) glfwSetClipboardString(window, toUtf8(bar.text()).c_str());
    }
    else if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) {
        std::wstring target = AddressBar::normalizeInput(bar.text());
        bar.blur();
        if (!target.empty()) app->pendingUrl = target;
    }
    else if (key == GLFW_KEY_ESCAPE) {
        bar.blur();
        bar.setText(app->currentUrl); // discard edits
    }
    else {
        bar.onEditKey(key);
    }
}

static void onScroll(GLFWwindow* window, double, double yoffset) {
    App* app = static_cast<App*>(glfwGetWindowUserPointer(window));
    app->engine->scroll(static_cast<int>(-yoffset * 40));
}

int wmain(int argc, wchar_t** argv) {
    if (!glfwInit()) return -1;

    GLFWwindow* window = glfwCreateWindow(900, 600, "ToyEngine OpenGL", nullptr, nullptr);
    if (!window) { glfwTerminate(); return -1; }

    glfwMakeContextCurrent(window);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    Engine engine(900, 600);
    OpenGLRenderer renderer;

    engine.setRenderer(&renderer);

    App app;
    app.window = window;
    app.engine = &engine;
    app.renderer = &renderer;
    glfwSetWindowUserPointer(window, &app);
    glfwSetMouseButtonCallback(window, onMouseButton);
    glfwSetScrollCallback(window, onScroll);
    glfwSetCharCallback(window, onChar);
    glfwSetKeyCallback(window, onKey);

    engine.setTopInset(AddressBar::kHeight); // page starts below the address bar

    GLFWcursor* handCursor = glfwCreateStandardCursor(GLFW_HAND_CURSOR);
    GLFWcursor* ibeamCursor = glfwCreateStandardCursor(GLFW_IBEAM_CURSOR);
    GLFWcursor* shownCursor = nullptr; // nullptr = default arrow

    if (argc > 1) navigate(app, argv[1]);
    else engine.loadHTML(kDefaultPage);

    while (!glfwWindowShouldClose(window)) {
        if (!app.pendingUrl.empty()) {
            std::wstring url = std::move(app.pendingUrl);
            app.pendingUrl.clear();
            navigate(app, url);
        }

        int width, height;
        glfwGetFramebufferSize(window, &width, &height);
        glViewport(0, 0, width, height);

        glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        engine.onResize(width, height);
        renderer.beginFrame(width, height, 0);
        engine.render(renderer);
        app.bar.draw(renderer, width, glfwGetTime()); // after the page so it covers overscroll

        // I-beam over the address bar, hand over links, arrow elsewhere
        int cx, cy;
        cursorInFramebuffer(window, cx, cy);
        GLFWcursor* wanted = nullptr;
        if (cy < AddressBar::kHeight) wanted = ibeamCursor;
        else if (!engine.linkAt(cx, cy, renderer).empty()) wanted = handCursor;
        if (wanted != shownCursor) {
            glfwSetCursor(window, wanted);
            shownCursor = wanted;
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glfwDestroyCursor(handCursor);
    glfwDestroyCursor(ibeamCursor);
    glfwTerminate();
    return 0;
}
