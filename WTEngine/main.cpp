// main
#include <windows.h>
#include <string>
#include <fstream>
#include "Engine.h"

static int scrollY = 0;
static int contentHeight = 0;

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
    case WM_SIZE: {
        int width = LOWORD(lParam);
        int height = HIWORD(lParam);

        if (engine) {
            engine->onResize(width, height);
            contentHeight = engine->getDocumentHeight();
        }
        int maxScroll = contentHeight - height;
        if (maxScroll < 0) maxScroll = 0;

        if (scrollY > maxScroll) scrollY = maxScroll;
        if (scrollY < 0) scrollY = 0;

        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;

        si.nMin = 0;
        si.nMax = contentHeight - 1;
        si.nPage = height;
        si.nPos = scrollY;

        SetScrollInfo(hWnd, SB_VERT, &si, TRUE);

        InvalidateRect(hWnd, NULL, TRUE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        int saved = SaveDC(hdc);

        SetViewportOrgEx(hdc, 0, -scrollY, NULL);

        if (engine) engine->render(hdc);

        RestoreDC(hdc, saved);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_VSCROLL:
    {
        SCROLLINFO si = {};
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hWnd, SB_VERT, &si);

        int oldScrollY = scrollY;

        switch (LOWORD(wParam)) {
        case SB_LINEUP:        scrollY -= 20; break;
        case SB_LINEDOWN:      scrollY += 20; break;
        case SB_PAGEUP:        scrollY -= si.nPage; break;
        case SB_PAGEDOWN:      scrollY += si.nPage; break;
        case SB_THUMBTRACK:    scrollY = si.nTrackPos; break;
        }

        scrollY = max(0, min(scrollY, si.nMax - (int)si.nPage));

        if (scrollY != oldScrollY) {
            si.fMask = SIF_POS;
            si.nPos = scrollY;
            SetScrollInfo(hWnd, SB_VERT, &si, TRUE);
            InvalidateRect(hWnd, NULL, TRUE);
        }
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
        WS_OVERLAPPEDWINDOW | WS_VSCROLL, CW_USEDEFAULT, CW_USEDEFAULT, 900, 600,
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
