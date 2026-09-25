// PageLoader.cpp
#include "PageLoader.h"

void PageLoader::start(const std::wstring& url, const std::string* postBody, bool replace) {
    auto pending = std::make_shared<Pending>();
    pending->url = url;
    pending->replace = replace;
    current_ = pending; // drops (abandons) whatever was previously in flight
    startedAt_ = std::chrono::steady_clock::now();

    // The network thread only ever holds a weak reference: current_ is the
    // one owner, so once it moves on (a newer navigation, or cancel()) the
    // old request is skipped if it hadn't been sent yet (stillWanted), and
    // its result is dropped if it had. Either way nothing here can touch a
    // PageLoader - or App - that no longer exists.
    std::weak_ptr<Pending> weak = pending;
    FetchOptions options;
    options.priority = FetchPriority::Page;
    options.stillWanted = [weak] { return !weak.expired(); };
    fetchPageAsync(url, postBody, std::move(options), [weak](FetchResult res) {
        std::shared_ptr<Pending> p = weak.lock();
        if (!p) return;
        std::lock_guard<std::mutex> lock(p->mutex); // released before `p` (declared first)
        p->result = std::move(res);
        p->done = true;
    });
}

void PageLoader::cancel() {
    current_.reset();
}

bool PageLoader::poll(FetchResult& outResult, std::wstring& outUrl, bool& outReplace) {
    if (!current_) return false;

    // A local copy, not just current_ itself: this keeps `Pending` (and
    // its mutex) alive for this whole call regardless of what happens to
    // current_ below. Without it, current_.reset() at the end - if this
    // were the last reference - would destroy the mutex while `lock`
    // (below) still held it, and its destructor would then unlock an
    // already-destroyed mutex.
    std::shared_ptr<Pending> pending = current_;
    bool done;
    {
        std::lock_guard<std::mutex> lock(pending->mutex);
        done = pending->done;
        if (done) outResult = std::move(pending->result);
    } // lock released before touching current_ below (and before the timeout check,
      // which doesn't need it - `url`/`replace` are set once in start() and never
      // touched by the network thread, so they're safe to read unlocked)

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
