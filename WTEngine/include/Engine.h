// Engine.h
#pragma once
#include <string>
#include <memory>
#include <vector>
#include <algorithm>
#include <chrono>
#include "DOM.h"
#include "JSBinding.h"
#include "Layout.h"
#include "ResourceLoader.h"
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
    // Whether the user is currently dragging a window edge (Windows'
    // WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE). During a drag, an expensive
    // relayout waits for the drag to pause; when it ends, it happens right
    // away. See onResize.
    void setLiveResize(bool live);
    void setRenderer(Renderer* r); // used to measure text for wrapping
    void setTopInset(int px);      // reserve space above the page (e.g. for the address bar)
    void setBottomInset(int px);   // reserve space below it (e.g. for the developer console)

    // Page zoom (1 = 100%). The page is laid out as if the window were
    // 1/zoom as wide - so text reflows, as in a browser - then drawn scaled
    // up by zoom. Scroll positions and layout are in page pixels; window
    // coordinates from the mouse are converted (toPage).
    void setZoom(float zoom);
    float zoom() const { return zoom_; }

    // --- Find in page (Ctrl+F) ----------------------------------------
    // Searches the page's visible text, case-insensitively and across word
    // and line boundaries, highlighting every match; the current one is
    // highlighted differently and scrolled into view. The search is kept
    // up to date as the page relayouts (until clearFind).
    void findText(const std::wstring& query); // a new search, starting at the first match in view
    void findNext(bool backwards);
    void clearFind();
    int findMatchCount() const { return (int)findMatches_.size(); }
    int findCurrentIndex() const { return findCurrent_; } // -1 when there are no matches

    // Runs a line typed into the developer console against the current
    // page (see evaluateInConsole); does nothing if the page has no JS realm.
    void consoleEval(const std::wstring& code);
    void scroll(int delta);

    // Keyboard scrolling: a "line" (arrow keys), a screenful (PageUp/PageDown/
    // Space - a little less than the viewport, so some context stays
    // visible, as browsers do), or the very top/bottom (Home/End).
    void scrollLines(int lines);
    void scrollPages(int pages);
    void scrollToEdge(bool bottom);

    // The overlay scrollbar along the page's right edge, in window
    // coordinates. Drawn by render() only when the page is taller than the
    // viewport. Drag the thumb (begin/drag/end), or click the track to move
    // a screenful towards the click.
    enum class ScrollbarPart { None, Thumb, Track };
    ScrollbarPart scrollbarAt(int x, int y) const;
    void beginScrollbarDrag(int y);
    void dragScrollbar(int y);
    void endScrollbarDrag() { draggingScrollbar_ = false; }
    bool draggingScrollbar() const { return draggingScrollbar_; }
    void pageTowards(int y);
    void render(Renderer& renderer, double timeSeconds = 0);
    int getDocumentHeight() const;
    int getScrollY() const { return scrollY; }

    // The page's <title> text (whitespace-collapsed), or empty if it has
    // none. Live: reflects a later document.title = ... from JS too.
    std::wstring title() const;

    // Returns the href of the link under window-space point (x, y), or empty.
    std::wstring linkAt(int x, int y, Renderer& renderer) const;

    // --- Form controls -------------------------------------------------
    enum class Cursor { Arrow, Hand, IBeam };
    Cursor cursorAt(int x, int y, Renderer& renderer) const;

    // Handles a click at window (x, y): focuses a text field, toggles a
    // checkbox, or presses a button. Returns true if a control was hit;
    // otherwise any focused field loses focus. When a control is hit, its
    // click listeners (and its ancestors', via bubbling) run first, and
    // event.preventDefault() cancels the toggle / submit / dropdown - so the
    // caller must not also call dispatchClick.
    bool onClick(int x, int y, double timeSeconds, Renderer& renderer);

    // Fires a click event at the element at window (x, y), bubbling up
    // through its ancestors. Returns true if a listener called
    // event.preventDefault() - the caller should then skip its own default
    // handling (e.g. following a link) for this click.
    bool dispatchClick(int x, int y);

    // Fires keydown (down) or keyup at the focused field, or at document if
    // none has focus. Returns true if a listener called preventDefault():
    // the caller should then skip the key's own action (scrolling, editing,
    // Tab, Enter, ...); the character a cancelled keydown would type is
    // dropped too (onChar).
    bool onKeyEvent(const KeyInfo& info, bool down);

    // Tells the engine where the mouse is (window coordinates; anywhere
    // off the page, e.g. over the address bar, clears the hover). Call
    // once per frame after render(). Fires mouseover/mouseout and
    // mouseenter/mouseleave when the element under the mouse changes. If
    // the change could affect a :hover rule, re-lays-out the page so the
    // next frame shows it - unless the last layout was too slow to repeat
    // on every mouse move (see kHoverRelayoutBudgetMs in Engine.cpp).
    void updateHover(int x, int y);

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

    // Retrieves (and clears) a navigation requested by JS - location.href=,
    // .replace(), .assign(), .reload(), or a bare `location = url` /
    // `window.location = url` assignment. `outReplace` is true for
    // .replace()/.reload() - the caller should then overwrite the current
    // history entry instead of pushing a new one (see
    // PageHistory::replaceCurrent).
    bool takeNavigation(std::wstring& outUrl, bool& outReplace);

private:
    int width, height, documentHeight = 0;
    int scrollY = 0;
    int topInset = 0;
    int bottomInset = 0;
    float zoom_ = 1.0f;

    // A relayout for a new window width, deferred (see onResize).
    bool relayoutPending_ = false;
    bool resizePainted_ = false; // a frame has shown the new size since then
    bool liveResize_ = false;    // the user is dragging a window edge (setLiveResize)
    std::chrono::steady_clock::time_point lastResize_;

    // Find in page. A match can span several text boxes (layout makes one
    // box per word), so it's a list of character ranges, one per box.
    struct FindSpan { size_t box; size_t start, end; }; // [start, end) within boxes[box].text
    struct FindMatch { std::vector<FindSpan> spans; };
    std::wstring findQuery_;
    std::vector<FindMatch> findMatches_;
    int findCurrent_ = -1;
    // Per layout box, the spans to highlight in it (and whether each is part
    // of the current match) - rebuilt along with findMatches_.
    std::vector<std::vector<std::pair<FindSpan, bool>>> findHighlights_;
    void runFind();                // recomputes findMatches_ against the current layout
    void rebuildFindHighlights();
    void scrollToFindMatch();
    // A text's width in page pixels as it will actually be drawn: at the
    // zoomed size the renderer rasterizes it at (see drawText), scaled back
    // by the zoom. Layout wraps with this, so words don't crowd together
    // when the zoomed glyphs come out a little wider than plain scaling.
    float measurePageText(Renderer& r, const std::wstring& text, float fontSize, bool bold,
                          bool italic = false, const std::wstring* family = nullptr) const;
    // Window point -> page point (layout coordinates, scroll included).
    // False above the page (over the address bar).
    bool toPage(int x, int y, int& pageX, int& pageY) const;

    // Scrollbar state (see scrollbarAt).
    bool draggingScrollbar_ = false;
    float dragGrabOffset_ = 0;      // where on the thumb the drag started, so it doesn't jump
    bool scrollbarHovered_ = false; // set by updateHover; the thumb darkens
    // The track and thumb, in window coordinates; false when the page fits
    // and there's no scrollbar.
    bool scrollbarGeometry(float& trackTop, float& trackH, float& thumbTop, float& thumbH) const;
    int maxScroll() const { return std::max(documentHeight - viewHeight(), 0); }
    void drawScrollbar(Renderer& renderer);
    // The visible part of the page, in page pixels (so zoom shrinks it).
    int viewHeight() const { return (int)((height - topInset - bottomInset) / zoom_); }

    std::shared_ptr<Document> document;
    LayoutRoot layoutRoot;
    Renderer* measurer = nullptr;
    std::wstring pageBaseUrl; // current page's URL, for resolving <img src> against
    int lastImageGeneration = -1; // renderer's imageGeneration() as of the last layout
    // An image layout sized by a guess has arrived since the last layout;
    // render() re-lays-out for it once enough time has passed since the
    // last layout (lastLayoutEnd_), so images streaming in are taken in
    // batches rather than one full layout each.
    bool imageRelayoutPending_ = false;
    std::chrono::steady_clock::time_point lastLayoutEnd_;

    // A fresh JS realm per page (recreated on every loadHTML(), matching
    // how navigation already discards and re-parses the whole DOM).
    std::unique_ptr<JSEngine> jsEngine;
    DOMBindingState domState; // reset alongside jsEngine; see JSBinding.h
    void beginScripts();
    JSContext* jsContext() const; // the page's JS realm, or nullptr

    // Page lifecycle events: DOMContentLoaded once every script has run,
    // load once every stylesheet has too (see fireReadyEvents).
    bool domContentLoadedFired_ = false;
    bool loadFired_ = false;
    void fireReadyEvents();

    // Whether `el` is still in the page. A JS event listener can free
    // elements (innerHTML = ...), so any Element* held across one must be
    // checked with this before it's used again. inPage walks the whole
    // tree; stillInPage skips that when no script has changed the DOM since
    // the last layout.
    bool inPage(const Element* el) const;
    bool stillInPage(const Element* el) const { return el && (!domState.domDirty || inPage(el)); }

    // MouseEvent coordinates for window point (x, y).
    MouseInfo mouseInfoAt(int x, int y) const;
    Element* mouseTarget_ = nullptr; // the element under the mouse, for mouseover/mouseout
    void fireMouseTransition(Element* from, Element* to, const MouseInfo& info);

    bool suppressChar_ = false; // the last keydown was cancelled: drop the character it types

    // A page's own <link rel=stylesheet> and <script src> fetches happen
    // concurrently on background threads (ResourceLoader) instead of
    // blocking the UI thread on them one at a time - see Engine.cpp's
    // pollResources for the full explanation. Each task records enough to
    // either apply immediately (inline) or look up its fetch once ready.
    struct StyleTask {
        size_t fetchIndex;
        int orderBase; // precomputed document-order band - see parseAndBuild
        bool applied = false;
    };
    struct ScriptTask {
        bool external;
        size_t fetchIndex;       // valid if external
        std::wstring inlineCode; // valid if !external
        std::shared_ptr<Node> el; // the <script> element, for document.currentScript
    };
    ResourceLoader styleLoader_;
    ResourceLoader scriptLoader_;
    std::vector<StyleTask> styleTasks_;
    std::vector<ScriptTask> scriptTasks_;
    size_t scriptCursor_ = 0; // next scriptTasks_ index to run, in document order
    void advanceScripts();    // runs scriptTasks_[scriptCursor_..] while each is ready, stopping at the first that isn't
    void pollResources();     // called every frame from render(): applies newly-ready stylesheets, advances scripts

    // Form state. The focused field's text lives in `editor` while editing
    // and is copied back to the element's "value" attribute after each edit.
    Element* focusedEl = nullptr;
    Element* focusedForm = nullptr;
    TextEditor editor;
    float inputScrollX = 0;
    // The focused field's value when it gained focus, and whether the user
    // has edited it since - "change" fires on blur only for a user edit
    // that left the value different (a script setting .value doesn't count).
    std::wstring focusValue_;
    bool userEdited_ = false;
    // After an edit to the focused field: fires "input" if its text changed
    // from `before`.
    void afterEdit(const std::wstring& before, const wchar_t* inputType, const std::wstring& data);
    bool hasSubmission = false;
    FormSubmission submission;

    // The <select> currently showing its dropdown, or nullptr. openSelectBox
    // is a snapshot of that select's own closed box (doc-space x/y/width/
    // height/fontSize), captured when opened - it anchors the dropdown's
    // position and its option rows' hit-testing, since the dropdown itself
    // isn't part of layoutRoot.boxes (see drawOpenSelect).
    Element* openSelect = nullptr;
    LayoutBox openSelectBox;

    // :hover state - the hovered element and its ancestors (see
    // CSS::HoverSet). `hoverSetLive` is false once a JS DOM mutation has
    // happened since the set was built, since its elements may then have
    // been freed: they're still safe to compare against, but not to
    // dereference (updateHover needs to, to test :hover rules against them).
    CSS::HoverSet hoverSet;
    bool hoverSetLive = false;
    double lastLayoutMs = 0; // how long the last doLayout() took

    void parseAndBuild(const std::wstring& html);
    void doLayout();

    // The topmost element whose box contains window point (x, y), or nullptr.
    Element* elementAt(int x, int y) const;
    const LayoutBox* controlAt(int x, int y) const;
    void focusInput(const LayoutBox& box);
    void syncValue();
    void queueSubmit(Element* form, Element* submitter);
    void drawControl(Renderer& renderer, const LayoutBox& box, int screenY, double timeSeconds);
    void drawOpenSelect(Renderer& renderer); // draws openSelect's dropdown rows, if one is open
    // How far a box is drawn below its layout position at the current
    // scroll position: scrollY for a position: fixed one (so it stays put
    // on screen), the current sticking distance for a sticky one, else 0.
    // Everything that paints or hit-tests a box adds it to `y`.
    int boxShift(const LayoutBox& b) const;
    int shiftFor(bool fixed, int sticky) const; // boxShift's rule, for a LayoutRoot::ElementRect too

    // --- Geometry for scripts (DOMBindingState::geometry) ---------------
    // A script reading geometry or computed style after changing the DOM
    // gets an answer that reflects the change: the page is laid out on the
    // spot, as browsers do. Unless layouts are slow (over 16 ms) and one
    // ended less than its own duration ago - so a script alternating
    // writes and reads can't spend more than about half the time laying
    // out; it then gets the last layout's geometry (styles are recomputed).
    void layoutForScript();
    unsigned layoutChangeCount_ = 0; // DOMBindingState::changeCount as of the last layout
    unsigned styleChangeCount_ = 0;  // ... as of the last time computed styles were current
    bool pageLaidOut_ = false; // the current page has been laid out at least once
    // The element's border box in viewport coordinates: its block box if
    // layout recorded one, else the union of its own boxes and its
    // children's (an inline element, a table row).
    bool scriptElementRect(Element* el, double& x, double& y, double& w, double& h);
    bool unionRect(const Element* el, int& x0, int& y0, int& x1, int& y1, int depth);
    // layoutRoot.boxes by element, rebuilt once per layout when needed.
    std::unordered_map<const Element*, std::vector<size_t>> boxesByElement_;
    unsigned layoutGeneration_ = 0, boxIndexGeneration_ = ~0u;
    // Whether page point (docX, docY) is inside the box and its clip.
    bool boxContains(const LayoutBox& b, int docX, int docY) const;
    // A box's box-shadows (outer ones; inset shadows aren't drawn).
    void paintShadows(Renderer& renderer, const LayoutBox& b, float screenY);
    // A box's background-image/gradient layers over the area (x, y, w, h) -
    // inside its border - clipped to `radii` if given.
    void paintBackgroundLayers(Renderer& renderer, const LayoutBox& b, float x, float y, float w, float h,
                               const float* radii);
    void paintControlBackground(Renderer& r, const LayoutBox& b, float x, float y, float w, float h);
};
