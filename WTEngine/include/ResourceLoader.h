// ResourceLoader.h
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <chrono>
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
    // abandon-in-place pattern as PageLoader). `dest` (script or style) and
    // `referrer` (the page) set the request headers - see FetchOptions.
    void start(std::vector<std::wstring> urls, FetchDest dest = FetchDest::Script,
               const std::wstring& referrer = L"");

    // Adds one more fetch to the current batch - a module an import names,
    // a <script> a page's script inserted - and returns its index.
    // `integrity`: the tag's integrity attribute, checked against the body
    // (FetchOptions::integrity).
    size_t add(std::wstring url, FetchDest dest, const std::wstring& referrer,
               const std::wstring& integrity = L"");

    size_t count() const { return slots_.size(); }

    // True once fetch `index` has finished (successfully or not).
    bool ready(size_t index) const;

    // Blocks until fetch `index` has finished or `timeout` passes; returns
    // ready(index). For the rare fetch that can't wait for a later frame
    // (a module import() names that wasn't fetched ahead - see Modules.cpp).
    bool wait(size_t index, std::chrono::milliseconds timeout) const;

    // Only valid once ready(index) (or wait) is true.
    const FetchResult& result(size_t index) const;

    // The URL fetch `index` was started with (e.g. for an error message).
    const std::wstring& url(size_t index) const { return urls_[index]; }

private:
    struct Slot {
        mutable std::mutex mutex;
        std::condition_variable arrived; // for wait()
        bool done = false;
        FetchResult result;
    };

    // The only owners of the current batch's slots - the network thread
    // holds weak_ptrs (see start()). Only ever touched from the thread that
    // calls start()/ready()/result() (Engine, i.e. the UI thread).
    std::vector<std::shared_ptr<Slot>> slots_;
    std::vector<std::wstring> urls_; // parallel to slots_
};
