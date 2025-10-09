// Engine.cpp
#include "Engine.h"
#include "HTMLParser.h"

Engine::Engine(HWND hwnd_) : hwnd(hwnd_), width(800), height(600) {
    RECT rc; GetClientRect(hwnd, &rc);
    width = rc.right - rc.left;
    height = rc.bottom - rc.top;
    layoutRoot.viewportWidth = width;
}

Engine::~Engine() {}

void Engine::loadHTML(const std::wstring& html) {
    parseAndBuild(html);
    doLayout();
    InvalidateRect(hwnd, NULL, TRUE);
}

void Engine::parseAndBuild(const std::wstring& html) {
    HTMLParser parser;
    document = parser.parse(html);
    layoutRoot.rootNode = document->body.get();
}

void Engine::onResize(int w, int h) {
    width = w; height = h;
    layoutRoot.viewportWidth = width;
    doLayout();
}

void Engine::doLayout() {

    if (!document) return;
    layoutRoot.boxes.clear();
    layoutRoot.layout();
}

void Engine::render(HDC hdc) {
    OutputDebugString(L"Engine::render called\n");

    if (!document || !document->body) {
        MessageBox(hwnd, L"HTML parsing failed or <body> not found.", L"Error", MB_OK);
        return;
    }
    if (layoutRoot.boxes.empty()) {
        MessageBox(hwnd, L"No layout boxes generated.", L"Error", MB_OK);
        return;
    }

    // white background
    RECT rc; GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW+1));

    // render computed boxes
    for (auto& b : layoutRoot.boxes) {
        // draw background if present
        if (!b.background.empty()) {
            COLORREF col = RGB(240,240,240);
            // parse simple hex #rrggbb
            if (b.background.size() == 7 && b.background[0] == L'#') {
                int r = std::stoi(std::string(b.background.begin()+1, b.background.begin()+3), nullptr, 16);
                int g = std::stoi(std::string(b.background.begin()+3, b.background.begin()+5), nullptr, 16);
                int bl= std::stoi(std::string(b.background.begin()+5, b.background.begin()+7), nullptr, 16);
                col = RGB(r,g,bl);
            }
            HBRUSH brush = CreateSolidBrush(col);
            RECT r = { b.x, b.y, b.x + b.width, b.y + b.height };
            FillRect(hdc, &r, brush);
            DeleteObject(brush);
        }

        // draw text
        if (!b.text.empty()) {
            RECT tr = { b.x + 4, b.y + 4, b.x + b.width - 4, b.y + b.height - 4 };
            // set font size from node style (basic)
            int fontSize = b.fontSize > 0 ? b.fontSize : 14;
            HFONT hf = CreateFontW(-MulDiv(fontSize, GetDeviceCaps(hdc, LOGPIXELSY), 72),
                                   0,0,0, FW_DONTCARE, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HFONT oldf = (HFONT)SelectObject(hdc, hf);
            DrawTextW(hdc, b.text.c_str(), (int)b.text.size(), &tr, DT_WORDBREAK);
            SelectObject(hdc, oldf);
            DeleteObject(hf);
        }
    }
}

std::wstring html = LR"(<body><p>Hello World!</p></body>)";
