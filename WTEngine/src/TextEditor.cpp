// TextEditor.cpp
#define NOMINMAX
#include "TextEditor.h"
#include "Renderer.h"
#include <GLFW/glfw3.h> // key codes only
#include <cmath>
#include <cstdlib>
#include <cwctype>

void TextEditor::setText(const std::wstring& t) {
    text_ = t;
    collapseTo(text_.size());
}

std::wstring TextEditor::display() const {
    return masked_ ? std::wstring(text_.size(), L'•') : text_;
}

std::wstring TextEditor::selectedText() const {
    return text_.substr(selLow(), selHigh() - selLow());
}

void TextEditor::selectAll() {
    anchor_ = 0;
    cursor_ = text_.size();
}

bool TextEditor::deleteSelection() {
    if (!hasSelection()) return false;
    size_t lo = selLow();
    text_.erase(lo, selHigh() - lo);
    collapseTo(lo);
    return true;
}

void TextEditor::insert(const std::wstring& s) {
    std::wstring clean;
    for (wchar_t c : s) {
        if (c >= 32 && c != 127) clean.push_back(c); // drop newlines/control chars
    }
    if (clean.empty()) return;

    deleteSelection(); // typing or pasting replaces the selection
    text_.insert(cursor_, clean);
    collapseTo(cursor_ + clean.size());
}

void TextEditor::onChar(unsigned int cp) {
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

bool TextEditor::onEditKey(int key) {
    switch (key) {
    case GLFW_KEY_BACKSPACE:
        if (!deleteSelection() && cursor_ > 0) { text_.erase(cursor_ - 1, 1); collapseTo(cursor_ - 1); }
        return true;
    case GLFW_KEY_DELETE:
        if (!deleteSelection() && cursor_ < text_.size()) text_.erase(cursor_, 1);
        return true;
    case GLFW_KEY_LEFT: // with a selection, jump to its left edge
        if (hasSelection()) collapseTo(selLow());
        else if (cursor_ > 0) collapseTo(cursor_ - 1);
        return true;
    case GLFW_KEY_RIGHT:
        if (hasSelection()) collapseTo(selHigh());
        else if (cursor_ < text_.size()) collapseTo(cursor_ + 1);
        return true;
    case GLFW_KEY_HOME:
        collapseTo(0);
        return true;
    case GLFW_KEY_END:
        collapseTo(text_.size());
        return true;
    }
    return false;
}

bool TextEditor::registerClick(int windowX, double now) {
    bool isDouble = lastClickTime_ >= 0 && now - lastClickTime_ < 0.4 &&
                    std::abs(windowX - lastClickX_) <= 4;
    lastClickTime_ = isDouble ? -1 : now; // a third click starts over
    lastClickX_ = windowX;
    return isDouble;
}

// Puts the caret at the character boundary nearest the pointer.
void TextEditor::placeCaretAt(float localX, Renderer& renderer, float fontSize) {
    std::wstring shown = display();
    size_t best = 0;
    float bestDist = std::fabs(localX);
    for (size_t i = 1; i <= shown.size(); i++) {
        float w = renderer.measureText(shown.substr(0, i), fontSize);
        float d = std::fabs(localX - w);
        if (d < bestDist) { bestDist = d; best = i; }
    }
    collapseTo(best);
}

static bool isWordChar(wchar_t c) {
    return iswalnum(c) || c == L'_';
}

// Selects the word (run of letters/digits/_) under the pointer. On punctuation,
// selects that run of punctuation instead. Does nothing on empty text.
void TextEditor::selectWordAt(float localX, Renderer& renderer, float fontSize) {
    if (text_.empty()) return;
    if (masked_) { selectAll(); return; } // don't reveal a password's word breaks

    // Find the character whose horizontal extent contains the pointer.
    size_t idx = text_.size() - 1; // past the end -> last character
    for (size_t i = 0; i < text_.size(); i++) {
        if (localX < renderer.measureText(text_.substr(0, i + 1), fontSize)) { idx = i; break; }
    }

    if (iswspace(text_[idx])) { collapseTo(idx); return; }

    bool word = isWordChar(text_[idx]);
    auto sameKind = [&](wchar_t c) { return !iswspace(c) && isWordChar(c) == word; };

    size_t lo = idx, hi = idx + 1;
    while (lo > 0 && sameKind(text_[lo - 1])) lo--;
    while (hi < text_.size() && sameKind(text_[hi])) hi++;
    anchor_ = lo;
    cursor_ = hi;
}
