// CookieJar.cpp - see CookieJar.h.
#define NOMINMAX
#include "CookieJar.h"
#include <windows.h>
#include <wininet.h> // InternetTimeToSystemTimeA, for Expires dates
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

#pragma comment(lib, "wininet.lib")

static std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

static std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t");
    return s.substr(a, b - a + 1);
}

// The path part of a path-and-query: "/a/b?x=1#y" -> "/a/b".
static std::string pathOnly(const std::wstring& pathAndQuery) {
    std::string p = toUtf8(pathAndQuery);
    p = p.substr(0, p.find_first_of("?#"));
    return p.empty() || p[0] != '/' ? "/" : p;
}

// RFC 6265's default-path: the request path up to (not including) its last
// '/', or "/".
static std::string defaultPath(const std::string& path) {
    size_t slash = path.rfind('/');
    return slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash);
}

static bool domainMatches(const std::string& host, const std::string& domain) {
    if (host == domain) return true;
    return host.size() > domain.size() && host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
           host[host.size() - domain.size() - 1] == '.';
}

static bool pathMatches(const std::string& requestPath, const std::string& cookiePath) {
    if (requestPath == cookiePath) return true;
    if (requestPath.compare(0, cookiePath.size(), cookiePath) != 0) return false;
    return cookiePath.back() == '/' || requestPath[cookiePath.size()] == '/';
}

// An HTTP date ("Wed, 21 Oct 2026 07:28:00 GMT", or one of the older forms)
// as a time_t. False if it can't be read. A date at or before 1970 - what
// sites send to delete a cookie - comes back as 1, still in the past but
// never 0, which means "session cookie" here.
static bool parseHttpDate(const std::string& s, std::time_t& out) {
    SYSTEMTIME st{};
    if (!InternetTimeToSystemTimeA(s.c_str(), &st, 0)) return false;
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) return false;
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    // FILETIME counts 100ns ticks since 1601; time_t seconds since 1970.
    const unsigned long long kEpoch = 116444736000000000ULL;
    out = u.QuadPart <= kEpoch ? 1 : std::max<std::time_t>((std::time_t)((u.QuadPart - kEpoch) / 10000000ULL), 1);
    return true;
}

CookieJar& CookieJar::instance() {
    static CookieJar jar;
    return jar;
}

CookieJar::CookieJar() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring dir = std::wstring(buf) + L"\\WTEngine";
        CreateDirectoryW(dir.c_str(), nullptr);
        file_ = dir + L"\\Cookies.txt";
    }
    load();
}

CookieJar::~CookieJar() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dirty_) save();
}

void CookieJar::setCookie(const std::string& header, bool https, const std::wstring& hostW,
                          const std::wstring& pathAndQuery, bool fromScript) {
    std::string host = lower(toUtf8(hostW));
    std::string requestPath = pathOnly(pathAndQuery);
    std::time_t now = std::time(nullptr);

    // name=value, then ;-separated attributes.
    std::vector<std::string> parts;
    std::stringstream ss(header);
    for (std::string part; std::getline(ss, part, ';');) parts.push_back(trim(part));
    if (parts.empty()) return;
    size_t eq = parts[0].find('=');
    if (eq == std::string::npos) return; // a bare "value" with no name: ignored, as Chrome does
    Cookie c;
    c.name = trim(parts[0].substr(0, eq));
    c.value = trim(parts[0].substr(eq + 1));
    if (c.value.size() >= 2 && c.value.front() == '"' && c.value.back() == '"') c.value = c.value.substr(1, c.value.size() - 2);
    if (c.name.empty() || c.name.size() + c.value.size() > 4096) return;

    c.domain = host;
    c.path = defaultPath(requestPath);
    bool hasMaxAge = false;
    long long maxAge = 0;
    for (size_t i = 1; i < parts.size(); i++) {
        size_t e = parts[i].find('=');
        std::string key = lower(trim(parts[i].substr(0, e)));
        std::string val = e == std::string::npos ? "" : trim(parts[i].substr(e + 1));
        if (key == "domain" && !val.empty()) {
            std::string d = lower(val);
            if (d[0] == '.') d.erase(0, 1);
            // Must cover the host, and can't be a bare top-level domain
            // ("com") - the rough stand-in for a public-suffix check.
            if (d.find('.') == std::string::npos || !domainMatches(host, d)) return;
            c.domain = d;
            c.hostOnly = false;
        }
        else if (key == "path" && !val.empty() && val[0] == '/') c.path = val;
        else if (key == "max-age") {
            try { maxAge = std::stoll(val); hasMaxAge = true; } catch (...) {}
        }
        else if (key == "expires" && !hasMaxAge) parseHttpDate(val, c.expires); // unreadable: stays a session cookie
        else if (key == "secure") c.secure = true;
        else if (key == "httponly") c.httpOnly = true;
    }
    if (hasMaxAge) c.expires = maxAge <= 0 ? 1 : now + (std::time_t)maxAge; // Max-Age wins over Expires
    if (c.secure && !https) return;    // only a secure page may set a Secure cookie
    if (fromScript && c.httpOnly) return;
    // __Secure- and __Host- prefixes are promises the cookie keeps.
    if (c.name.rfind("__Secure-", 0) == 0 && !c.secure) return;
    if (c.name.rfind("__Host-", 0) == 0 && (!c.secure || !c.hostOnly || c.path != "/")) return;

    std::lock_guard<std::mutex> lock(mutex_);
    // A script can't shadow an HttpOnly cookie either - one of the same name
    // on an overlapping domain, at any path, would be sent alongside it.
    if (fromScript && std::any_of(cookies_.begin(), cookies_.end(), [&](const Cookie& o) {
            return o.httpOnly && o.name == c.name && (domainMatches(c.domain, o.domain) || domainMatches(o.domain, c.domain));
        }))
        return;
    auto same = std::find_if(cookies_.begin(), cookies_.end(), [&](const Cookie& o) {
        return o.name == c.name && o.domain == c.domain && o.path == c.path;
    });
    bool replacedPersistent = false;
    if (same != cookies_.end()) {
        if (fromScript && same->httpOnly) return; // a script can't overwrite an HttpOnly cookie
        c.created = same->created;                // replacing keeps its place in the order
        replacedPersistent = same->expires != 0;
        cookies_.erase(same);
    }
    else {
        c.created = ++counter_;
    }
    if (c.expires != 0 || replacedPersistent) dirty_ = true; // the saved file changes
    if (c.expires == 0 || c.expires > now) cookies_.push_back(std::move(c)); // an expiry in the past just deletes it

    // Saved at most every few seconds (a page can set dozens of cookies at
    // once), and on exit.
    if (dirty_ && now - lastSave_ >= 3) save();
}

std::string CookieJar::cookieHeader(bool https, const std::wstring& hostW, const std::wstring& pathAndQuery,
                                    bool forScript) {
    std::string host = lower(toUtf8(hostW));
    std::string path = pathOnly(pathAndQuery);
    std::lock_guard<std::mutex> lock(mutex_);
    removeExpired(std::time(nullptr));

    std::vector<const Cookie*> matching;
    for (const auto& c : cookies_) {
        if (c.hostOnly ? host != c.domain : !domainMatches(host, c.domain)) continue;
        if (!pathMatches(path, c.path)) continue;
        if (c.secure && !https) continue;
        if (forScript && c.httpOnly) continue;
        matching.push_back(&c);
    }
    // Longer paths first, then oldest first - the order RFC 6265 asks for.
    std::sort(matching.begin(), matching.end(), [](const Cookie* a, const Cookie* b) {
        if (a->path.size() != b->path.size()) return a->path.size() > b->path.size();
        return a->created < b->created;
    });
    std::string out;
    for (const Cookie* c : matching) {
        if (!out.empty()) out += "; ";
        out += c->name + "=" + c->value;
    }
    return out;
}

void CookieJar::removeExpired(std::time_t now) {
    auto gone = std::remove_if(cookies_.begin(), cookies_.end(),
                               [now](const Cookie& c) { return c.expires != 0 && c.expires <= now; });
    if (gone != cookies_.end()) {
        cookies_.erase(gone, cookies_.end());
        dirty_ = true;
    }
}

// Cookies.txt: one persistent cookie per line, tab-separated:
// domain, hostOnly, path, secure, httpOnly, expires, name, value.
// (Cookie names and values can't contain tabs or newlines.)
void CookieJar::save() {
    dirty_ = false;
    lastSave_ = std::time(nullptr);
    if (file_.empty()) return;
    std::wstring tmp = file_ + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        for (const auto& c : cookies_) {
            if (c.expires == 0) continue; // session cookies aren't kept
            out << c.domain << '\t' << c.hostOnly << '\t' << c.path << '\t' << c.secure << '\t'
                << c.httpOnly << '\t' << (long long)c.expires << '\t' << c.name << '\t' << c.value << '\n';
        }
        if (!out) return;
    }
    MoveFileExW(tmp.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void CookieJar::load() {
    if (file_.empty()) return;
    std::ifstream in(file_, std::ios::binary);
    std::time_t now = std::time(nullptr);
    for (std::string line; std::getline(in, line);) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        for (std::string field; std::getline(ss, field, '\t');) f.push_back(field);
        if (f.size() != 8) continue;
        Cookie c;
        c.domain = f[0];
        c.hostOnly = f[1] == "1";
        c.path = f[2];
        c.secure = f[3] == "1";
        c.httpOnly = f[4] == "1";
        try { c.expires = (std::time_t)std::stoll(f[5]); } catch (...) { continue; }
        c.name = f[6];
        c.value = f[7];
        if (c.expires <= now) continue;
        c.created = ++counter_;
        cookies_.push_back(std::move(c));
    }
}
