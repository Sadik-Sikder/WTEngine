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
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>
#include <chrono>

#include <boost/asio.hpp>
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <openssl/x509.h>
#include <zlib.h>

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
// The raw result of one HTTP round-trip, before it's turned into the
// public FetchResult/HttpResponse/bytes a caller asked for.

struct RawResponse {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string location;    // Location header, for the redirect loop (followRedirect)
    std::string contentType; // Content-Type header, for fetchHttpAsync
    std::wstring finalUrl;
    std::wstring error;
};

// ---------------------------------------------------------------------
// Connection reuse: a second request to the same host skips resolve/
// connect/TLS handshake entirely by reusing an idle kept-alive connection
// - measured (see HOW_IT_WORKS.md) at roughly 200ms of pure overhead per
// new connection, dominated by the TLS handshake, and a page very commonly
// sends many requests to the same host (its own origin, or a shared CDN).

// How long to keep an idle connection when the response didn't say (no
// Keep-Alive: timeout=N) - conservative, safely under most servers' actual
// defaults (commonly 5-15s), so guessing wrong in the "kept it too long"
// direction stays rare. The stale-connection retry in NetworkThread::sendOne
// covers it when a guess is wrong anyway, so this only needs to be a
// reasonable default, not a guarantee.
static constexpr std::chrono::seconds kDefaultPoolTimeout{ 4 };
// Bounds memory/socket use if connections are returned faster than
// they're reused - a low ceiling is fine since the goal is avoiding
// *repeat* handshakes to a host being fetched from concurrently right
// now, not maintaining a large persistent cache of every host ever visited.
static constexpr size_t kMaxPooledPerHost = 6; // = HostLimiter's per-host connection limit

// Parses a "Keep-Alive: timeout=N, max=N" response header. `max` (the
// number of requests the server will still allow on this connection)
// isn't tracked here - running into it is just another way a pooled
// connection turns out to be unusable, already handled reactively by the
// stale-connection retry in NetworkThread::sendOne, so there's nothing extra
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

// HTTP/1.1 (the only version this file ever sends - see buildRequest)
// defaults to keeping the connection open; only an explicit "close" token
// says otherwise.
static bool responseWantsClose(const http::response<http::string_body>& res) {
    auto it = res.find(http::field::connection);
    if (it == res.end()) return false;
    std::string val(it->value());
    for (auto& c : val) c = (char)std::tolower((unsigned char)c);
    return val.find("close") != std::string::npos;
}

// Decompresses a gzip- or deflate-encoded response body via zlib's
// inflate(). windowBits 15+32 is zlib's own documented trick for "detect
// either a gzip or a zlib/deflate header automatically" - one code path
// serves both encodings without needing to know in advance which a given
// server actually used. Some CDNs (confirmed against a real one, serving
// static assets stored pre-compressed in S3/CloudFront) send
// Content-Encoding: gzip unconditionally, regardless of whether the
// request even sent an Accept-Encoding header asking for it - so this
// isn't optional best-effort handling, every caller needs it. On any
// failure (truncated/corrupt data, an encoding claimed but not actually
// used), returns `compressed` unchanged rather than an empty result -
// matches how a redirect with a bad URL or a failed connection already
// degrade to "give back something rather than silently lose the page"
// elsewhere in this file. Brotli ("br") isn't supported - would need a
// separate library, and no server should send it unasked since this
// file never advertises it in Accept-Encoding (see buildRequest).
static std::string decompressBody(const std::string& compressed) {
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 32) != Z_OK) return compressed;

    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed.data()));
    zs.avail_in = static_cast<uInt>(compressed.size());

    std::string out;
    char chunk[16384];
    int ret;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(chunk);
        zs.avail_out = sizeof(chunk);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) { inflateEnd(&zs); return compressed; }
        out.append(chunk, sizeof(chunk) - zs.avail_out);
    } while (ret != Z_STREAM_END);

    inflateEnd(&zs);
    return out;
}

// Fills `out` from a received response: status, the headers this file
// cares about, and the body - decompressed if the server compressed it.
// Used by the network thread's exchange().
static void fillRawResponse(http::response<http::string_body>& res, RawResponse& out) {
    out.status = res.result_int();
    out.body = std::move(res.body());
    auto loc = res.find(http::field::location);
    if (loc != res.end()) out.location.assign(loc->value());
    auto ct = res.find(http::field::content_type);
    if (ct != res.end()) out.contentType.assign(ct->value());
    out.ok = true;

    auto ce = res.find(http::field::content_encoding);
    if (ce != res.end()) {
        std::string encoding(ce->value());
        for (auto& c : encoding) c = (char)std::tolower((unsigned char)c);
        if (encoding.find("gzip") != std::string::npos || encoding.find("deflate") != std::string::npos)
            out.body = decompressBody(out.body);
        // "br" (Brotli) and anything else unrecognized passes through as-is.
    }
}

// Extra request headers beyond the ones buildRequest always sets; an
// entry here overrides a default of the same name (e.g. Content-Type).
using HeaderList = std::vector<std::pair<std::string, std::string>>;

// Builds the request for one hop. `keepAlive` false sends "Connection:
// close", for a connection that won't be pooled.
static http::request<http::string_body> buildRequest(const UrlParts& parts, http::verb method,
                                                     const std::string& body, const char* accept,
                                                     const HeaderList& extraHeaders, bool keepAlive) {
    http::request<http::string_body> req{method, wideToUtf8(parts.pathAndQuery), 11};
    req.set(http::field::host, wideToUtf8(parts.host));
    req.set(http::field::user_agent, "WTEngine/0.1");
    if (accept) req.set(http::field::accept, accept); // "text/html" for a page, unset for an image
    req.set(http::field::accept_encoding, "gzip, deflate"); // decompressBody (fillRawResponse) handles both - see its comment
    req.set(http::field::connection, keepAlive ? "keep-alive" : "close"); // keep-alive: see ConnectionPool above
    if (!body.empty()) req.set(http::field::content_type, "application/x-www-form-urlencoded");
    for (const auto& [name, value] : extraHeaders) req.set(name, value);
    if (!body.empty() || method == http::verb::post || method == http::verb::put || method == http::verb::patch) {
        req.body() = body;
        req.prepare_payload(); // Content-Length (0 for an empty POST, which some servers require)
    }
    return req;
}

// If `raw` is a redirect, points `url` at its target and returns true (the
// caller sends the next hop). A 301/302/303 turns the next hop into a
// body-less GET, as browsers do; a 307/308 keeps the method and body.
// Returns false when `raw` isn't a redirect - or when its Location can't
// be resolved, in which case `raw` is also marked failed.
static bool followRedirect(std::wstring& url, http::verb& method, std::string& body, RawResponse& raw) {
    if (raw.status < 300 || raw.status >= 400 || raw.location.empty()) return false;
    std::wstring loc = utf8ToWide(raw.location);
    wchar_t combined[4096]; DWORD sz = 4096;
    if (!InternetCombineUrlW(url.c_str(), loc.c_str(), combined, &sz, ICU_BROWSER_MODE)) {
        raw.ok = false;
        raw.error = L"Bad redirect URL";
        return false;
    }
    url = combined;
    if (raw.status != 307 && raw.status != 308) {
        method = http::verb::get;
        body.clear();
    }
    return true;
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

// ---------------------------------------------------------------------
// The network thread.
//
// Every request - pages, scripts, stylesheets, images, JS fetch() - runs
// as a C++20 coroutine on one io_context, driven by one thread. Each
// co_await (resolve, connect, handshake, write, read) suspends that
// request and frees the thread for the others; Asio waits on all their
// sockets at once through an I/O completion port (IOCP) on Windows and
// resumes whichever one has data. So the number of requests in flight is
// no longer tied to a number of threads, and a slow server holds up
// nothing but its own request.
//
// Everything below the public API runs only on that thread, which is what
// lets the connection pool and the per-host limiter work without locks.
// Results reach callers through their `onDone` callbacks - also run on the
// network thread, so they only hand results off (see Fetcher.h).

namespace {

// A connection that can be kept alive and reused: exactly one of `tls` /
// `plain` is set.
struct Connection {
    std::unique_ptr<ssl::stream<beast::tcp_stream>> tls;
    std::unique_ptr<beast::tcp_stream> plain;
    std::chrono::steady_clock::time_point idleSince;
    std::chrono::seconds serverTimeout = kDefaultPoolTimeout;
    beast::tcp_stream& tcp() { return tls ? beast::get_lowest_layer(*tls) : *plain; }
};

// Idle kept-alive connections by "scheme://host:port".
class ConnectionPool {
public:
    std::unique_ptr<Connection> take(const std::string& key) {
        auto it = idle_.find(key);
        if (it == idle_.end()) return nullptr;
        auto now = std::chrono::steady_clock::now();
        auto& bucket = it->second;
        while (!bucket.empty()) {
            auto conn = std::move(bucket.back());
            bucket.pop_back();
            if (now - conn->idleSince <= conn->serverTimeout) return conn;
            // else: past its advertised lifetime, likely already closed by
            // the server - discard without even trying it.
        }
        return nullptr;
    }

    void give(const std::string& key, std::unique_ptr<Connection> conn) {
        auto& bucket = idle_[key];
        if (bucket.size() >= kMaxPooledPerHost) return; // closed by falling out of scope
        conn->tcp().expires_never(); // no stale deadline left armed while it sits idle
        conn->idleSince = std::chrono::steady_clock::now();
        bucket.push_back(std::move(conn));
    }

    void clear() { idle_.clear(); }

private:
    std::unordered_map<std::string, std::vector<std::unique_ptr<Connection>>> idle_;
};

// At most kMaxConnectionsPerHost requests in flight to one host at once -
// the same limit browsers use for HTTP/1.1. Without it, a page with 100
// images on one CDN would open 100 connections at once (which servers
// throttle or refuse) and bury the page's own scripts behind them. The
// rest wait here and are admitted highest priority first (FetchPriority
// order), first-come-first-served within a priority - so a script queued
// behind 50 images still goes next.
class HostLimiter {
public:
    net::awaitable<void> acquire(const std::string& key, FetchPriority priority) {
        Host& host = hosts_[key];
        if (host.active < kMaxConnectionsPerHost) { host.active++; co_return; }

        // Wait on a timer that never expires on its own; release() cancels
        // it to wake this request, handing over its slot (so `active`
        // doesn't change).
        auto timer = std::make_shared<net::steady_timer>(co_await net::this_coro::executor,
                                                         net::steady_timer::time_point::max());
        host.waiting.push_back({ priority, nextSeq_++, timer });
        co_await timer->async_wait(net::as_tuple(net::use_awaitable)); // "cancelled" is the wake-up, not an error
    }

    void release(const std::string& key) {
        auto it = hosts_.find(key);
        if (it == hosts_.end()) return;
        Host& host = it->second;
        if (host.waiting.empty()) {
            if (--host.active == 0) hosts_.erase(it);
            return;
        }
        auto next = std::min_element(host.waiting.begin(), host.waiting.end(),
            [](const Waiter& a, const Waiter& b) {
                return a.priority != b.priority ? a.priority < b.priority : a.seq < b.seq;
            });
        auto timer = next->timer;
        host.waiting.erase(next);
        timer->cancel();
    }

private:
    static constexpr int kMaxConnectionsPerHost = 6;
    struct Waiter {
        FetchPriority priority;
        uint64_t seq;
        std::shared_ptr<net::steady_timer> timer;
    };
    struct Host {
        int active = 0;
        std::vector<Waiter> waiting;
    };
    std::unordered_map<std::string, Host> hosts_;
    uint64_t nextSeq_ = 0;
};

// Holds one of a host's slots for as long as it's alive.
class HostSlot {
public:
    HostSlot(HostLimiter& limiter, std::string key) : limiter_(&limiter), key_(std::move(key)) {}
    ~HostSlot() { limiter_->release(key_); }
    HostSlot(const HostSlot&) = delete;
    HostSlot& operator=(const HostSlot&) = delete;
private:
    HostLimiter* limiter_;
    std::string key_;
};

// Sends `req` on a connected stream and reads the response. Throws on
// failure, like the Asio operations it wraps.
template <class Stream>
net::awaitable<void> exchange(Stream& stream, http::request<http::string_body>& req, RawResponse& out,
                              bool& keepAlive, std::chrono::seconds& keepAliveTimeout) {
    co_await http::async_write(stream, req, net::use_awaitable);
    beast::flat_buffer buffer;
    http::response<http::string_body> res;
    co_await http::async_read(stream, buffer, res, net::use_awaitable);
    fillRawResponse(res, out);
    out.ok = true;

    keepAlive = !responseWantsClose(res);
    keepAliveTimeout = kDefaultPoolTimeout;
    auto ka = res.find(http::field::keep_alive);
    if (ka != res.end()) {
        if (auto t = parseKeepAliveTimeout(std::string(ka->value()))) keepAliveTimeout = std::chrono::seconds(*t);
    }
}

net::awaitable<void> exchangeOn(Connection& conn, http::request<http::string_body>& req, RawResponse& out,
                                bool& keepAlive, std::chrono::seconds& keepAliveTimeout) {
    if (conn.tls) co_await exchange(*conn.tls, req, out, keepAlive, keepAliveTimeout);
    else co_await exchange(*conn.plain, req, out, keepAlive, keepAliveTimeout);
}

// Resolves, connects and (for HTTPS) handshakes a new connection. The 3s
// connect deadline covers trying every resolved address - see the file
// header for why a short deadline, not the OS's own connect timeout, is
// what avoids long stalls on a host with an unreachable address.
net::awaitable<std::unique_ptr<Connection>> openConnection(const UrlParts& parts, const std::string& host,
                                                          const std::string& port) {
    auto executor = co_await net::this_coro::executor;
    tcp::resolver resolver(executor);
    auto results = co_await resolver.async_resolve(host, port, net::use_awaitable);

    auto conn = std::make_unique<Connection>();
    if (parts.https) {
        conn->tls = std::make_unique<ssl::stream<beast::tcp_stream>>(executor, sharedSslContext());
        // SNI: without this, many hosts (anything relying on virtual hosting
        // by hostname, i.e. most of the web) return the wrong certificate or
        // reject the handshake outright.
        if (!SSL_set_tlsext_host_name(conn->tls->native_handle(), host.c_str())) {
            beast::error_code ec{static_cast<int>(::ERR_get_error()), net::error::get_ssl_category()};
            throw beast::system_error(ec);
        }
        // verify_peer (set on the shared context) alone only checks the
        // certificate chains to a trusted CA - not that it's actually FOR
        // this host. Without this, any validly-CA-signed certificate for
        // any site would be accepted, defeating the point.
        conn->tls->set_verify_callback(ssl::host_name_verification(host));

        conn->tcp().expires_after(std::chrono::seconds(3));
        co_await conn->tcp().async_connect(results, net::use_awaitable);
        conn->tcp().expires_after(std::chrono::seconds(10));
        co_await conn->tls->async_handshake(ssl::stream_base::client, net::use_awaitable);
    } else {
        conn->plain = std::make_unique<beast::tcp_stream>(executor);
        conn->tcp().expires_after(std::chrono::seconds(3));
        co_await conn->tcp().async_connect(results, net::use_awaitable);
    }
    co_return conn;
}

// What a request needs besides its URL and body.
struct RequestSpec {
    http::verb method = http::verb::get;
    const char* accept = nullptr;
    HeaderList headers;
    bool errorOnHttpStatus = true; // 4xx/5xx is a failure (pages, resources, images) vs a normal response (fetch())
    FetchPriority priority = FetchPriority::Fetch;
    std::function<bool()> stillWanted;
};

class NetworkThread {
public:
    static NetworkThread& instance() {
        static NetworkThread thread;
        return thread;
    }

    template <class Awaitable>
    void spawn(Awaitable&& work) {
        net::co_spawn(io_, std::forward<Awaitable>(work), net::detached);
    }

    // A whole request, redirects included. Never throws: every failure
    // becomes RawResponse::error.
    net::awaitable<RawResponse> fetch(std::wstring url, std::string body, RequestSpec spec) {
        RawResponse raw;
        for (int redirect = 0; redirect < 10; redirect++) {
            UrlParts parts;
            if (!crackUrl(url, parts)) { raw = RawResponse{}; raw.error = L"Bad URL"; co_return raw; }
            std::string key = std::string(parts.https ? "https://" : "http://") +
                              wideToUtf8(parts.host) + ":" + std::to_string(parts.port);

            // Per hop: a redirect can lead to a different host.
            co_await limiter_.acquire(key, spec.priority);
            HostSlot slot(limiter_, key);

            // Checked after waiting for a slot, when nothing has been sent
            // yet: a superseded page load or script batch costs nothing.
            if (spec.stillWanted && !spec.stillWanted()) {
                raw = RawResponse{};
                raw.error = L"Cancelled";
                co_return raw;
            }

            std::string failure;
            try {
                raw = co_await sendOne(parts, key, spec, body);
            } catch (const std::exception& e) {
                failure = e.what(); // can't co_return from inside a catch block
            }
            if (!failure.empty()) { raw = RawResponse{}; raw.error = utf8ToWide(failure); co_return raw; }

            if (followRedirect(url, spec.method, body, raw)) continue;
            if (!raw.ok) co_return raw; // a redirect with an unusable Location

            raw.finalUrl = url;
            if (spec.errorOnHttpStatus && raw.status >= 400) {
                raw.ok = false;
                raw.error = L"HTTP " + std::to_wstring(raw.status);
            }
            co_return raw;
        }
        raw = RawResponse{};
        raw.error = L"Too many redirects";
        co_return raw;
    }

    ~NetworkThread() {
        io_.stop();
        if (thread_.joinable()) thread_.join();
        pool_.clear(); // close pooled sockets while io_ (their owner) is still alive
    }

private:
    NetworkThread() : work_(net::make_work_guard(io_)) {
        sharedSslContext(); // construct it first so it's destroyed after this (static destruction is reverse order)
        thread_ = std::thread([this] { io_.run(); });
    }

    // One hop: reuse a pooled connection if there is one, else open a new
    // one. A pooled connection can turn out to be stale (the server closed
    // it while it sat idle - the one failure pooling can't avoid, only
    // recover from); that failure is swallowed and the request retried on
    // a fresh connection, whose own failures do propagate.
    net::awaitable<RawResponse> sendOne(const UrlParts& parts, const std::string& key,
                                        const RequestSpec& spec, const std::string& body) {
        // fetch() may legitimately wait on a slow API; everything else
        // keeps the original page-load deadline.
        auto deadline = std::chrono::seconds(spec.priority == FetchPriority::Fetch ? 30 : 10);
        http::request<http::string_body> req = buildRequest(parts, spec.method, body, spec.accept, spec.headers, true);
        RawResponse out;
        bool keepAlive = false;
        std::chrono::seconds keepAliveTimeout{};

        if (auto conn = pool_.take(key)) {
            bool stale = false;
            try {
                conn->tcp().expires_after(deadline);
                co_await exchangeOn(*conn, req, out, keepAlive, keepAliveTimeout);
            } catch (const std::exception&) {
                stale = true;
            }
            if (!stale) {
                if (keepAlive) { conn->serverTimeout = keepAliveTimeout; pool_.give(key, std::move(conn)); }
                co_return out;
            }
            out = RawResponse{};
        }

        auto conn = co_await openConnection(parts, wideToUtf8(parts.host), std::to_string(parts.port));
        conn->tcp().expires_after(deadline);
        co_await exchangeOn(*conn, req, out, keepAlive, keepAliveTimeout);
        if (keepAlive) { conn->serverTimeout = keepAliveTimeout; pool_.give(key, std::move(conn)); }
        // else `conn` closes as it goes out of scope. No TLS close_notify:
        // the server has said it's closing anyway, and waiting on its reply
        // could stall this request.
        co_return out;
    }

    // Declared before io_, so destroyed after it: coroutine frames still
    // pending at exit are destroyed along with io_, and their HostSlots
    // release into the limiter on the way out.
    HostLimiter limiter_;
    ConnectionPool pool_;
    net::io_context io_{1}; // concurrency hint: exactly one thread runs this loop
    net::executor_work_guard<net::io_context::executor_type> work_; // keeps run() alive while idle
    std::thread thread_;
};

// --- The per-API coroutines: run a request, convert its result, call onDone.

net::awaitable<void> runPage(std::wstring url, std::string body, bool isPost, FetchOptions options,
                             std::function<void(FetchResult)> onDone) {
    FetchResult res;
    if (!isHttpUrl(url)) {
        res = readLocalFile(url); // a POST body makes no sense for a file
    } else {
        RequestSpec spec;
        spec.method = isPost ? http::verb::post : http::verb::get;
        // "text/html" for a top-level page; a script/stylesheet is whatever it is.
        spec.accept = options.priority == FetchPriority::Page ? "text/html" : "*/*";
        spec.priority = options.priority;
        spec.stillWanted = std::move(options.stillWanted);
        RawResponse raw = co_await NetworkThread::instance().fetch(url, std::move(body), std::move(spec));
        res.ok = raw.ok;
        res.error = raw.error;
        res.finalUrl = raw.finalUrl;
        if (raw.ok) res.html = utf8ToWide(raw.body);
    }
    onDone(std::move(res));
}

net::awaitable<void> runBytes(std::wstring url, FetchOptions options,
                              std::function<void(bool, std::vector<unsigned char>)> onDone) {
    std::vector<unsigned char> bytes;
    bool ok = false;
    if (url.rfind(L"data:", 0) == 0) {
        ok = decodeDataUri(url, bytes);
    } else if (!isHttpUrl(url)) {
        std::ifstream in(url, std::ios::binary);
        if (in) {
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            ok = true;
        }
    } else {
        RequestSpec spec;
        spec.priority = options.priority;
        spec.stillWanted = std::move(options.stillWanted);
        RawResponse raw = co_await NetworkThread::instance().fetch(url, std::string(), std::move(spec));
        if (raw.ok) {
            bytes.assign(raw.body.begin(), raw.body.end());
            ok = true;
        }
    }
    onDone(ok, std::move(bytes));
}

net::awaitable<void> runHttp(HttpRequest request, std::function<void(HttpResponse)> onDone) {
    HttpResponse res;
    std::string methodName = request.method;
    for (auto& c : methodName) c = (char)std::toupper((unsigned char)c);
    http::verb method = http::string_to_verb(methodName);

    if (!isHttpUrl(request.url)) {
        // A local file - only reachable from a page that was itself loaded
        // from disk (see JS fetch()).
        std::ifstream in(request.url, std::ios::binary);
        if (!in) {
            res.error = L"Could not open file: " + request.url;
        } else {
            res.body.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            res.ok = true;
            res.status = 200;
            res.finalUrl = request.url;
        }
    } else if (method == http::verb::unknown) {
        res.error = L"Unsupported method";
    } else {
        RequestSpec spec;
        spec.method = method;
        spec.accept = "*/*";
        spec.headers = std::move(request.headers);
        spec.errorOnHttpStatus = false;
        spec.priority = FetchPriority::Fetch;
        RawResponse raw = co_await NetworkThread::instance().fetch(request.url, std::move(request.body), std::move(spec));
        res.ok = raw.ok;
        res.status = raw.status;
        res.body = std::move(raw.body);
        res.contentType = std::move(raw.contentType);
        res.finalUrl = raw.finalUrl;
        res.error = raw.error;
    }
    onDone(std::move(res));
}

} // namespace

// ---------------------------------------------------------------------
// Public API - each just starts a coroutine on the network thread.

void fetchPageAsync(std::wstring url, const std::string* postBody, FetchOptions options,
                    std::function<void(FetchResult)> onDone) {
    bool isPost = postBody != nullptr;
    NetworkThread::instance().spawn(runPage(std::move(url), isPost ? *postBody : std::string(), isPost,
                                            std::move(options), std::move(onDone)));
}

void fetchBytesAsync(std::wstring url, FetchOptions options,
                     std::function<void(bool, std::vector<unsigned char>)> onDone) {
    NetworkThread::instance().spawn(runBytes(std::move(url), std::move(options), std::move(onDone)));
}

void fetchHttpAsync(HttpRequest request, std::function<void(HttpResponse)> onDone) {
    NetworkThread::instance().spawn(runHttp(std::move(request), std::move(onDone)));
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
