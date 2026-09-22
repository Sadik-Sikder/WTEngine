// PageLoader.h
#pragma once
#include <string>
#include <memory>
#include <mutex>
#include <chrono>
#include "Fetcher.h"

// Fetches a page on a background thread so a slow or unreachable server
// doesn't freeze the whole application - the same problem OpenGLRenderer's
// image loader threads already solve for <img>, applied here to page
// navigation itself: the highest-impact place a single blocking fetch can
// freeze the UI for a long time (observed: ~30 seconds navigating to a
// real site whose IPv6 route is black-holed - WinINet tries it first and
// won't fall back to the working IPv4 address until the OS's own TCP
// connect timeout elapses).
//
// Scope: only the top-level page fetch (address bar, link clicks, form
// submits, JS location changes - i.e. what main.cpp's navigate() starts)
// goes through this. A page's own <script src> and <link rel=stylesheet>
// fetches (Engine::runScripts / Engine::parseAndBuild) still happen
// synchronously once that page's HTML is already in hand - a smaller,
// separate blocking window (same host, already known reachable) not
// addressed here.
class PageLoader {
public:
    // Starts fetching `url` in the background (as a POST if `postBody` is
    // given - copied immediately, so the caller's copy need not outlive
    // this call). Abandons whatever was previously in flight: its result,
    // if it later arrives, is simply never read - the same "a new load
    // wins" behavior a real browser has for navigating away mid-load.
    // `replace` is carried through unchanged to poll()'s result.
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
    // outResult.error names a timeout. This is an application-level give-
    // up, not a WinINet setting: shortening WinINet's own timeout options
    // (INTERNET_OPTION_CONNECT_TIMEOUT etc.) turned out not to reliably
    // bound how long a stuck connection attempt actually takes (observed:
    // a black-holed IPv6 route can still run the OS's own ~20-30s TCP
    // connect timeout regardless of those options), so this bounds it
    // from the caller's side instead - the underlying fetch may keep
    // running in its own thread for a while longer, same as any other
    // abandoned/superseded fetch (see start()): harmless, its eventual
    // result is simply never read once current_ is dropped here.
    // Returns false while nothing is in flight, or while still within
    // kTimeout with no result yet.
    bool poll(FetchResult& outResult, std::wstring& outUrl, bool& outReplace);

private:
    static constexpr std::chrono::seconds kTimeout{ 8 };

    // Shared with the background thread via shared_ptr rather than owned
    // directly: start()/cancel() just drop this PageLoader's own
    // reference to it, so an abandoned fetch's thread can safely finish
    // and write its result into a slot nothing reads anymore, instead of
    // into a dangling PageLoader (or App, which owns one and may itself
    // be gone by then, e.g. the window closing while a fetch is stuck).
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
