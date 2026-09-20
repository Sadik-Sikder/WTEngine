// Fetcher.cpp
#define NOMINMAX
#include "Fetcher.h"
#include <windows.h>
#include <wininet.h>
#include <wincrypt.h>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <vector>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "crypt32.lib")

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

static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

struct InetHandle {
    HINTERNET h;
    explicit InetHandle(HINTERNET handle) : h(handle) {}
    ~InetHandle() { if (h) InternetCloseHandle(h); }
    InetHandle(const InetHandle&) = delete;
    InetHandle& operator=(const InetHandle&) = delete;
};

// Checks the status, reads the body and records the final URL from an open
// request handle (works for both InternetOpenUrl and HttpSendRequest).
static void readResponse(HINTERNET request, const std::wstring& requestedUrl, FetchResult& res) {
    DWORD status = 0, statusSize = sizeof(status);
    if (HttpQueryInfoW(request, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                       &status, &statusSize, nullptr) && status >= 400) {
        res.error = L"HTTP " + std::to_wstring(status);
        return;
    }

    std::string body;
    char buf[8192];
    DWORD got = 0;
    while (InternetReadFile(request, buf, sizeof(buf), &got) && got > 0) {
        body.append(buf, got);
    }

    wchar_t finalUrl[4096];
    DWORD finalSize = sizeof(finalUrl);
    if (InternetQueryOptionW(request, INTERNET_OPTION_URL, finalUrl, &finalSize)) {
        res.finalUrl = finalUrl;
    }
    else {
        res.finalUrl = requestedUrl;
    }

    res.html = utf8ToWide(body);
    res.ok = true;
}

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

    readResponse(request.h, url, res);
    return res;
}

static FetchResult postHttp(const std::wstring& url, const std::string& body) {
    FetchResult res;

    // Split the URL into host / port / path for InternetConnect.
    wchar_t host[256], path[2048], extra[2048];
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;   uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;    uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!InternetCrackUrlW(url.c_str(), 0, 0, &uc)) { res.error = L"Bad URL"; return res; }

    InetHandle session(InternetOpenW(L"WTEngine/0.1", INTERNET_OPEN_TYPE_PRECONFIG,
                                     nullptr, nullptr, 0));
    if (!session.h) { res.error = L"InternetOpen failed"; return res; }

    InetHandle connection(InternetConnectW(session.h, host, uc.nPort, nullptr, nullptr,
                                           INTERNET_SERVICE_HTTP, 0, 0));
    if (!connection.h) {
        res.error = L"Could not connect (error " + std::to_wstring(GetLastError()) + L")";
        return res;
    }

    DWORD flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE;
    if (uc.nScheme == INTERNET_SCHEME_HTTPS) flags |= INTERNET_FLAG_SECURE;

    std::wstring target = std::wstring(path, uc.dwUrlPathLength) +
                          std::wstring(extra, uc.dwExtraInfoLength);
    if (target.empty()) target = L"/";

    InetHandle request(HttpOpenRequestW(connection.h, L"POST", target.c_str(), nullptr,
                                        nullptr, nullptr, flags, 0));
    if (!request.h) { res.error = L"Could not open request"; return res; }

    static const wchar_t headers[] =
        L"Content-Type: application/x-www-form-urlencoded\r\nAccept: text/html\r\n";
    if (!HttpSendRequestW(request.h, headers, (DWORD)-1L,
                          (LPVOID)body.data(), (DWORD)body.size())) {
        res.error = L"Request failed (error " + std::to_wstring(GetLastError()) + L")";
        return res;
    }

    readResponse(request.h, url, res);
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

FetchResult fetchPage(const std::wstring& url, const std::string* postBody) {
    if (!isHttpUrl(url)) return readLocalFile(url); // a POST body makes no sense for a file
    return postBody ? postHttp(url, *postBody) : fetchHttp(url);
}

// Decodes a "data:[<mediatype>];base64,<data>" URI's payload. Only the
// base64 form is supported (what every image-embedding tool produces);
// anything else (a bare percent-encoded data: URI) is rejected.
static bool decodeDataUri(const std::wstring& uri, std::vector<unsigned char>& outBytes) {
    size_t comma = uri.find(L',');
    if (comma == std::wstring::npos) return false;

    std::wstring meta = uri.substr(5, comma - 5); // between "data:" and ','
    if (meta.find(L";base64") == std::wstring::npos) return false;

    std::string b64 = wideToUtf8(uri.substr(comma + 1));

    DWORD outLen = 0;
    if (!CryptStringToBinaryA(b64.c_str(), (DWORD)b64.size(), CRYPT_STRING_BASE64,
                              nullptr, &outLen, nullptr, nullptr)) {
        return false;
    }
    outBytes.resize(outLen);
    if (!CryptStringToBinaryA(b64.c_str(), (DWORD)b64.size(), CRYPT_STRING_BASE64,
                              outBytes.data(), &outLen, nullptr, nullptr)) {
        return false;
    }
    outBytes.resize(outLen);
    return true;
}

bool fetchBytes(const std::wstring& url, std::vector<unsigned char>& outBytes) {
    outBytes.clear();

    if (url.rfind(L"data:", 0) == 0) return decodeDataUri(url, outBytes);

    if (!isHttpUrl(url)) {
        std::ifstream in(url, std::ios::binary);
        if (!in) return false;
        outBytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    }

    InetHandle session(InternetOpenW(L"WTEngine/0.1", INTERNET_OPEN_TYPE_PRECONFIG,
                                     nullptr, nullptr, 0));
    if (!session.h) return false;

    InetHandle request(InternetOpenUrlW(
        session.h, url.c_str(), nullptr, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
        INTERNET_FLAG_IGNORE_REDIRECT_TO_HTTP | INTERNET_FLAG_IGNORE_REDIRECT_TO_HTTPS, 0));
    if (!request.h) return false;

    DWORD status = 0, statusSize = sizeof(status);
    if (HttpQueryInfoW(request.h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                       &status, &statusSize, nullptr) && status >= 400) {
        return false;
    }

    char buf[8192];
    DWORD got = 0;
    while (InternetReadFile(request.h, buf, sizeof(buf), &got) && got > 0) {
        outBytes.insert(outBytes.end(), buf, buf + got);
    }
    return true;
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

std::string urlEncodeForm(const std::wstring& text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : wideToUtf8(text)) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '*') {
            out.push_back((char)c);
        }
        else if (c == ' ') {
            out.push_back('+');
        }
        else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

std::wstring withQuery(const std::wstring& url, const std::string& query) {
    std::wstring base = url.substr(0, url.find_first_of(L"?#"));
    return base + L"?" + std::wstring(query.begin(), query.end()); // query is ASCII
}
