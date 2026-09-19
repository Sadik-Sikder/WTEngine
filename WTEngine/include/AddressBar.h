// AddressBar.h
#pragma once
#include <string>

class Renderer;

// A single-line URL text field drawn across the top of the window.
// It only holds editing state and draws itself; the window's input callbacks
// (see main.cpp) forward characters, keys and clicks to it.
class AddressBar {
public:
    static constexpr int kHeight = 40; // pixels reserved at the top of the window

    // Replaces the text (e.g. after navigation). Does not change focus.
    void setText(const std::wstring& text);
    const std::wstring& text() const { return text_; }

    bool focused() const { return focused_; }
    void focus();  // focus and select everything, like Ctrl+L in a browser
    void blur();
    void selectAll();
    bool allSelected() const { return allSelected_; }

    void onChar(unsigned int codepoint);      // typed character
    void insert(const std::wstring& s);       // paste
    // Backspace / Delete / arrows / Home / End (GLFW key codes).
    // Returns true if the key was an editing key and was handled.
    bool onEditKey(int key);

    // Click at window x: focuses the bar; the first click selects all,
    // later clicks place the caret.
    void onClick(int x, Renderer& renderer);

    void draw(Renderer& renderer, int windowWidth, double timeSeconds);

    // Turns what the user typed into something fetchPage can load:
    // adds https:// to bare hosts, leaves URLs and local paths alone.
    static std::wstring normalizeInput(const std::wstring& input);

private:
    std::wstring text_;
    size_t cursor_ = 0;
    bool focused_ = false;
    bool allSelected_ = false;
    float scrollX_ = 0; // horizontal scroll of the text inside the field
};
