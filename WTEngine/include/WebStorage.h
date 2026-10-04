// WebStorage.h - the data behind localStorage and sessionStorage.
#pragma once
#include <string>
#include <map>

// One origin's key/value pairs. localStorage areas are loaded from disk the
// first time an origin uses them and written back after every change;
// sessionStorage areas live only in memory, for as long as the program runs
// (there is only one tab, so "the session" is the whole run).
class StorageArea {
public:
    // Null when there's no such key (getItem's result).
    const std::wstring* get(const std::wstring& key) const;
    // False, changing nothing, if the area would grow past kQuota.
    bool set(const std::wstring& key, const std::wstring& value);
    void remove(const std::wstring& key);
    void clear();
    size_t length() const { return items_.size(); }
    const std::map<std::wstring, std::wstring>& items() const { return items_; }

    // Like browsers: roughly 5 MB, counted in UTF-16 code units of keys plus values.
    static constexpr size_t kQuota = 5'000'000;

private:
    friend StorageArea& storageArea(const std::wstring& origin, bool session);
    std::map<std::wstring, std::wstring> items_;
    size_t used_ = 0;
    std::wstring file_; // where a localStorage area is saved; empty = memory only
    void save() const;
};

// The origin storage is partitioned by: "scheme://host[:port]" for an
// http(s) page (default ports dropped), "file://" for every page loaded
// from disk, or empty for anything else (e.g. the built-in start page).
std::wstring storageOrigin(const std::wstring& pageUrl);

// The area for `origin`. An empty origin gets a memory-only area even for
// localStorage, so nothing is written to disk for it.
StorageArea& storageArea(const std::wstring& origin, bool session);
