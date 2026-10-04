// WebStorage.cpp - localStorage/sessionStorage data and its on-disk form.
#define NOMINMAX
#include "WebStorage.h"
#include <windows.h>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <memory>
#include <vector>

static std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

static std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

// --- On-disk format -----------------------------------------------------
// One UTF-8 text file per origin, one item per line: key, a tab, value.
// Backslash, tab, CR and LF inside keys/values are escaped so each item
// stays on its own line.

static std::wstring escapeField(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        switch (c) {
        case L'\\': out += L"\\\\"; break;
        case L'\t': out += L"\\t"; break;
        case L'\n': out += L"\\n"; break;
        case L'\r': out += L"\\r"; break;
        default: out.push_back(c);
        }
    }
    return out;
}

static std::wstring unescapeField(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != L'\\' || i + 1 == s.size()) { out.push_back(s[i]); continue; }
        wchar_t n = s[++i];
        out.push_back(n == L't' ? L'\t' : n == L'n' ? L'\n' : n == L'r' ? L'\r' : n);
    }
    return out;
}

// %LOCALAPPDATA%\WTEngine\Local Storage, created on first use.
static std::wstring storageDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    std::wstring dir = std::wstring(buf) + L"\\WTEngine";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\Local Storage";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// "https://example.com:8080" -> "https___example.com_8080.txt"
static std::wstring fileNameFor(const std::wstring& origin) {
    std::wstring name;
    for (wchar_t c : origin) name.push_back(iswalnum(c) || c == L'.' || c == L'-' ? c : L'_');
    return name + L".txt";
}

void StorageArea::save() const {
    if (file_.empty()) return;
    std::wstring text;
    for (auto& [k, v] : items_) text += escapeField(k) + L"\t" + escapeField(v) + L"\n";
    std::string bytes = toUtf8(text);

    // Write a temporary file, then swap it in, so a crash mid-write can't
    // leave a half-written file behind.
    std::wstring tmp = file_ + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(bytes.data(), (std::streamsize)bytes.size());
        if (!out) return;
    }
    MoveFileExW(tmp.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING);
}

const std::wstring* StorageArea::get(const std::wstring& key) const {
    auto it = items_.find(key);
    return it == items_.end() ? nullptr : &it->second;
}

bool StorageArea::set(const std::wstring& key, const std::wstring& value) {
    auto it = items_.find(key);
    size_t old = it == items_.end() ? 0 : key.size() + it->second.size();
    size_t now = used_ - old + key.size() + value.size();
    if (now > kQuota) return false;
    if (it != items_.end() && it->second == value) return true; // nothing to save
    items_[key] = value;
    used_ = now;
    save();
    return true;
}

void StorageArea::remove(const std::wstring& key) {
    auto it = items_.find(key);
    if (it == items_.end()) return;
    used_ -= key.size() + it->second.size();
    items_.erase(it);
    save();
}

void StorageArea::clear() {
    if (items_.empty()) return;
    items_.clear();
    used_ = 0;
    save();
}

std::wstring storageOrigin(const std::wstring& url) {
    for (const wchar_t* scheme : { L"http", L"https" }) {
        std::wstring prefix = std::wstring(scheme) + L"://";
        if (url.size() <= prefix.size() || _wcsnicmp(url.c_str(), prefix.c_str(), prefix.size()) != 0) continue;

        size_t start = prefix.size();
        size_t end = url.find_first_of(L"/?#", start);
        std::wstring authority = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        size_t at = authority.rfind(L'@');
        if (at != std::wstring::npos) authority.erase(0, at + 1); // user:password@ isn't part of the origin
        for (auto& c : authority) c = (wchar_t)towlower(c);
        std::wstring defaultPort = scheme == std::wstring(L"http") ? L":80" : L":443";
        if (authority.size() > defaultPort.size() &&
            authority.compare(authority.size() - defaultPort.size(), defaultPort.size(), defaultPort) == 0)
            authority.erase(authority.size() - defaultPort.size());
        return authority.empty() ? L"" : std::wstring(scheme) + L"://" + authority;
    }
    // A page loaded from disk: C:\..., \\server\share\..., or file:...
    if (_wcsnicmp(url.c_str(), L"file:", 5) == 0 || (url.size() >= 2 && url[1] == L':') ||
        url.rfind(L"\\\\", 0) == 0)
        return L"file://";
    return L"";
}

StorageArea& storageArea(const std::wstring& origin, bool session) {
    static std::map<std::wstring, std::unique_ptr<StorageArea>> local, sessions;
    auto& areas = session ? sessions : local;
    auto& slot = areas[origin];
    if (slot) return *slot;

    slot = std::make_unique<StorageArea>();
    if (session || origin.empty()) return *slot;

    std::wstring dir = storageDir();
    if (dir.empty()) return *slot; // nowhere to save; works for this run only
    slot->file_ = dir + L"\\" + fileNameFor(origin);

    std::ifstream in(slot->file_, std::ios::binary);
    if (!in) return *slot; // nothing saved yet
    std::stringstream ss;
    ss << in.rdbuf();
    std::wstring text = fromUtf8(ss.str());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t eol = text.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = text.substr(pos, eol - pos);
        pos = eol + 1;
        size_t tab = line.find(L'\t');
        if (tab == std::wstring::npos) continue;
        std::wstring k = unescapeField(line.substr(0, tab)), v = unescapeField(line.substr(tab + 1));
        slot->used_ += k.size() + v.size();
        slot->items_[std::move(k)] = std::move(v);
    }
    return *slot;
}
