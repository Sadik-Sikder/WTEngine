// ResourceLoader.h
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include "Fetcher.h"

// Fetches a batch of URLs (every external <script src>/<link rel=stylesheet>
// a page references) concurrently on background threads, so Engine can run
// scripts and apply stylesheets as each one lands instead of blocking the
// UI thread on them one at a time, serially - see Engine::pollResources.
// Modeled on PageLoader's thread-safety pattern: a shared_ptr<Slot> per
// in-flight fetch, abandoned (not cancelled) if a new start() supersedes it
// - its thread keeps running to completion, but nothing reads the result
// once start() has replaced the slot list.
class ResourceLoader {
public:
    void start(std::vector<std::wstring> urls);

    size_t count() const { return slots_.size(); }

    // True once fetch `index` has finished (successfully or not).
    bool ready(size_t index) const;

    // Only valid once ready(index) is true.
    const FetchResult& result(size_t index) const;

private:
    struct Slot {
        mutable std::mutex mutex;
        bool done = false;
        FetchResult result;
    };
    std::vector<std::shared_ptr<Slot>> slots_;
};
