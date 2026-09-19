// AddressBar.cpp
#define NOMINMAX
#include "AddressBar.h"
#include "Renderer.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace {
    // Arrow glyphs are spelled as numbers so the source file encoding can not garble them.
    const wchar_t kArrowLeft[] = { 0x2190, 0 };
    const wchar_t kArrowRight[] = { 0x2192, 0 };

    const int kNavSize = 28;    // Back / Forward buttons are kNavSize wide
    const int kNavBackX = 10;
    const int kNavFwdX = kNavBackX + kNavSize + 4;
    const int kFieldX = kNavFwdX + kNavSize + 8; // field's left edge (right of the buttons)
    const int kFieldY = 6;
    const int kFieldH = 28;
    const int kBarMargin = 10;  // gap to the window's right edge
    const int kTextPad = 8;     // gap between field edge and text
    const float kFontSize = 15;
    const int kTextTop = 4;     // text is drawn this far below the field's top

    const Color kBarBg{ 0.90f, 0.91f, 0.93f, 1.0f };
    const Color kBorder{ 0.72f, 0.74f, 0.78f, 1.0f };
    const Color kBorderFocus{ 0.20f, 0.45f, 0.90f, 1.0f };
    const Color kField{ 1, 1, 1, 1 };
    const Color kSelection{ 0.68f, 0.82f, 1.0f, 1.0f };
    const Color kInk{ 0, 0, 0, 1 };
    const Color kPlaceholder{ 0.55f, 0.55f, 0.58f, 1.0f };
}

void AddressBar::setText(const std::wstring& text) {
    ed_.setText(text);
    scrollX_ = 0;
}

void AddressBar::focus() {
    focused_ = true;
    ed_.selectAll();
}

void AddressBar::blur() {
    focused_ = false;
    ed_.collapseTo(ed_.cursor());
}

void AddressBar::onClick(int x, Renderer& renderer, double now) {
    bool isDouble = ed_.registerClick(x, now);

    if (!focused_) { focus(); return; }

    float localX = x - (kFieldX + kTextPad) + scrollX_;
    if (isDouble) ed_.selectWordAt(localX, renderer, kFontSize);
    else ed_.placeCaretAt(localX, renderer, kFontSize);
}

int AddressBar::navButtonAt(int x, int y) const {
    if (y < kFieldY || y >= kFieldY + kFieldH) return 0;
    if (canBack_ && x >= kNavBackX && x < kNavBackX + kNavSize) return -1;
    if (canForward_ && x >= kNavFwdX && x < kNavFwdX + kNavSize) return 1;
    return 0;
}

void AddressBar::draw(Renderer& r, int windowWidth, double t) {
    const int fieldW = std::max(windowWidth - kFieldX - kBarMargin, 50);
    const int viewW = fieldW - 2 * kTextPad; // visible text area
    const std::wstring& text = ed_.text();

    // Keep the caret in view while editing; show the start when idle.
    float caretX = r.measureText(text.substr(0, ed_.cursor()), kFontSize);
    if (!focused_) scrollX_ = 0;
    else {
        if (caretX - scrollX_ > viewW) scrollX_ = caretX - viewW;
        if (caretX - scrollX_ < 0) scrollX_ = caretX;
    }

    // Bar background, then the field with a border
    r.drawRect(0, 0, (float)windowWidth, (float)kHeight, kBarBg);

    // Back / Forward buttons: arrows, grayed out when there's nowhere to go
    auto navButton = [&](int x, const wchar_t* arrow, bool enabled) {
        r.drawRect((float)x - 1, (float)kFieldY - 1, (float)kNavSize + 2, (float)kFieldH + 2, kBorder);
        r.drawRect((float)x, (float)kFieldY, (float)kNavSize, (float)kFieldH, kField);
        float w = r.measureText(arrow, 16);
        r.drawText(x + (kNavSize - w) / 2, (float)kFieldY + 3, arrow, 16, enabled ? kInk : kPlaceholder);
    };
    navButton(kNavBackX, kArrowLeft, canBack_);
    navButton(kNavFwdX, kArrowRight, canForward_);

    r.drawRect((float)kFieldX - 1, (float)kFieldY - 1, (float)fieldW + 2, (float)kFieldH + 2,
               focused_ ? kBorderFocus : kBorder);
    r.drawRect((float)kFieldX, (float)kFieldY, (float)fieldW, (float)kFieldH, kField);

    // Long text scrolls sideways; keep it inside the field.
    r.setClip((float)kFieldX, (float)kFieldY, (float)fieldW, (float)kFieldH);

    const float textX = kFieldX + kTextPad - scrollX_;
    const float textY = (float)(kFieldY + kTextTop);

    if (text.empty()) {
        r.drawText((float)(kFieldX + kTextPad), textY, L"Type a URL and press Enter",
                   kFontSize, kPlaceholder);
    }
    else {
        if (ed_.hasSelection()) {
            float x0 = r.measureText(text.substr(0, ed_.selLow()), kFontSize);
            float x1 = r.measureText(text.substr(0, ed_.selHigh()), kFontSize);
            r.drawRect(textX + x0, (float)kFieldY + 3, x1 - x0, (float)kFieldH - 6, kSelection);
        }
        r.drawText(textX, textY, text, kFontSize, kInk);
    }

    if (focused_ && !ed_.hasSelection() && std::fmod(t, 1.0) < 0.6) { // blinking caret
        r.drawRect(kFieldX + kTextPad + caretX - scrollX_, (float)kFieldY + 4, 1.5f,
                   (float)kFieldH - 8, kInk);
    }

    r.clearClip();
}

std::wstring AddressBar::normalizeInput(const std::wstring& input) {
    size_t a = 0, b = input.size();
    while (a < b && iswspace(input[a])) a++;
    while (b > a && iswspace(input[b - 1])) b--;
    std::wstring s = input.substr(a, b - a);
    if (s.empty()) return L"";

    if (s.find(L"://") != std::wstring::npos) return s; // already a URL

    bool driveLetter = s.size() >= 3 && iswalpha(s[0]) && s[1] == L':' &&
                       (s[2] == L'\\' || s[2] == L'/');
    bool uncPath = s.rfind(L"\\\\", 0) == 0;
    if (driveLetter || uncPath || GetFileAttributesW(s.c_str()) != INVALID_FILE_ATTRIBUTES)
        return s; // local file

    return L"https://" + s;
}
