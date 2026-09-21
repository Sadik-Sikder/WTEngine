// Engine.h
#pragma once
#include <string>
#include <memory>
#include <vector>
#include "DOM.h"
#include "JSBinding.h"
#include "Layout.h"
#include "TextEditor.h"

// A form the user submitted. The caller resolves `action` against the current
// page's URL and performs the request.
struct FormSubmission {
    std::wstring action; // the form's action attribute (may be empty or relative)
    bool post = false;
    std::string body;    // application/x-www-form-urlencoded fields
};

class Renderer;
class JSEngine;
class Engine {
public:
    Engine(int w, int h);
    ~Engine();

    void loadHTML(const std::wstring& html, const std::wstring& baseUrl = L"");
    void onResize(int width, int height);
    void setRenderer(Renderer* r); // used to measure text for wrapping
    void setTopInset(int px);      // reserve space above the page (e.g. for the address bar)
    void scroll(int delta);
    void render(Renderer& renderer, double timeSeconds = 0);
    int getDocumentHeight() const;
    int getScrollY() const { return scrollY; }

    // Returns the href of the link under window-space point (x, y), or empty.
    std::wstring linkAt(int x, int y, Renderer& renderer) const;

    // --- Form controls -------------------------------------------------
    enum class Cursor { Arrow, Hand, IBeam };
    Cursor cursorAt(int x, int y, Renderer& renderer) const;

    // Handles a click at window (x, y): focuses a text field, toggles a
    // checkbox, or presses a button. Returns true if a control was hit;
    // otherwise any focused field loses focus.
    bool onClick(int x, int y, double timeSeconds, Renderer& renderer);

    // Fires any addEventListener('click', ...) registered on the element
    // at window (x, y), bubbling up through its ancestors. Returns true if
    // a listener called event.preventDefault() - the caller should then
    // skip its own default handling (e.g. following a link) for this click.
    bool dispatchClick(int x, int y);

    bool hasFocusedInput() const { return focusedEl != nullptr; }
    void blurInput();
    void onChar(unsigned int codepoint);
    void insertText(const std::wstring& s);
    bool onEditKey(int key);
    void selectAllInput();
    std::wstring focusedSelection() const; // empty for password fields
    void focusNextInput(bool backwards);   // Tab / Shift+Tab
    void submitFocused();                  // Enter in a text field

    // Retrieves (and clears) a submission queued by a button press or Enter.
    bool takeSubmission(FormSubmission& out);

private:
    int width, height, documentHeight = 0;
    int scrollY = 0;
    int topInset = 0;
    int viewHeight() const { return height - topInset; }

    std::shared_ptr<Document> document;
    LayoutRoot layoutRoot;
    Renderer* measurer = nullptr;
    std::wstring pageBaseUrl; // current page's URL, for resolving <img src> against
    int lastImageGeneration = -1; // renderer's imageGeneration() as of the last layout

    // A fresh JS realm per page (recreated on every loadHTML(), matching
    // how navigation already discards and re-parses the whole DOM).
    std::unique_ptr<JSEngine> jsEngine;
    DOMBindingState domState; // reset alongside jsEngine; see JSBinding.h
    void runScripts();

    // Form state. The focused field's text lives in `editor` while editing
    // and is copied back to the element's "value" attribute after each edit.
    Element* focusedEl = nullptr;
    Element* focusedForm = nullptr;
    TextEditor editor;
    float inputScrollX = 0;
    bool hasSubmission = false;
    FormSubmission submission;

    // The <select> currently showing its dropdown, or nullptr. openSelectBox
    // is a snapshot of that select's own closed box (doc-space x/y/width/
    // height/fontSize), captured when opened - it anchors the dropdown's
    // position and its option rows' hit-testing, since the dropdown itself
    // isn't part of layoutRoot.boxes (see drawOpenSelect).
    Element* openSelect = nullptr;
    LayoutBox openSelectBox;

    void parseAndBuild(const std::wstring& html);
    void doLayout();

    const LayoutBox* controlAt(int x, int y) const;
    void focusInput(const LayoutBox& box);
    void syncValue();
    void queueSubmit(Element* form, Element* submitter);
    void drawControl(Renderer& renderer, const LayoutBox& box, int screenY, double timeSeconds);
    void drawOpenSelect(Renderer& renderer); // draws openSelect's dropdown rows, if one is open
};
