// Fetcher.h
#pragma once
#include <string>
#include <utility>
#include <vector>

struct FetchResult {
    bool ok = false;
    std::wstring html;     // decoded page body (UTF-8 -> UTF-16)
    std::wstring finalUrl; // URL after redirects (base for relative links); empty for local files
    std::wstring error;
};

// Loads a page from an http(s):// URL, or from a local file path.
// With `postBody` (already form-encoded), sends it as an HTTP POST instead.
FetchResult fetchPage(const std::wstring& url, const std::string* postBody = nullptr);

// A general HTTP request, for JS fetch(): any method, extra headers, and a
// body. Unlike fetchPage, an HTTP error status (404, 500, ...) is still a
// completed response - `ok` means only that a response arrived at all.
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
HttpResponse fetchHttp(const HttpRequest& request);

// Fetches raw bytes from an http(s) URL or local file path, with no text
// decoding — used for binary resources like images. Returns false on failure.
bool fetchBytes(const std::wstring& url, std::vector<unsigned char>& outBytes);

// Resolves `href` against `baseUrl`. Returns an empty string for links that
// can't be navigated to (fragments, javascript:, mailto:, non-http bases).
std::wstring resolveUrl(const std::wstring& baseUrl, const std::wstring& href);

// application/x-www-form-urlencoded encoding of one name or value (as UTF-8).
std::string urlEncodeForm(const std::wstring& text);

// Replaces any query string and fragment on `url` with `?query`.
std::wstring withQuery(const std::wstring& url, const std::string& query);
