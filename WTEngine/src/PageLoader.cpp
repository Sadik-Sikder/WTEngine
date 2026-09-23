// PageLoader.cpp
#define NOMINMAX
#include "PageLoader.h"
#include <thread>
#include <windows.h>
#include <cstdio>

void PageLoader::start(const std::wstring& url, const std::string* postBody, bool replace) {
    auto pending = std::make_shared<Pending>();
    pending->url = url;
    pending->replace = replace;
    current_ = pending; // drops (abandons) whatever was previously in flight
    startedAt_ = std::chrono::steady_clock::now();

    bool hasBody = postBody != nullptr;
    std::string body = hasBody ? *postBody : std::string();

    // Detached: this thread's lifetime is however long fetchPage() takes,
    // independent of this PageLoader (or the App that owns it). It only
    // touches `pending`, kept alive by its own shared_ptr copy - safe to
    // write into even if `current_` has since moved on to a newer
    // navigation, or this PageLoader no longer exists at all.
    std::thread([pending, url, hasBody, body = std::move(body)]() {
        // COM must be initialized per-thread, not just once process-wide -
        // OpenGLRenderer's image-loader threads already do exactly this
        // (see its imageLoaderThreadMain) for the same reason: the main
        // thread is an STA (OpenGLRenderer's constructor calls
        // CoInitializeEx(..., COINIT_APARTMENTTHREADED)), and a thread
        // with no COM apartment of its own that ends up needing one -
        // which WinINet's PRECONFIG mode can, transitively, for
        // COM-based network services - gets cross-apartment-marshaled
        // through the main thread, which can stall it. Matching the main
        // thread's apartment type here avoids that marshaling entirely.
        bool comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

        FetchResult res = fetchPage(url, hasBody ? &body : nullptr);
        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->result = std::move(res);
            pending->done = true;
        } // lock released before CoUninitialize() - nothing below needs it held
        if (comInit) CoUninitialize();
    }).detach();
}

void PageLoader::cancel() {
    current_.reset();
}

bool PageLoader::poll(FetchResult& outResult, std::wstring& outUrl, bool& outReplace) {
    if (!current_) return false;

    // A local copy, not just current_ itself: this keeps `Pending` (and
    // its mutex) alive for this whole call regardless of what happens to
    // current_ below. Without it, current_.reset() at the end - if this
    // were the last reference, e.g. the background thread already
    // finished and dropped its own copy - would destroy the mutex while
    // `lock` (below) still held it, and its destructor would then unlock
    // an already-destroyed mutex.
    std::shared_ptr<Pending> pending = current_;
    bool done;
    {
        std::lock_guard<std::mutex> lock(pending->mutex);
        done = pending->done;
        if (done) outResult = std::move(pending->result);
    } // lock released before touching current_ below (and before the timeout check,
      // which doesn't need it - `url`/`replace` are set once in start() and never
      // touched by the background thread, so they're safe to read unlocked)

    if (!done && std::chrono::steady_clock::now() - startedAt_ < kTimeout) return false;

    if (!done) {
        outResult = FetchResult{};
        outResult.error = L"Timed out";
    }
    outUrl = pending->url;
    outReplace = pending->replace;
    current_.reset();
    return true;
}
