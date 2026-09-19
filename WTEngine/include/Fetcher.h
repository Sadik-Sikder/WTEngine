// Fetcher.h
#pragma once
#include <string>

struct FetchResult {
    bool ok = false;
    std::wstring html;     // decoded page body (UTF-8 -> UTF-16)
    std::wstring finalUrl; // URL after redirects (base for relative links); empty for local files
    std::wstring error;
};

// Loads a page from an http(s):// URL, or from a local file path.
FetchResult fetchPage(const std::wstring& url);

// Resolves `href` against `baseUrl`. Returns an empty string for links that
// can't be navigated to (fragments, javascript:, mailto:, non-http bases).
std::wstring resolveUrl(const std::wstring& baseUrl, const std::wstring& href);
