// TextEditor.h
#pragma once
#include <string>

class Renderer;

// Editing state for a single-line text field: the text, a caret, and a
// selection. Shared by the address bar and HTML <input> fields; it knows
// nothing about where or how the field is drawn.
class TextEditor {
public:
    void setText(const std::wstring& t);      // caret to the end, nothing selected
    const std::wstring& text() const { return text_; }

    // Password fields: text is shown as bullets and can't be word-selected.
    void setMasked(bool masked) { masked_ = masked; }
    bool masked() const { return masked_; }
    std::wstring display() const;             // the text as drawn

    size_t cursor() const { return cursor_; }
    size_t selLow() const { return anchor_ < cursor_ ? anchor_ : cursor_; }
    size_t selHigh() const { return anchor_ < cursor_ ? cursor_ : anchor_; }
    bool hasSelection() const { return anchor_ != cursor_; }
    std::wstring selectedText() const;
    void selectAll();
    void collapseTo(size_t pos) { cursor_ = anchor_ = pos; }

    void onChar(unsigned int codepoint);      // typed character
    void insert(const std::wstring& s);       // paste; replaces the selection
    // Backspace / Delete / arrows / Home / End (GLFW key codes).
    // Returns true if the key was an editing key and was handled.
    bool onEditKey(int key);

    // Mouse. `localX` is the pointer's x relative to the text's left edge
    // (with any horizontal scrolling already accounted for).
    // registerClick returns true if this click and the previous one form a
    // double-click (close in time and position).
    bool registerClick(int windowX, double timeSeconds);
    void placeCaretAt(float localX, Renderer& renderer, float fontSize);
    void selectWordAt(float localX, Renderer& renderer, float fontSize);

private:
    std::wstring text_;
    size_t cursor_ = 0;  // caret (the moving end of the selection)
    size_t anchor_ = 0;  // the fixed end; equals cursor_ when nothing is selected
    bool masked_ = false;

    double lastClickTime_ = -1;
    int lastClickX_ = 0;

    bool deleteSelection(); // returns true if something was removed
};
