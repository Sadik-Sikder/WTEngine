// AddressBar.h
#pragma once
#include <string>
#include "TextEditor.h"

class Renderer;

// A single-line URL text field drawn across the top of the window.
// Editing is done by a TextEditor; this class adds focus, drawing, and the
// address-bar-specific click behaviour. The window's input callbacks (see
// main.cpp) forward characters, keys and clicks to it.
class AddressBar {
public:
    static constexpr int kHeight = 40; // pixels reserved at the top of the window

    // Replaces the text (e.g. after navigation). Does not change focus.
    void setText(const std::wstring& text);
    const std::wstring& text() const { return ed_.text(); }

    bool focused() const { return focused_; }
    void focus();  // focus and select everything, like Ctrl+L in a browser
    void blur();
    void selectAll() { ed_.selectAll(); }

    bool hasSelection() const { return ed_.hasSelection(); }
    std::wstring selectedText() const { return ed_.selectedText(); }

    void onChar(unsigned int codepoint) { ed_.onChar(codepoint); }
    void insert(const std::wstring& s) { ed_.insert(s); }
    bool onEditKey(int key) { return ed_.onEditKey(key); }

    // Click at window x. The first click on an unfocused bar focuses it and
    // selects all; later clicks place the caret; a second click in quick
    // succession selects the word under the pointer.
    void onClick(int x, Renderer& renderer, double timeSeconds);

    void draw(Renderer& renderer, int windowWidth, double timeSeconds);

    // Turns what the user typed into something fetchPage can load:
    // adds https:// to bare hosts, leaves URLs and local paths alone.
    static std::wstring normalizeInput(const std::wstring& input);

private:
    TextEditor ed_;
    bool focused_ = false;
    float scrollX_ = 0; // horizontal scroll of the text inside the field
};
