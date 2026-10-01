// FindBar.h
#pragma once
#include <string>
#include "TextEditor.h"

class Renderer;

// The find-in-page box (Ctrl+F): a small panel at the top right of the
// page, just below the address bar, with a search field, a "3/12" match
// count, previous/next buttons and a close button. It only edits the
// query and reports what the user asked for; Engine does the searching
// (Engine::findText / findNext / clearFind).
class FindBar {
public:
    // What a click or key in the bar asks for.
    enum class Action { None, Edited, Next, Previous, Close };

    bool open() const { return open_; }
    // Ctrl+F: opens (or re-focuses) the bar with its text selected, so typing replaces it.
    void openAndFocus() { open_ = true; focused_ = true; input_.selectAll(); }
    void close() { open_ = false; focused_ = false; }
    bool focused() const { return focused_; }
    void blur() { focused_ = false; }
    const std::wstring& query() const { return input_.text(); }

    bool contains(int x, int y, int windowWidth) const;
    Action onClick(int x, int y, int windowWidth, Renderer& renderer, double timeSeconds);

    // Keyboard, while focused. Typing and edit keys return Edited when the
    // query changed (the caller re-runs the search).
    Action onChar(unsigned int codepoint);
    Action onKey(int key, bool shift);
    Action insert(const std::wstring& s);
    void selectAll() { input_.selectAll(); }
    std::wstring selectedText() const { return input_.hasSelection() ? input_.selectedText() : L""; }

    // `current` is 0-based, -1 for no match.
    void draw(Renderer& renderer, int windowWidth, int current, int count, double timeSeconds);

private:
    bool open_ = false;
    bool focused_ = false;
    TextEditor input_;
    float scrollX_ = 0;
};
