// Fetcher.cpp
#define NOMINMAX
#include "Fetcher.h"
#include <windows.h>
#include <wininet.h>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <vector>

#pragma comment(lib, "wininet.lib")

static bool isHttpUrl(const std::wstring& url) {
    return url.rfind(L"http://", 0) == 0 || url.rfind(L"https://", 0) == 0;
}

static std::wstring utf8ToWide(const std::string& bytes) {
    size_t start = 0;
    if (bytes.size() >= 3 && (unsigned char)bytes[0] == 0xEF &&
        (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) {
        start = 3; // skip UTF-8 BOM
    }
    int n = static_cast<int>(bytes.size() - start);
    if (n <= 0) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, bytes.data() + start, n, nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data() + start, n, out.data(), len);
    return out;
}

struct InetHandle {
    HINTERNET h;
    explicit InetHandle(HINTERNET handle) : h(handle) {}
    ~InetHandle() { if (h) InternetCloseHandle(h); }
    InetHandle(const InetHandle&) = delete;
    InetHandle& operator=(const InetHandle&) = delete;
};

static FetchResult fetchHttp(const std::wstring& url) {
    FetchResult res;

    InetHandle session(InternetOpenW(L"WTEngine/0.1", INTERNET_OPEN_TYPE_PRECONFIG,
                                     nullptr, nullptr, 0));
    if (!session.h) { res.error = L"InternetOpen failed"; return res; }

    InetHandle request(InternetOpenUrlW(
        session.h, url.c_str(), L"Accept: text/html\r\n", (DWORD)-1L,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
        INTERNET_FLAG_IGNORE_REDIRECT_TO_HTTP | INTERNET_FLAG_IGNORE_REDIRECT_TO_HTTPS, 0));
    if (!request.h) {
        res.error = L"Could not connect (error " + std::to_wstring(GetLastError()) + L")";
        return res;
    }

    DWORD status = 0, statusSize = sizeof(status);
    if (HttpQueryInfoW(request.h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                       &status, &statusSize, nullptr) && status >= 400) {
        res.error = L"HTTP " + std::to_wstring(status);
        return res;
    }

    std::string body;
    char buf[8192];
    DWORD got = 0;
    while (InternetReadFile(request.h, buf, sizeof(buf), &got) && got > 0) {
        body.append(buf, got);
    }

    wchar_t finalUrl[4096];
    DWORD finalSize = sizeof(finalUrl);
    if (InternetQueryOptionW(request.h, INTERNET_OPTION_URL, finalUrl, &finalSize)) {
        res.finalUrl = finalUrl;
    }
    else {
        res.finalUrl = url;
    }

    res.html = utf8ToWide(body);
    res.ok = true;
    return res;
}

static FetchResult readLocalFile(const std::wstring& path) {
    FetchResult res;
    std::ifstream in(path, std::ios::binary);
    if (!in) { res.error = L"Could not open file: " + path; return res; }
    std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    res.html = utf8ToWide(body);
    res.ok = true;
    return res;
}

FetchResult fetchPage(const std::wstring& url) {
    return isHttpUrl(url) ? fetchHttp(url) : readLocalFile(url);
}

std::wstring resolveUrl(const std::wstring& baseUrl, const std::wstring& href) {
    if (href.empty() || href[0] == L'#') return L"";

    // Skip non-navigable schemes (javascript:, mailto:, tel:, data:, ...)
    size_t colon = href.find(L':');
    if (colon != std::wstring::npos && !isHttpUrl(href)) {
        bool isScheme = colon > 0;
        for (size_t i = 0; i < colon && isScheme; i++) {
            if (!iswalnum(href[i]) && href[i] != L'+' && href[i] != L'-' && href[i] != L'.')
                isScheme = false;
        }
        if (isScheme) return L"";
    }

    if (isHttpUrl(href)) return href;
    if (!isHttpUrl(baseUrl)) return L""; // relative link inside a local file

    wchar_t out[4096];
    DWORD size = 4096;
    if (!InternetCombineUrlW(baseUrl.c_str(), href.c_str(), out, &size, ICU_BROWSER_MODE))
        return L"";
    return out;
}
