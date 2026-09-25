// PageLoader.h
#pragma once
#include <string>
#include <memory>
#include <mutex>
#include <chrono>
#include "Fetcher.h"

// Fetches the top-level page (address bar, link clicks, form submits, JS
// location changes - i.e. what main.cpp's navigate() starts) without
// blocking the UI thread, so a slow or unreachable server doesn't freeze
// the application. The request itself runs on Fetcher's network thread
// (fetchPageAsync); this class tracks which navigation is current and
// hands its result to the main loop.
class PageLoader {
public:
    // Starts fetching `url` (as a POST if `postBody` is given - copied
    // immediately, so the caller's copy need not outlive this call).
    // Abandons whatever was previously in flight: it's skipped if it
    // hadn't been sent yet, and otherwise its result is simply never read
    // - the same "a new load wins" behavior a real browser has for
    // navigating away mid-load. `replace` is carried through unchanged to
    // poll()'s result.
    void start(const std::wstring& url, const std::string* postBody, bool replace);

    // Abandons whatever's in flight without starting a new one - for a
    // navigation that bypasses this loader entirely (Back/Forward, which
    // redisplays cached HTML instantly), so a fetch that was already in
    // flight can't clobber the page the user just navigated to once it
    // eventually finishes.
    void cancel();

    // Called once per frame. Returns true (and fills every out-param) the
    // moment the in-flight fetch finishes - or once it's been running
    // longer than kTimeout, in which case outResult.ok is false and
    // outResult.error names a timeout. An application-level give-up on top
    // of Fetcher's own connect/exchange deadlines: the underlying request
    // may keep running a little longer, harmlessly - its eventual result
    // is simply never read once current_ is dropped here. Returns false
    // while nothing is in flight, or while still within kTimeout with no
    // result yet.
    bool poll(FetchResult& outResult, std::wstring& outUrl, bool& outReplace);

private:
    static constexpr std::chrono::seconds kTimeout{ 8 };

    // current_ is the only owner; the network thread holds a weak_ptr (see
    // start()), so dropping current_ is all it takes to abandon a request.
    struct Pending {
        std::mutex mutex;
        bool done = false;
        FetchResult result;
        std::wstring url;
        bool replace = false;
    };
    std::shared_ptr<Pending> current_;
    std::chrono::steady_clock::time_point startedAt_;
};
