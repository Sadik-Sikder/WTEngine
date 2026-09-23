// ResourceLoader.cpp
#include "ResourceLoader.h"

ResourceLoader::ResourceLoader() {
    for (int i = 0; i < kWorkerThreads; i++)
        workers_.emplace_back([this] { workerMain(); });
}

ResourceLoader::~ResourceLoader() {
    // Wake the pool so idle workers can see shuttingDown_ and exit; one
    // still fetching finishes that request first, then sees it on its next
    // loop iteration. Join before returning - workers capture `this` by
    // pointer, so none may still be running once this object is gone.
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        shuttingDown_ = true;
    }
    queueCv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
}

void ResourceLoader::start(std::vector<std::wstring> urls) {
    slots_.clear();
    slots_.reserve(urls.size());

    std::vector<QueueItem> items;
    items.reserve(urls.size());
    for (auto& url : urls) {
        auto slot = std::make_shared<Slot>();
        slots_.push_back(slot);
        items.push_back(QueueItem{ std::move(url), std::move(slot) });
    }

    std::lock_guard<std::mutex> lock(queueMutex_);
    // A superseded batch's own still-queued entries are left in place
    // rather than removed here - workerMain skips them cheaply (see there)
    // once a worker reaches them, without ever fetching. Simpler than
    // filtering the queue on every start(), and just as fast in practice
    // since a skip costs one use_count() check, not a network round-trip.
    for (auto& item : items) queue_.push_back(std::move(item));
    queueCv_.notify_all(); // wake every idle worker, not just one - a whole batch may have just arrived
}

bool ResourceLoader::ready(size_t index) const {
    std::lock_guard<std::mutex> lock(slots_[index]->mutex);
    return slots_[index]->done;
}

const FetchResult& ResourceLoader::result(size_t index) const {
    // No lock: only ever called right after ready(index) returned true, on
    // the same (UI) thread - that ready() call already acquired and
    // released this slot's mutex, which synchronizes-with the worker's
    // release after writing `result`, making it visible here without
    // needing to hold the lock again.
    return slots_[index]->result;
}

void ResourceLoader::workerMain() {
    for (;;) {
        QueueItem item;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this] { return shuttingDown_ || !queue_.empty(); });
            if (queue_.empty()) break; // shuttingDown_, and nothing left to drain
            item = std::move(queue_.front());
            queue_.pop_front();
        }

        // item.slot is referenced here, and - only if the batch it came
        // from is still the current one - by ResourceLoader::slots_ too.
        // use_count() == 1 means start() has since replaced slots_ with a
        // newer batch, so nothing can ever read this fetch's result -
        // skip the actual network I/O rather than spend a worker on it.
        // Best-effort (the batch could still be superseded a moment
        // later), which is fine: a fetch that slips through anyway just
        // finishes normally and is discarded, the same as it always was.
        if (item.slot.use_count() == 1) continue;

        FetchResult res = fetchPage(item.url);
        std::lock_guard<std::mutex> lock(item.slot->mutex);
        item.slot->result = std::move(res);
        item.slot->done = true;
    }
}
