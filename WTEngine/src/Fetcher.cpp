// Fetcher.cpp
//
// HTTP(S) fetching via Boost.Beast/Asio instead of WinINet. Why: WinINet
// resolves a dual-stack host and tries addresses *sequentially* - on a
// host whose IPv6 route is unreachable (common on some networks/VMs),
// it waits out the OS's own full TCP connect timeout (empirically
// ~20-30s) before ever trying the working IPv4 address, and did so no
// matter what INTERNET_OPTION_CONNECT_TIMEOUT was set to. beast::tcp_stream
// (below) tries each resolved address in turn with a short *per-attempt*
// deadline (expires_after), so a bad address is abandoned in a couple of
// seconds instead of tens of seconds - the same practical effect as
// RFC 8305 Happy Eyeballs (which real browsers and .NET's HttpClient
// both implement, and which is why they don't have this problem),
// without needing to race connections in parallel.
//
// URL parsing/combining (InternetCrackUrlW/InternetCombineUrlW) is pure
// string manipulation - no networking - so it's kept as-is; only the
// actual socket I/O (InternetOpenUrlW/HttpSendRequestW/InternetReadFile)
// is replaced.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN // otherwise windows.h pulls in the old winsock.h, which conflicts with Asio's winsock2.h
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 // required by Asio on Windows
#endif
#include "Fetcher.h"
#include <windows.h>
#include <wininet.h>   // InternetCrackUrlW/InternetCombineUrlW only - see above
#include <wincrypt.h>  // CryptStringToBinaryA (data: URIs) and the ROOT cert store (CertOpenSystemStoreW etc.)
#include <cctype>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <chrono>

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/x509.h>

#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "crypt32.lib")

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;

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

// ---------------------------------------------------------------------
// URL decomposition - InternetCrackUrlW/InternetCombineUrlW are pure
// string utilities (no networking), reused as-is from the old WinINet
// implementation rather than writing a new URL parser.

struct UrlParts {
    std::wstring host;
    std::wstring pathAndQuery; // always starts with '/'
    INTERNET_PORT port = 0;
    bool https = false;
};

static bool crackUrl(const std::wstring& url, UrlParts& out) {
    wchar_t host[256]{}, path[2048]{}, extra[2048]{};
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;   uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;    uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!InternetCrackUrlW(url.c_str(), 0, 0, &uc)) return false;

    out.host = host;
    out.pathAndQuery = std::wstring(path) + std::wstring(extra);
    if (out.pathAndQuery.empty()) out.pathAndQuery = L"/";
    out.port = uc.nPort;
    out.https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

// ---------------------------------------------------------------------
// TLS: load the live Windows "ROOT" certificate store into an OpenSSL
// trust store. OpenSSL has no built-in notion of Windows' certificate
// store (SSL_CTX_set_default_verify_paths only looks for Unix-style CA
// bundle files), so without this, every HTTPS connection would fail
// verification - and disabling verification instead is not an option,
// that would accept literally any certificate. Loading the *live* store
// (rather than a bundled snapshot) also means this stays in sync with
// whatever CAs Windows itself trusts or distrusts over time.

static void loadWindowsRootCertificates(ssl::context& ctx) {
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (!store) return;

    X509_STORE* x509Store = X509_STORE_new();
    PCCERT_CONTEXT certCtx = nullptr;
    while ((certCtx = CertEnumCertificatesInStore(store, certCtx)) != nullptr) {
        const unsigned char* encoded = certCtx->pbCertEncoded;
        X509* x509 = d2i_X509(nullptr, &encoded, certCtx->cbCertEncoded);
        if (x509) {
            X509_STORE_add_cert(x509Store, x509);
            X509_free(x509);
        }
    }
    CertCloseStore(store, 0);

    SSL_CTX_set_cert_store(ctx.native_handle(), x509Store); // takes ownership of x509Store
}

// One shared context for the process: safe (and the recommended pattern)
// to reuse an ssl::context across concurrent connections from multiple
// threads - only *creating* new ssl streams from it concurrently, never
// mutating it after this one-time setup. Avoids re-walking the whole
// Windows cert store on every single request.
static ssl::context& sharedSslContext() {
    static ssl::context ctx = [] {
        ssl::context c(ssl::context::tlsv12_client);
        c.set_verify_mode(ssl::verify_peer); // verify the server's cert chains to a trusted CA
        loadWindowsRootCertificates(c);
        return c;
    }();
    return ctx;
}

// ---------------------------------------------------------------------
// The raw result of one HTTP round-trip - defined here (ahead of where
// the rest of this file's comments put "the actual network I/O") because
// the connection-pooling code just below needs it as a parameter type.

struct RawResponse {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string location; // Location header, for the redirect loop below
    std::wstring finalUrl;
    std::wstring error;
};

// ---------------------------------------------------------------------
// Connection pooling: reusing an already-open, already-TLS-handshaked
// connection for a second request to the same host skips resolve/connect/
// handshake entirely - measured (see HOW_IT_WORKS.md) at roughly 200ms of
// pure overhead per connection, dominated by the TLS handshake alone, and
// a page very commonly sends several requests to the same host (its own
// origin, or a shared CDN for stylesheets/scripts/images).
//
// Each pooled connection owns its net::io_context: an Asio/Beast stream
// is permanently bound to the io_context it was constructed with (an
// executor can't be rebound afterward), so reusing a stream across
// separate sendOneRequest() calls - potentially from different background
// threads, since PageLoader/ResourceLoader/the image loader all call into
// this file concurrently - means keeping its io_context alive alongside
// it, not just the stream itself.

struct PooledPlainConnection {
    net::io_context ioc;
    beast::tcp_stream stream;
    std::chrono::steady_clock::time_point idleSince;
    std::chrono::seconds serverTimeout;
    explicit PooledPlainConnection(std::chrono::seconds timeout) : stream(ioc), serverTimeout(timeout) {}
};

struct PooledSslConnection {
    net::io_context ioc;
    beast::ssl_stream<beast::tcp_stream> stream;
    std::chrono::steady_clock::time_point idleSince;
    std::chrono::seconds serverTimeout;
    PooledSslConnection(ssl::context& sslCtx, std::chrono::seconds timeout)
        : stream(ioc, sslCtx), serverTimeout(timeout) {}
};

// How long to keep an idle connection when the response didn't say (no
// Keep-Alive: timeout=N) - conservative, safely under most servers' actual
// defaults (commonly 5-15s), so guessing wrong in the "kept it too long"
// direction stays rare. The write/read failure path in sendOneRequest
// covers it when a guess is wrong anyway, so this only needs to be a
// reasonable default, not a guarantee.
static constexpr std::chrono::seconds kDefaultPoolTimeout{ 4 };
// Bounds memory/socket use if connections are returned faster than
// they're reused - a low ceiling is fine since the goal is avoiding
// *repeat* handshakes to a host being fetched from concurrently right
// now, not maintaining a large persistent cache of every host ever visited.
static constexpr size_t kMaxPooledPerHost = 4;

// Parses a "Keep-Alive: timeout=N, max=N" response header. `max` (the
// number of requests the server will still allow on this connection)
// isn't tracked here - running into it is just another way a pooled
// connection turns out to be unusable, already handled reactively by the
// write/read failure fallback in sendOneRequest, so there's nothing extra
// to do with knowing it in advance.
static std::optional<int> parseKeepAliveTimeout(const std::string& value) {
    std::istringstream ss(value);
    std::string token;
    while (std::getline(ss, token, ',')) {
        size_t eq = token.find('=');
        if (eq == std::string::npos) continue;
        size_t start = token.find_first_not_of(" \t");
        if (start == std::string::npos || start >= eq) continue;
        if (token.compare(start, eq - start, "timeout") != 0) continue;
        try { return std::stoi(token.substr(eq + 1)); }
        catch (...) { return std::nullopt; }
    }
    return std::nullopt;
}

// HTTP/1.1 (the only version this file ever sends - see sendOneRequest)
// defaults to keeping the connection open; only an explicit "close" token
// says otherwise.
static bool responseWantsClose(const http::response<http::string_body>& res) {
    auto it = res.find(http::field::connection);
    if (it == res.end()) return false;
    std::string val(it->value());
    for (auto& c : val) c = (char)std::tolower((unsigned char)c);
    return val.find("close") != std::string::npos;
}

// One shared pool for the process, keyed by "host:port" - separate maps
// for plain and TLS connections since they're different C++ types, not
// because the concept differs. Thread-safe: every method locks the same
// mutex, matching PageLoader/ResourceLoader's established pattern of one
// mutex per shared structure rather than anything more elaborate.
class ConnectionPool {
public:
    std::unique_ptr<PooledPlainConnection> takePlain(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        return take(plain_, key);
    }
    void givePlain(const std::string& key, std::unique_ptr<PooledPlainConnection> conn) {
        std::lock_guard<std::mutex> lock(mutex_);
        give(plain_, key, std::move(conn));
    }
    std::unique_ptr<PooledSslConnection> takeSsl(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        return take(ssl_, key);
    }
    void giveSsl(const std::string& key, std::unique_ptr<PooledSslConnection> conn) {
        std::lock_guard<std::mutex> lock(mutex_);
        give(ssl_, key, std::move(conn));
    }

private:
    template <class Conn>
    static std::unique_ptr<Conn> take(std::unordered_map<std::string, std::vector<std::unique_ptr<Conn>>>& map,
                                       const std::string& key) {
        auto it = map.find(key);
        if (it == map.end()) return nullptr;
        auto& bucket = it->second;
        auto now = std::chrono::steady_clock::now();
        while (!bucket.empty()) {
            auto conn = std::move(bucket.back());
            bucket.pop_back();
            if (now - conn->idleSince <= conn->serverTimeout) return conn;
            // else: past its advertised lifetime, likely already closed by
            // the server - discard without even trying it, and check the
            // next one (there's rarely more than one, but a burst of
            // concurrent requests can leave a few queued up).
        }
        return nullptr;
    }
    template <class Conn>
    static void give(std::unordered_map<std::string, std::vector<std::unique_ptr<Conn>>>& map,
                      const std::string& key, std::unique_ptr<Conn> conn) {
        auto& bucket = map[key];
        if (bucket.size() >= kMaxPooledPerHost) return; // conn (and its connection) is closed by falling out of scope here
        conn->idleSince = std::chrono::steady_clock::now();
        bucket.push_back(std::move(conn));
    }

    std::mutex mutex_;
    std::unordered_map<std::string, std::vector<std::unique_ptr<PooledPlainConnection>>> plain_;
    std::unordered_map<std::string, std::vector<std::unique_ptr<PooledSslConnection>>> ssl_;
};

static ConnectionPool& sharedConnectionPool() {
    static ConnectionPool pool;
    return pool;
}

// Sends `req` on an already-connected `stream` (freshly opened, or reused
// from the pool) and fills `out` from the response - shared between the
// plain-TCP and TLS paths in sendOneRequest below via the template
// parameter, since both stream types support the same write/read API;
// only connecting and tearing down differ between them, which stays with
// their respective callers. Throws on failure (a stale pooled connection,
// most commonly), same as the write/read calls it wraps - the caller
// decides what that means.
template <class Stream>
static void writeAndRead(Stream& stream, const http::request<http::string_body>& req, RawResponse& out,
                          bool& keepAlive, std::chrono::seconds& keepAliveTimeout) {
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::write(stream, req);
    http::read(stream, buffer, res);

    out.status = res.result_int();
    out.body = std::move(res.body());
    auto loc = res.find(http::field::location);
    if (loc != res.end()) out.location.assign(loc->value());
    out.ok = true;

    keepAlive = !responseWantsClose(res);
    keepAliveTimeout = kDefaultPoolTimeout;
    auto ka = res.find(http::field::keep_alive);
    if (ka != res.end()) {
        if (auto t = parseKeepAliveTimeout(std::string(ka->value()))) keepAliveTimeout = std::chrono::seconds(*t);
    }
}

// ---------------------------------------------------------------------
// The actual network I/O.

// Connects to the resolved address(es) for host:port, trying each in turn
// with a short per-attempt deadline - see the file header comment for why
// this (not a longer overall timeout) is what actually avoids a long
// stall on a host with an unreachable address in its DNS results.
static void connectStream(beast::tcp_stream& stream, const std::string& host, const std::string& port) {
    tcp::resolver resolver(stream.get_executor());
    auto const results = resolver.resolve(host, port);
    stream.expires_after(std::chrono::seconds(3));
    stream.connect(results);
}

// Opens a fresh HTTPS connection (SNI + hostname verification + connect +
// handshake), sends `req` on it, and either returns it to the pool or
// shuts it down, depending on what the response said. Split out of
// sendOneRequest below only because it's used from two places there (the
// normal path, and the fallback after a pooled connection turns out to be
// stale) - not a general-purpose helper otherwise.
static void sendFreshHttps(const std::string& host, const std::string& port, const std::string& poolKey,
                            const http::request<http::string_body>& req, RawResponse& out) {
    auto fresh = std::make_unique<PooledSslConnection>(sharedSslContext(), kDefaultPoolTimeout);

    // SNI: without this, many hosts (anything relying on virtual hosting
    // by hostname, i.e. most of the web) return the wrong certificate or
    // reject the handshake outright.
    if (!SSL_set_tlsext_host_name(fresh->stream.native_handle(), host.c_str())) {
        beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
        throw beast::system_error(ec);
    }
    // verify_peer (set on the shared context) alone only checks the
    // certificate chains to a trusted CA - not that it's actually FOR
    // this host. Without this, any validly-CA-signed certificate for any
    // site would be accepted, defeating the point.
    fresh->stream.set_verify_callback(ssl::host_name_verification(host));

    connectStream(beast::get_lowest_layer(fresh->stream), host, port);
    beast::get_lowest_layer(fresh->stream).expires_after(std::chrono::seconds(10));
    fresh->stream.handshake(ssl::stream_base::client);

    bool keepAlive; std::chrono::seconds timeout;
    writeAndRead(fresh->stream, req, out, keepAlive, timeout);

    if (keepAlive) {
        fresh->serverTimeout = timeout;
        sharedConnectionPool().giveSsl(poolKey, std::move(fresh));
    } else {
        beast::error_code ec;
        beast::get_lowest_layer(fresh->stream).expires_after(std::chrono::seconds(3));
        fresh->stream.shutdown(ec); // the peer closing first is routine, not an error - ignore ec
    }
}

// Same shape as sendFreshHttps, for plain (non-TLS) HTTP.
static void sendFreshPlain(const std::string& host, const std::string& port, const std::string& poolKey,
                            const http::request<http::string_body>& req, RawResponse& out) {
    auto fresh = std::make_unique<PooledPlainConnection>(kDefaultPoolTimeout);
    connectStream(fresh->stream, host, port);
    fresh->stream.expires_after(std::chrono::seconds(10));

    bool keepAlive; std::chrono::seconds timeout;
    writeAndRead(fresh->stream, req, out, keepAlive, timeout);

    if (keepAlive) {
        fresh->serverTimeout = timeout;
        sharedConnectionPool().givePlain(poolKey, std::move(fresh));
    } else {
        beast::error_code ec;
        fresh->stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    }
}

// Sends one request (no redirect following - fetchRaw below loops) and
// returns the raw response. Tries a pooled connection for this host
// first, if one's available - just write+read, skipping resolve/connect/
// (for HTTPS) handshake entirely. If that throws (the connection had
// already been silently closed by the server while it sat idle - the one
// failure mode pooling can't avoid, only recover from), the exception is
// swallowed here and a fresh connection is opened instead, transparently;
// only a fresh connection's own failure is reported to the caller.
// Exceptions from anywhere in the Beast/Asio call chain (resolve/connect/
// handshake/write/read failures, including a connect that exhausted every
// resolved address) are caught and reported via RawResponse::error,
// matching how the rest of this file signals failure (a bool/empty-error
// pattern, not exceptions, at the API boundary fetchPage/fetchBytes expose).
static RawResponse sendOneRequest(const UrlParts& parts, http::verb method, const std::string& body,
                                   const char* accept) {
    RawResponse out;
    std::string host = wideToUtf8(parts.host);
    std::string port = std::to_string(parts.port);
    std::string target = wideToUtf8(parts.pathAndQuery);
    std::string poolKey = host + ":" + port;

    http::request<http::string_body> req{method, target, 11};
    req.set(http::field::host, host);
    req.set(http::field::user_agent, "WTEngine/0.1");
    if (accept) req.set(http::field::accept, accept); // "text/html" for a page, unset for an image
    req.set(http::field::connection, "keep-alive"); // was "close" - see ConnectionPool above
    if (!body.empty()) {
        req.set(http::field::content_type, "application/x-www-form-urlencoded");
        req.body() = body;
        req.prepare_payload();
    }

    try {
        if (parts.https) {
            if (auto pooled = sharedConnectionPool().takeSsl(poolKey)) {
                try {
                    bool keepAlive; std::chrono::seconds timeout;
                    beast::get_lowest_layer(pooled->stream).expires_after(std::chrono::seconds(10));
                    writeAndRead(pooled->stream, req, out, keepAlive, timeout);
                    if (keepAlive) {
                        pooled->serverTimeout = timeout;
                        sharedConnectionPool().giveSsl(poolKey, std::move(pooled));
                    }
                    return out; // succeeded via the pooled connection - done
                }
                catch (std::exception const&) {
                    // Stale - `pooled` is destroyed here (closing whatever's
                    // left of it), and a fresh connection is tried below.
                }
            }
            sendFreshHttps(host, port, poolKey, req, out);
        } else {
            if (auto pooled = sharedConnectionPool().takePlain(poolKey)) {
                try {
                    bool keepAlive; std::chrono::seconds timeout;
                    pooled->stream.expires_after(std::chrono::seconds(10));
                    writeAndRead(pooled->stream, req, out, keepAlive, timeout);
                    if (keepAlive) {
                        pooled->serverTimeout = timeout;
                        sharedConnectionPool().givePlain(poolKey, std::move(pooled));
                    }
                    return out;
                }
                catch (std::exception const&) {
                    // fall through to a fresh connection, same as above
                }
            }
            sendFreshPlain(host, port, poolKey, req, out);
        }
    }
    catch (std::exception const& e) {
        out.error = utf8ToWide(e.what());
    }
    return out;
}

// Sends a GET or POST, following up to 10 redirects (always as a GET
// after the first hop - matches how real browsers treat 301/302/303; a
// stricter implementation would preserve the method for 307/308, not
// done here to keep this simple), returning the final raw response.
static RawResponse fetchRaw(std::wstring url, const std::string* postBody, const char* accept) {
    bool isPost = postBody != nullptr;
    std::string body = isPost ? *postBody : std::string();
    RawResponse raw;

    for (int redirect = 0; redirect < 10; redirect++) {
        UrlParts parts;
        if (!crackUrl(url, parts)) { raw = RawResponse{}; raw.error = L"Bad URL"; return raw; }

        raw = sendOneRequest(parts, isPost ? http::verb::post : http::verb::get, body, accept);
        if (!raw.ok) return raw;

        if (raw.status >= 300 && raw.status < 400 && !raw.location.empty()) {
            std::wstring loc = utf8ToWide(raw.location);
            wchar_t combined[4096]; DWORD sz = 4096;
            if (!InternetCombineUrlW(url.c_str(), loc.c_str(), combined, &sz, ICU_BROWSER_MODE)) {
                raw.ok = false;
                raw.error = L"Bad redirect URL";
                return raw;
            }
            url = combined;
            isPost = false;
            body.clear();
            continue;
        }

        raw.finalUrl = url;
        if (raw.status >= 400) {
            raw.ok = false;
            raw.error = L"HTTP " + std::to_wstring(raw.status);
        }
        return raw;
    }

    raw.ok = false;
    raw.error = L"Too many redirects";
    return raw;
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

    RawResponse raw = fetchRaw(url, postBody, "text/html");
    FetchResult res;
    res.ok = raw.ok;
    res.error = raw.error;
    res.finalUrl = raw.finalUrl;
    if (raw.ok) res.html = utf8ToWide(raw.body);
    return res;
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

    RawResponse raw = fetchRaw(url, nullptr, nullptr);
    if (!raw.ok) return false;
    outBytes.assign(raw.body.begin(), raw.body.end());
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
