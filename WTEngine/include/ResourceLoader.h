// ResourceLoader.h
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include "Fetcher.h"

// Fetches a batch of URLs (every external <script src>/<link rel=stylesheet>
// a page references) concurrently, so Engine can run scripts and apply
// stylesheets as each one lands instead of blocking the UI thread on them
// one at a time, serially - see Engine::pollResources. The requests run on
// Fetcher's network thread at FetchPriority::Blocking, so when a host is
// busy they go ahead of the page's images.
class ResourceLoader {
public:
    // Starts fetching every URL in `urls`, replacing any previous batch:
    // the old batch's requests are skipped if they hadn't been sent yet,
    // and otherwise their results are simply never read (the same
    // abandon-in-place pattern as PageLoader).
    void start(std::vector<std::wstring> urls);

    size_t count() const { return slots_.size(); }

    // True once fetch `index` has finished (successfully or not).
    bool ready(size_t index) const;

    // Only valid once ready(index) is true.
    const FetchResult& result(size_t index) const;

    // The URL fetch `index` was started with (e.g. for an error message).
    const std::wstring& url(size_t index) const { return urls_[index]; }

private:
    struct Slot {
        mutable std::mutex mutex;
        bool done = false;
        FetchResult result;
    };

    // The only owners of the current batch's slots - the network thread
    // holds weak_ptrs (see start()). Only ever touched from the thread that
    // calls start()/ready()/result() (Engine, i.e. the UI thread).
    std::vector<std::shared_ptr<Slot>> slots_;
    std::vector<std::wstring> urls_; // parallel to slots_
};
