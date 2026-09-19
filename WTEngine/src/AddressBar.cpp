// AddressBar.cpp
#define NOMINMAX
#include "AddressBar.h"
#include "Renderer.h"
#include <GLFW/glfw3.h> // key codes only
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace {
    const int kFieldX = 10;     // field's left edge
    const int kFieldY = 6;
    const int kFieldH = 28;
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
    text_ = text;
    cursor_ = text_.size();
    allSelected_ = false;
    scrollX_ = 0;
}

void AddressBar::focus() {
    focused_ = true;
    selectAll();
}

void AddressBar::blur() {
    focused_ = false;
    allSelected_ = false;
}

void AddressBar::selectAll() {
    allSelected_ = !text_.empty();
    cursor_ = text_.size();
}

void AddressBar::insert(const std::wstring& s) {
    std::wstring clean;
    for (wchar_t c : s) {
        if (c >= 32 && c != 127) clean.push_back(c); // drop newlines/control chars
    }
    if (clean.empty()) return;

    if (allSelected_) { text_.clear(); cursor_ = 0; allSelected_ = false; }
    text_.insert(cursor_, clean);
    cursor_ += clean.size();
}

void AddressBar::onChar(unsigned int cp) {
    if (cp < 32 || cp == 127) return;
    std::wstring s;
    if (cp > 0xFFFF) { // UTF-16 surrogate pair
        cp -= 0x10000;
        s.push_back((wchar_t)(0xD800 + (cp >> 10)));
        s.push_back((wchar_t)(0xDC00 + (cp & 0x3FF)));
    }
    else {
        s.push_back((wchar_t)cp);
    }
    insert(s);
}

bool AddressBar::onEditKey(int key) {
    switch (key) {
    case GLFW_KEY_BACKSPACE:
        if (allSelected_) { text_.clear(); cursor_ = 0; allSelected_ = false; }
        else if (cursor_ > 0) { text_.erase(cursor_ - 1, 1); cursor_--; }
        return true;
    case GLFW_KEY_DELETE:
        if (allSelected_) { text_.clear(); cursor_ = 0; allSelected_ = false; }
        else if (cursor_ < text_.size()) { text_.erase(cursor_, 1); }
        return true;
    case GLFW_KEY_LEFT:
        if (allSelected_) { cursor_ = 0; allSelected_ = false; }
        else if (cursor_ > 0) cursor_--;
        return true;
    case GLFW_KEY_RIGHT:
        if (allSelected_) { cursor_ = text_.size(); allSelected_ = false; }
        else if (cursor_ < text_.size()) cursor_++;
        return true;
    case GLFW_KEY_HOME:
        cursor_ = 0; allSelected_ = false;
        return true;
    case GLFW_KEY_END:
        cursor_ = text_.size(); allSelected_ = false;
        return true;
    }
    return false;
}

void AddressBar::onClick(int x, Renderer& renderer) {
    if (!focused_) { focus(); return; }

    // Place the caret at the character boundary nearest the click.
    float target = x - (kFieldX + kTextPad) + scrollX_;
    size_t best = 0;
    float bestDist = std::fabs(target);
    for (size_t i = 1; i <= text_.size(); i++) {
        float w = renderer.measureText(text_.substr(0, i), kFontSize);
        float d = std::fabs(target - w);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    cursor_ = best;
    allSelected_ = false;
}

void AddressBar::draw(Renderer& r, int windowWidth, double t) {
    const int fieldW = std::max(windowWidth - 2 * kFieldX, 50);
    const int viewW = fieldW - 2 * kTextPad; // visible text area

    // Keep the caret in view while editing; show the start when idle.
    float caretX = r.measureText(text_.substr(0, cursor_), kFontSize);
    if (!focused_) scrollX_ = 0;
    else {
        if (caretX - scrollX_ > viewW) scrollX_ = caretX - viewW;
        if (caretX - scrollX_ < 0) scrollX_ = caretX;
    }

    // Bar background, then the field with a border
    r.drawRect(0, 0, (float)windowWidth, (float)kHeight, kBarBg);
    r.drawRect((float)kFieldX - 1, (float)kFieldY - 1, (float)fieldW + 2, (float)kFieldH + 2,
               focused_ ? kBorderFocus : kBorder);
    r.drawRect((float)kFieldX, (float)kFieldY, (float)fieldW, (float)kFieldH, kField);

    const float textX = kFieldX + kTextPad - scrollX_;
    const float textY = (float)(kFieldY + kTextTop);

    if (text_.empty()) {
        r.drawText((float)(kFieldX + kTextPad), textY, L"Type a URL and press Enter",
                   kFontSize, kPlaceholder);
    }
    else {
        if (allSelected_) {
            float w = r.measureText(text_, kFontSize);
            r.drawRect(textX, (float)kFieldY + 3, w, (float)kFieldH - 6, kSelection);
        }
        r.drawText(textX, textY, text_, kFontSize, kInk);
    }

    if (focused_ && !allSelected_ && std::fmod(t, 1.0) < 0.6) { // blinking caret
        r.drawRect(kFieldX + kTextPad + caretX - scrollX_, (float)kFieldY + 4, 1.5f,
                   (float)kFieldH - 8, kInk);
    }

    // The text may run past either end of the field; there is no clipping in
    // the Renderer interface, so paint the bar colour over the overflow.
    r.drawRect(0, 0, (float)(kFieldX - 1), (float)kHeight, kBarBg);
    r.drawRect((float)(kFieldX + fieldW + 1), 0, (float)windowWidth, (float)kHeight, kBarBg);
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
