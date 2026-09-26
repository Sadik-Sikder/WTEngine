// DevConsole.cpp
#define NOMINMAX
#include "DevConsole.h"
#include "Renderer.h"
#include <GLFW/glfw3.h> // key codes only
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

// ---------------------------------------------------------------------
// ConsoleLog

void ConsoleLog::add(LogLevel level, const std::wstring& source, const std::wstring& text) {
    if (entries_.size() >= kMaxEntries) {
        if (entries_.front().level == LogLevel::Error) errors_--;
        entries_.erase(entries_.begin());
    }
    entries_.push_back({ level, source, text });
    if (level == LogLevel::Error) errors_++;
    version_++;

    std::wstring line = L"[" + source + L"] " + text + L"\n";
    wprintf(L"%ls", line.c_str());
    OutputDebugStringW(line.c_str());
}

void ConsoleLog::clear() {
    entries_.clear();
    errors_ = 0;
    version_++;
}

ConsoleLog& consoleLog() {
    static ConsoleLog log;
    return log;
}

// ---------------------------------------------------------------------
// DevConsolePanel

namespace {
    const int kHeaderH = 26;
    const int kInputH = 28;
    const int kLineH = 18;
    const float kFont = 13;
    const int kPad = 8;
    const int kIconW = 18; // icon column at the left of each entry

    // Glyphs spelled as numbers so the source file encoding can't garble them.
    const wchar_t kWarnIcon[] = { 0x26A0, 0 };  // warning sign
    const wchar_t kErrorIcon[] = { 0x2716, 0 }; // heavy X
    const wchar_t kInIcon[] = { 0x203A, 0 };    // single right-pointing angle quote
    const wchar_t kOutIcon[] = { 0x2039, 0 };   // single left-pointing angle quote
    const wchar_t kClose[] = { 0x2715, 0 };

    const Color kBg{ 1, 1, 1, 1 };
    const Color kHeaderBg{ 0.95f, 0.95f, 0.96f, 1 };
    const Color kBorder{ 0.78f, 0.80f, 0.82f, 1 };
    const Color kInk{ 0.10f, 0.10f, 0.10f, 1 };
    const Color kDim{ 0.50f, 0.50f, 0.53f, 1 };
    const Color kWarnBg{ 1.0f, 0.98f, 0.88f, 1 };
    const Color kWarnInk{ 0.45f, 0.30f, 0.0f, 1 };
    const Color kErrorBg{ 1.0f, 0.94f, 0.94f, 1 };
    const Color kErrorInk{ 0.70f, 0.05f, 0.10f, 1 };
    const Color kInputInk{ 0.10f, 0.25f, 0.70f, 1 };
    const Color kSelection{ 0.68f, 0.82f, 1.0f, 1 };

    // What goes before an entry's text on its first line: its source,
    // except for plain console output, where it would only be noise.
    std::wstring prefixFor(const LogEntry& e) {
        if (e.level == LogLevel::Input || e.level == LogLevel::Result || e.source == L"console") return L"";
        return L"[" + e.source + L"] ";
    }

    // The longest prefix of `s` that fits in `width`, broken after a space
    // when there's one in the second half of it (so words mostly stay
    // whole), otherwise at a character. Always at least one character.
    size_t fitChars(const std::wstring& s, float width, Renderer& r) {
        if (r.measureText(s, kFont) <= width) return s.size();
        size_t lo = 1, hi = s.size();
        while (lo < hi) { // largest n with measure(s[0..n)) <= width
            size_t mid = (lo + hi + 1) / 2;
            if (r.measureText(s.substr(0, mid), kFont) <= width) lo = mid;
            else hi = mid - 1;
        }
        size_t space = s.rfind(L' ', lo);
        if (space != std::wstring::npos && space > lo / 2 && space < lo) return space + 1;
        return lo;
    }
}

int DevConsolePanel::height(int windowHeight) const {
    if (!open_) return 0;
    int h = std::clamp(windowHeight * 2 / 5, 140, 360);
    return std::max(std::min(h, windowHeight - 100), kHeaderH + kInputH + kLineH); // always leave some page visible
}

void DevConsolePanel::rewrap(Renderer& r, float width) {
    lines_.clear();
    const auto& entries = consoleLog().entries();
    for (size_t i = 0; i < entries.size(); i++) {
        std::wstring all = prefixFor(entries[i]) + entries[i].text;
        bool first = true;
        size_t start = 0;
        do {
            size_t nl = all.find(L'\n', start);
            std::wstring piece = all.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
            if (!piece.empty() && piece.back() == L'\r') piece.pop_back();
            do { // wrap one source line into as many display lines as it needs
                size_t n = piece.empty() ? 0 : fitChars(piece, width, r);
                lines_.push_back({ i, piece.substr(0, n), first });
                first = false;
                piece.erase(0, n);
            } while (!piece.empty());
            start = nl == std::wstring::npos ? std::wstring::npos : nl + 1;
        } while (start != std::wstring::npos);
    }
    wrappedVersion_ = consoleLog().version();
    wrappedWidth_ = width;
}

void DevConsolePanel::onScroll(double yoffset) {
    scrollLines_ = std::max(0, scrollLines_ + (int)std::lround(yoffset * 3)); // clamped against the content in draw()
}

void DevConsolePanel::onClick(int x, int y, int w, int windowHeight, Renderer& r, double now) {
    int top = windowHeight - height(windowHeight);
    if (y < top + kHeaderH) {
        if (x >= w - 30) { toggle(); return; }                             // close
        if (x >= w - 90 && x < w - 36) { consoleLog().clear(); return; }    // clear
        return;
    }
    // Anywhere else focuses the input; on the input line, also place the caret.
    focused_ = true;
    if (y >= windowHeight - kInputH) {
        bool isDouble = input_.registerClick(x, now);
        float localX = x - (kPad + kIconW) + inputScrollX_;
        if (isDouble) input_.selectWordAt(localX, r, kFont);
        else input_.placeCaretAt(localX, r, kFont);
    }
}

std::wstring DevConsolePanel::onKey(int key) {
    if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) {
        std::wstring code = input_.text();
        if (code.empty()) return L"";
        if (history_.empty() || history_.back() != code) history_.push_back(code);
        input_.setText(L"");
        historyPos_ = -1;
        scrollLines_ = 0; // show the result
        return code;
    }
    if (key == GLFW_KEY_UP) {
        if (history_.empty()) return L"";
        if (historyPos_ == -1) { draft_ = input_.text(); historyPos_ = (int)history_.size() - 1; }
        else if (historyPos_ > 0) historyPos_--;
        input_.setText(history_[historyPos_]);
        return L"";
    }
    if (key == GLFW_KEY_DOWN) {
        if (historyPos_ == -1) return L"";
        if (historyPos_ + 1 < (int)history_.size()) input_.setText(history_[++historyPos_]);
        else { historyPos_ = -1; input_.setText(draft_); }
        return L"";
    }
    if (key == GLFW_KEY_ESCAPE) { focused_ = false; return L""; }
    if (input_.onEditKey(key)) historyPos_ = -1;
    return L"";
}

void DevConsolePanel::draw(Renderer& r, int w, int windowHeight, double t) {
    if (!open_) return;
    const int h = height(windowHeight);
    const int top = windowHeight - h;
    const float textX = (float)(kPad + kIconW);
    const float textW = std::max(w - textX - kPad, 40.0f);

    // Frame and header
    r.drawRect(0, (float)top, (float)w, (float)h, kBg);
    r.drawRect(0, (float)top, (float)w, 1, kBorder);
    r.drawRect(0, (float)top + 1, (float)w, (float)kHeaderH - 1, kHeaderBg);
    r.drawRect(0, (float)(top + kHeaderH), (float)w, 1, kBorder);
    r.drawText((float)kPad, (float)top + 5, L"Console", kFont, kInk, true);

    int warns = 0;
    for (const auto& e : consoleLog().entries()) if (e.level == LogLevel::Warn) warns++;
    std::wstring counts;
    if (consoleLog().errorCount()) counts += std::to_wstring(consoleLog().errorCount()) + (consoleLog().errorCount() == 1 ? L" error" : L" errors");
    if (warns) counts += (counts.empty() ? L"" : L", ") + std::to_wstring(warns) + (warns == 1 ? L" warning" : L" warnings");
    if (!counts.empty()) {
        float titleW = r.measureText(L"Console", kFont, true);
        r.drawText(kPad + titleW + 12, (float)top + 5, counts, kFont,
                   consoleLog().errorCount() ? kErrorInk : kWarnInk);
    }
    r.drawText((float)(w - 86), (float)top + 5, L"Clear", kFont, kDim);
    r.drawText((float)(w - 22), (float)top + 5, kClose, kFont, kDim);

    // Log lines, newest at the bottom
    if (wrappedVersion_ != consoleLog().version() || wrappedWidth_ != textW) rewrap(r, textW);
    const int areaTop = top + kHeaderH + 1;
    const int areaH = h - kHeaderH - 1 - kInputH;
    const int visible = std::max(areaH / kLineH, 1);
    const int total = (int)lines_.size();
    scrollLines_ = std::clamp(scrollLines_, 0, std::max(total - visible, 0));
    const int last = total - scrollLines_;          // one past the last line shown
    const int firstShown = std::max(last - visible, 0);

    r.setClip(0, (float)areaTop, (float)w, (float)areaH);
    const auto& entries = consoleLog().entries();
    int y = areaTop + areaH - (last - firstShown) * kLineH; // bottom-aligned
    for (int i = firstShown; i < last; i++, y += kLineH) {
        const Line& line = lines_[i];
        const LogEntry& e = entries[line.entry];
        Color ink = kInk;
        const wchar_t* icon = nullptr;
        switch (e.level) {
        case LogLevel::Warn:   r.drawRect(0, (float)y, (float)w, kLineH, kWarnBg);  ink = kWarnInk;  icon = kWarnIcon; break;
        case LogLevel::Error:  r.drawRect(0, (float)y, (float)w, kLineH, kErrorBg); ink = kErrorInk; icon = kErrorIcon; break;
        case LogLevel::Input:  ink = kInputInk; icon = kInIcon; break;
        case LogLevel::Result: ink = kDim;      icon = kOutIcon; break;
        case LogLevel::Log: break;
        }
        if (line.first) {
            if (i > 0 && lines_[i - 1].entry != line.entry) r.drawRect(0, (float)y, (float)w, 1, kHeaderBg); // separator
            if (icon) r.drawText((float)kPad, (float)y + 2, icon, kFont - 1, ink);
        }
        r.drawText(textX, (float)y + 2, line.text, kFont, ink);
    }
    if (total == 0) r.drawText(textX, (float)areaTop + 6, L"No messages. Type JavaScript below and press Enter.", kFont, kDim);
    r.clearClip();

    // Input line
    const int inY = windowHeight - kInputH;
    r.drawRect(0, (float)inY, (float)w, 1, kBorder);
    r.drawText((float)kPad, (float)inY + 6, kInIcon, kFont, focused_ ? kInputInk : kDim, true);
    const std::wstring& text = input_.text();
    const float viewW = textW;
    float caretX = r.measureText(text.substr(0, input_.cursor()), kFont);
    if (!focused_) inputScrollX_ = 0;
    else {
        if (caretX - inputScrollX_ > viewW) inputScrollX_ = caretX - viewW;
        if (caretX - inputScrollX_ < 0) inputScrollX_ = caretX;
    }
    r.setClip(textX, (float)inY + 1, viewW, (float)kInputH - 1);
    const float tx = textX - inputScrollX_;
    if (text.empty() && !focused_) {
        r.drawText(textX, (float)inY + 6, L"Click here to run JavaScript on this page", kFont, kDim);
    } else {
        if (input_.hasSelection()) {
            float x0 = r.measureText(text.substr(0, input_.selLow()), kFont);
            float x1 = r.measureText(text.substr(0, input_.selHigh()), kFont);
            r.drawRect(tx + x0, (float)inY + 5, x1 - x0, (float)kInputH - 10, kSelection);
        }
        r.drawText(tx, (float)inY + 6, text, kFont, kInk);
        if (focused_ && !input_.hasSelection() && std::fmod(t, 1.0) < 0.6)
            r.drawRect(tx + caretX, (float)inY + 6, 1.5f, (float)kInputH - 12, kInk);
    }
    r.clearClip();
}
