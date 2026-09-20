// Fetcher.h
#pragma once
#include <string>
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
