// main
#include <windows.h>
#include <string>
#include <fstream>
#include "Engine.h"

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static Engine* engine = nullptr;
    switch (msg) {
    case WM_CREATE: {
        engine = new Engine(hWnd);
		// Future will load from file or network
        std::wstring html = LR"(
            <html>
              <body>
                <div style="background:#cfe8ff;padding:8px">
                  <p style="font-size:18px;margin:6px">Hello from <span style="font-weight:bold">ToyEngine</span>!</p>
                  <p>This is a minimal browser engine demo. It supports block layout and basic inline style.</p>
                </div>
                <div style="background:#ffe8cf;padding:8px;margin-top:8px">
                  <p>Second box with some longer text to demonstrate automatic wrapping. Resize the window to re-layout.</p>
                </div>
              </body>
            </html>
        )";
        engine->loadHTML(html);
        return 0;
    }
    case WM_SIZE:
        if (engine) engine->onResize(LOWORD(lParam), HIWORD(lParam));
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        /*TextOutW(hdc, 50, 50, L"Hello World from Win32", 24);
        Rectangle(hdc, 40, 80, 300, 150);*/
        if (engine) engine->render(hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        if (engine) { delete engine; engine = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR pCmdLine, int nCmdShow) {
    const wchar_t CLASS_NAME[] = L"WTEngineWindowClass";
    WNDCLASS wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        0, CLASS_NAME, L"WTEngine - Minimal Browser Engine",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 900, 600,
        NULL, NULL, hInstance, NULL
    );

    if (hwnd == NULL) return 0;
    ShowWindow(hwnd, nCmdShow);

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}
