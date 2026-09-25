// PageHistory.h
#pragma once
#include <string>
#include <utility>
#include <vector>

// One visited page. The HTML is kept so Back/Forward can redisplay it without
// hitting the network (and without re-sending a POSTed form).
struct HistoryEntry {
    std::wstring url;
    std::wstring html;
    int scrollY = 0; // where the user had scrolled to
};

// Browser-style history made of two stacks around the current page:
//
//   back_  [oldest ... previous]   current_   forward_ [... next]
//
// Visiting a new page pushes the current one onto back_ and empties forward_.
// back() moves current_ onto forward_ and pops back_ into its place;
// forward() does the reverse.
class PageHistory {
public:
    static constexpr size_t kMaxBack = 50; // oldest pages are dropped beyond this

    void visit(HistoryEntry entry) {
        if (hasCurrent_) {
            back_.push_back(std::move(current_));
            if (back_.size() > kMaxBack) back_.erase(back_.begin());
        }
        current_ = std::move(entry);
        hasCurrent_ = true;
        forward_.clear(); // a new page invalidates the "future"
    }

    // Like visit(), but overwrites the current page in place instead of
    // pushing it onto back_ - for a navigation that shouldn't leave a
    // Back-button stop of its own, e.g. JS location.replace() (real
    // browsers give that exact method its name for exactly this reason).
    void replaceCurrent(HistoryEntry entry) {
        current_ = std::move(entry);
        hasCurrent_ = true;
        forward_.clear(); // same as visit(): a new page invalidates the "future"
    }

    // Swaps in a freshly re-fetched copy of the current page (the Reload
    // button / F5). Unlike replaceCurrent(), both Back *and* Forward are
    // kept: reloading is re-showing the same history stop, not moving on
    // from it.
    void reloadCurrent(HistoryEntry entry) {
        current_ = std::move(entry);
        hasCurrent_ = true;
    }

    bool canGoBack() const { return !back_.empty(); }
    bool canGoForward() const { return !forward_.empty(); }

    // Both return the page to show, or nullptr if there is nowhere to go.
    const HistoryEntry* back() {
        if (back_.empty()) return nullptr;
        forward_.push_back(std::move(current_));
        current_ = std::move(back_.back());
        back_.pop_back();
        return &current_;
    }

    const HistoryEntry* forward() {
        if (forward_.empty()) return nullptr;
        back_.push_back(std::move(current_));
        current_ = std::move(forward_.back());
        forward_.pop_back();
        return &current_;
    }

    // The page currently shown, or nullptr before the first visit.
    const HistoryEntry* current() const { return hasCurrent_ ? &current_ : nullptr; }

    // Remember the scroll position of the page being left.
    void saveScroll(int scrollY) { if (hasCurrent_) current_.scrollY = scrollY; }

private:
    std::vector<HistoryEntry> back_;
    HistoryEntry current_;
    bool hasCurrent_ = false;
    std::vector<HistoryEntry> forward_;
};
