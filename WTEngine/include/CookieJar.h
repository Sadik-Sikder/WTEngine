// CookieJar.h - HTTP cookies: stored from Set-Cookie, sent back as Cookie.
#pragma once
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

// One process-wide jar, shared by every request (the network thread) and
// document.cookie (the UI thread) - so every method locks. Persistent
// cookies (with Expires/Max-Age) are saved to
// %LOCALAPPDATA%\WTEngine\Cookies.txt and survive restarts; session
// cookies last until the program exits.
//
// Simplifications: no public-suffix list (a Domain= is only refused when it
// has no dot, like "com", or doesn't cover the host), and SameSite isn't
// enforced - cookies go with cross-site requests too.
class CookieJar {
public:
    static CookieJar& instance();

    // The Cookie header for a request to that URL: "a=1; b=2", or empty.
    // `forScript` leaves out HttpOnly cookies (document.cookie can't see them).
    std::string cookieHeader(bool https, const std::wstring& host, const std::wstring& path,
                             bool forScript = false);

    // Stores one Set-Cookie header value received from that URL (or, with
    // `fromScript`, one document.cookie assignment - which can't set or
    // overwrite an HttpOnly cookie).
    void setCookie(const std::string& header, bool https, const std::wstring& host,
                   const std::wstring& path, bool fromScript = false);

    ~CookieJar(); // saves persistent cookies

private:
    struct Cookie {
        std::string name, value;
        std::string domain;     // lowercase, no leading dot
        bool hostOnly = true;   // no Domain=: only that exact host
        std::string path = "/";
        std::time_t expires = 0; // 0 = session cookie
        bool secure = false;
        bool httpOnly = false;
        unsigned long long created = 0; // orders cookies with equal paths
    };

    CookieJar();
    void load();
    void save();
    void removeExpired(std::time_t now);

    std::mutex mutex_;
    std::vector<Cookie> cookies_;
    unsigned long long counter_ = 0;
    std::wstring file_;
    bool dirty_ = false;
    std::time_t lastSave_ = 0;
};
