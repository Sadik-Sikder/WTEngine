// Fetcher.h
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

// All network I/O goes through one shared network thread, which drives
// every socket through an I/O completion port (IOCP) - no thread per
// request, and no caller ever blocks on the network (see Fetcher.cpp).
// Each fetch*Async function returns immediately; its `onDone` runs exactly
// once, *on that network thread* (not the caller's), when the request
// completes or fails - so it must only hand the result off (e.g. into a
// mutex-guarded slot the caller polls), never touch UI/JS/GL state
// directly. `onDone` is skipped only if the process is exiting.

// When several requests to the same host are waiting for one of its
// connections (at most 6 at once, like browsers), lower values go first.
enum class FetchPriority {
    Page = 0,     // the top-level page
    Blocking = 1, // scripts and stylesheets - the page can't finish without them
    Fetch = 2,    // JS fetch()
    Image = 3,
};

struct FetchOptions {
    FetchPriority priority = FetchPriority::Fetch;
    // If set, called (on the network thread - so it must be thread-safe,
    // e.g. checking a weak_ptr) right before the request is sent. Returning
    // false skips it; `onDone` then gets a failure ("Cancelled"). Lets a
    // superseded page load or script batch cost nothing if it hadn't
    // started yet.
    std::function<bool()> stillWanted;
};

struct FetchResult {
    bool ok = false;
    std::wstring html;     // decoded page body (UTF-8 -> UTF-16)
    std::wstring finalUrl; // URL after redirects (base for relative links); empty for local files
    std::wstring error;
};

// Loads a page, script or stylesheet as text from an http(s):// URL, or
// from a local file path. With `postBody` (already form-encoded), sends it
// as an HTTP POST instead. An HTTP error status (4xx/5xx) is a failure.
void fetchPageAsync(std::wstring url, const std::string* postBody, FetchOptions options,
                    std::function<void(FetchResult)> onDone);

// Fetches raw bytes - for binary resources like images - from an http(s)
// URL, a local file path, or a data:...;base64 URI. `ok` false on failure.
void fetchBytesAsync(std::wstring url, FetchOptions options,
                     std::function<void(bool ok, std::vector<unsigned char> bytes)> onDone);

// A general HTTP request, for JS fetch(): any method, extra headers, and a
// body. Unlike fetchPageAsync, an HTTP error status (404, 500, ...) is
// still a completed response - `ok` means only that a response arrived.
struct HttpRequest {
    std::wstring url;       // absolute http(s) URL, or a local file path (read as a GET)
    std::string method = "GET";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};
struct HttpResponse {
    bool ok = false;         // false = network error (see `error`); true even for a 404
    int status = 0;
    std::string body;        // raw bytes, already decompressed
    std::string contentType;
    std::wstring finalUrl;   // after redirects
    std::wstring error;
};
void fetchHttpAsync(HttpRequest request, std::function<void(HttpResponse)> onDone);

// Resolves `href` against `baseUrl`. Returns an empty string for links that
// can't be navigated to (fragments, javascript:, mailto:, non-http bases).
std::wstring resolveUrl(const std::wstring& baseUrl, const std::wstring& href);

// application/x-www-form-urlencoded encoding of one name or value (as UTF-8).
std::string urlEncodeForm(const std::wstring& text);

// Replaces any query string and fragment on `url` with `?query`.
std::wstring withQuery(const std::wstring& url, const std::string& query);
