// ResourceLoader.h
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <deque>
#include "Fetcher.h"

// Fetches a batch of URLs (every external <script src>/<link rel=stylesheet>
// a page references) on a small, fixed pool of background threads, so
// Engine can run scripts and apply stylesheets as each one lands instead of
// blocking the UI thread on them one at a time, serially - see
// Engine::pollResources. A bounded pool - not one OS thread per URL - for
// the same reason OpenGLRenderer's image loader already uses one
// (kImageLoaderThreads): a pathological page could reference hundreds of
// resources, and spawning hundreds of threads at once for it is real,
// unbounded overhead a fixed-size pool avoids entirely.
class ResourceLoader {
public:
    ResourceLoader();
    ~ResourceLoader();
    ResourceLoader(const ResourceLoader&) = delete;
    ResourceLoader& operator=(const ResourceLoader&) = delete;

    // Queues every URL in `urls` for the pool to fetch. Safe to call again
    // before a previous batch finishes - a queued-but-not-yet-started entry
    // from the old batch is cheaply skipped once a worker reaches it (see
    // workerMain), and one already being fetched finishes normally, but its
    // result is just never read once nothing points at it anymore (the same
    // abandon-in-place pattern as PageLoader).
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
    struct QueueItem {
        std::wstring url;
        std::shared_ptr<Slot> slot;
    };
    void workerMain();

    // Only ever touched from the thread that calls start()/ready()/result()
    // (Engine, i.e. the UI thread) - never accessed by a worker, so it
    // needs no lock of its own.
    std::vector<std::shared_ptr<Slot>> slots_;

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<QueueItem> queue_;
    bool shuttingDown_ = false;
    std::vector<std::thread> workers_;

    static constexpr int kWorkerThreads = 4; // matches OpenGLRenderer::kImageLoaderThreads
};
