// ResourceLoader.cpp
#include "ResourceLoader.h"

void ResourceLoader::start(std::vector<std::wstring> urls, FetchDest dest, const std::wstring& referrer) {
    // Dropping the old slots is what abandons the previous batch.
    slots_.clear();
    urls_.clear();
    slots_.reserve(urls.size());
    for (auto& url : urls) add(std::move(url), dest, referrer);
}

size_t ResourceLoader::add(std::wstring url, FetchDest dest, const std::wstring& referrer) {
    auto slot = std::make_shared<Slot>();
    slots_.push_back(slot);
    urls_.push_back(url);

    std::weak_ptr<Slot> weak = slot;
    FetchOptions options;
    options.priority = FetchPriority::Blocking;
    options.dest = dest;
    options.referrer = referrer;
    options.stillWanted = [weak] { return !weak.expired(); };
    fetchPageAsync(std::move(url), nullptr, std::move(options), [weak](FetchResult res) {
        std::shared_ptr<Slot> s = weak.lock();
        if (!s) return;
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->result = std::move(res);
            s->done = true;
        }
        s->arrived.notify_all();
    });
    return slots_.size() - 1;
}

bool ResourceLoader::ready(size_t index) const {
    std::lock_guard<std::mutex> lock(slots_[index]->mutex);
    return slots_[index]->done;
}

bool ResourceLoader::wait(size_t index, std::chrono::milliseconds timeout) const {
    std::unique_lock<std::mutex> lock(slots_[index]->mutex);
    return slots_[index]->arrived.wait_for(lock, timeout, [&] { return slots_[index]->done; });
}

const FetchResult& ResourceLoader::result(size_t index) const {
    // No lock: only ever called right after ready(index) returned true, on
    // the same (UI) thread - that ready() call already acquired and
    // released this slot's mutex, which synchronizes-with the network
    // thread's release after writing `result`, making it visible here
    // without needing to hold the lock again.
    return slots_[index]->result;
}
