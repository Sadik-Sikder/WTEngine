// DevConsole.h
#pragma once
#include <string>
#include <vector>
#include "TextEditor.h"

class Renderer;

// ---------------------------------------------------------------------
// The console log: everything a page's JS says or throws, in order.

enum class LogLevel {
    Log,    // console.log/info/debug
    Warn,   // console.warn
    Error,  // console.error, and every uncaught error
    Input,  // a line typed into the console's input, echoed back
    Result, // what that line evaluated to
};

struct LogEntry {
    LogLevel level;
    std::wstring source; // where it came from: "console", "script", "click handler", "timer", "promise", ...
    std::wstring text;   // may contain newlines (e.g. an error's stack)
};

class ConsoleLog {
public:
    // Appends an entry, and mirrors it to stdout and the debugger's Output
    // window as "[source] text", as all JS output was before the panel existed.
    void add(LogLevel level, const std::wstring& source, const std::wstring& text);
    void clear(); // a new page starts with an empty console, as in browsers

    const std::vector<LogEntry>& entries() const { return entries_; }
    int errorCount() const { return errors_; }
    // Bumped on every change, so the panel can tell when to re-wrap its lines.
    unsigned version() const { return version_; }

private:
    static constexpr size_t kMaxEntries = 1000; // oldest dropped beyond this - a page logging in a loop can't grow it forever
    std::vector<LogEntry> entries_;
    int errors_ = 0;
    unsigned version_ = 0;
};

// The one log for the process. Only ever touched from the UI thread - all
// JS runs there - so it needs no lock.
ConsoleLog& consoleLog();

// ---------------------------------------------------------------------
// The panel: the log, docked at the bottom of the window (F12), with an
// input line that evaluates JS against the page.

class DevConsolePanel {
public:
    bool open() const { return open_; }
    void toggle() { open_ = !open_; if (!open_) focused_ = false; }
    void show() { open_ = true; }

    // How tall the panel is in a window `windowHeight` tall - 0 when closed.
    // The page's viewport shrinks by this much (Engine::setBottomInset).
    int height(int windowHeight) const;
    bool contains(int y, int windowHeight) const { return open_ && y >= windowHeight - height(windowHeight); }

    // Input focus: while focused, typed characters and edit keys go to the
    // console's input line instead of the address bar or the page.
    bool focused() const { return focused_; }
    void blur() { focused_ = false; }

    // A click inside the panel: its Clear/Close buttons, or the input line.
    void onClick(int x, int y, int windowWidth, int windowHeight, Renderer& renderer, double timeSeconds);
    void onScroll(double yoffset); // mouse wheel over the panel

    // Keyboard, while focused. onKey returns the code to evaluate when Enter
    // was pressed (and records it in the input history), else empty.
    void onChar(unsigned int codepoint) { input_.onChar(codepoint); historyPos_ = -1; }
    std::wstring onKey(int key);
    void insert(const std::wstring& s) { input_.insert(s); }
    void selectAll() { input_.selectAll(); }
    std::wstring selectedText() const { return input_.hasSelection() ? input_.selectedText() : L""; }

    void draw(Renderer& renderer, int windowWidth, int windowHeight, double timeSeconds);

private:
    // One wrapped display line of an entry.
    struct Line {
        size_t entry;
        std::wstring text;
        bool first; // the entry's first line - where its icon/source tag go
    };
    void rewrap(Renderer& renderer, float textWidth);

    bool open_ = false;
    bool focused_ = false;
    TextEditor input_;
    float inputScrollX_ = 0;

    std::vector<std::wstring> history_; // submitted inputs, oldest first
    int historyPos_ = -1;               // -1 = editing a new line, else an index into history_
    std::wstring draft_;                // the unsubmitted line, kept while browsing history

    int scrollLines_ = 0; // how many lines up from the bottom the view is; 0 = following new output
    std::vector<Line> lines_;
    unsigned wrappedVersion_ = ~0u;
    float wrappedWidth_ = -1;
};
