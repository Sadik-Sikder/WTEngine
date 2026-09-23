// ResourceLoader.cpp
#include "ResourceLoader.h"
#include <thread>

void ResourceLoader::start(std::vector<std::wstring> urls) {
    slots_.clear();
    slots_.reserve(urls.size());
    for (auto& url : urls) {
        auto slot = std::make_shared<Slot>();
        slots_.push_back(slot);
        std::thread([slot, url]() {
            FetchResult res = fetchPage(url);
            std::lock_guard<std::mutex> lock(slot->mutex);
            slot->result = std::move(res);
            slot->done = true;
        }).detach();
    }
}

bool ResourceLoader::ready(size_t index) const {
    std::lock_guard<std::mutex> lock(slots_[index]->mutex);
    return slots_[index]->done;
}

const FetchResult& ResourceLoader::result(size_t index) const {
    // No lock: only ever called right after ready(index) returned true, on
    // the same (UI) thread - that ready() call already acquired and
    // released this slot's mutex, which synchronizes-with the background
    // thread's release after writing `result`, making it visible here
    // without needing to hold the lock again.
    return slots_[index]->result;
}
