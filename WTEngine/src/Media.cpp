#include "Media.h"
#include "Fetcher.h"

#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <mferror.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <io.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <limits>
#include <map>
#include <mutex>
#include <set>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace {

// DXGI_FORMAT_B8G8R8A8_UNORM, without pulling in dxgiformat.h: the pixel
// format frames are converted to - what WIC calls 32bppBGRA.
constexpr UINT32 kFrameFormat = 87;

bool startMediaFoundation() {
    static const bool started = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    return started;
}

// The DOM event each of the media engine's events stands for. The media
// engine was built to implement HTMLMediaElement, so most map one to one.
const wchar_t* domEventName(DWORD event) {
    switch (event) {
    case MF_MEDIA_ENGINE_EVENT_LOADSTART: return L"loadstart";
    case MF_MEDIA_ENGINE_EVENT_PROGRESS: return L"progress";
    case MF_MEDIA_ENGINE_EVENT_SUSPEND: return L"suspend";
    case MF_MEDIA_ENGINE_EVENT_ABORT: return L"abort";
    case MF_MEDIA_ENGINE_EVENT_ERROR: return L"error";
    case MF_MEDIA_ENGINE_EVENT_EMPTIED: return L"emptied";
    case MF_MEDIA_ENGINE_EVENT_STALLED: return L"stalled";
    case MF_MEDIA_ENGINE_EVENT_PLAY: return L"play";
    case MF_MEDIA_ENGINE_EVENT_PAUSE: return L"pause";
    case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA: return L"loadedmetadata";
    case MF_MEDIA_ENGINE_EVENT_LOADEDDATA: return L"loadeddata";
    case MF_MEDIA_ENGINE_EVENT_WAITING: return L"waiting";
    case MF_MEDIA_ENGINE_EVENT_PLAYING: return L"playing";
    case MF_MEDIA_ENGINE_EVENT_CANPLAY: return L"canplay";
    case MF_MEDIA_ENGINE_EVENT_CANPLAYTHROUGH: return L"canplaythrough";
    case MF_MEDIA_ENGINE_EVENT_SEEKING: return L"seeking";
    case MF_MEDIA_ENGINE_EVENT_SEEKED: return L"seeked";
    case MF_MEDIA_ENGINE_EVENT_TIMEUPDATE: return L"timeupdate";
    case MF_MEDIA_ENGINE_EVENT_ENDED: return L"ended";
    case MF_MEDIA_ENGINE_EVENT_RATECHANGE: return L"ratechange";
    case MF_MEDIA_ENGINE_EVENT_DURATIONCHANGE: return L"durationchange";
    case MF_MEDIA_ENGINE_EVENT_VOLUMECHANGE: return L"volumechange";
    case MF_MEDIA_ENGINE_EVENT_FORMATCHANGE: return L"resize"; // the video's size changed
    default: return nullptr;
    }
}

// Events arrive on Media Foundation's worker threads; the page picks them
// up on the main thread (takeEvents). Shared with the callback object,
// which the media engine may release after the player is gone.
struct EventQueue {
    std::mutex mutex;
    std::vector<std::wstring> events;
};

class Notify : public IMFMediaEngineNotify {
public:
    explicit Notify(std::shared_ptr<EventQueue> q) : queue_(std::move(q)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *out = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG n = --refs_;
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP EventNotify(DWORD event, DWORD_PTR param1, DWORD) override {
        // The media engine waits on this one until it's acknowledged.
        if (event == MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE) {
            SetEvent(reinterpret_cast<HANDLE>(param1));
            return S_OK;
        }
        if (const wchar_t* name = domEventName(event)) {
            std::lock_guard<std::mutex> lock(queue_->mutex);
            queue_->events.push_back(name);
        }
        return S_OK;
    }

private:
    std::atomic<ULONG> refs_{ 1 };
    std::shared_ptr<EventQueue> queue_;
};

// --- http(s) sources, through the browser's own network stack -------------
// The media engine can fetch a URL itself, but its requests go out without
// the page's cookies or referrer, so servers that check either refuse them.
// Instead an http(s) file is read through an HttpByteStream: in 1 MB pieces,
// each a Range request through Fetcher (cookies, referrer, our User-Agent),
// a few pieces ahead of where playback reads. Reads come from Media
// Foundation's worker threads, which may block; the UI thread never does.

constexpr QWORD kChunk = 1 << 20;
constexpr size_t kMaxChunks = 48; // ~48 MB of the file kept in memory at once
constexpr QWORD kReadAhead = 3;   // pieces fetched ahead of the one being read

// The pieces fetched so far. Shared with Fetcher's callbacks, which can
// finish after the stream is gone.
struct ChunkStore {
    std::mutex mutex;
    std::condition_variable cv;
    std::map<QWORD, std::string> chunks; // piece index -> its bytes
    std::map<QWORD, unsigned long long> lastUse;
    unsigned long long clock = 0;
    std::set<QWORD> inFlight, failed;
    bool closed = false; // the player is going: waiting reads give up
};

// Asks for piece `index` (and so on) unless it's here or on its way.
void requestChunk(const std::shared_ptr<ChunkStore>& store, const std::wstring& url, const std::wstring& referrer,
                  QWORD index, QWORD length) {
    const QWORD first = index * kChunk;
    if (first >= length) return;
    {
        std::lock_guard<std::mutex> lock(store->mutex);
        if (store->closed || store->chunks.count(index) || store->inFlight.count(index)) return;
        store->inFlight.insert(index);
        store->failed.erase(index);
    }
    const QWORD last = std::min(first + kChunk, length) - 1;
    HttpRequest req;
    req.url = url;
    req.referrer = referrer;
    req.headers.push_back({ "Range", "bytes=" + std::to_string(first) + "-" + std::to_string(last) });
    std::weak_ptr<ChunkStore> weak = store;
    fetchHttpAsync(std::move(req), [weak, index, first, last](HttpResponse res) {
        auto s = weak.lock();
        if (!s) return;
        std::string bytes;
        const size_t want = (size_t)(last - first + 1);
        if (res.ok && res.status == 206) bytes = std::move(res.body);
        else if (res.ok && res.status == 200 && res.body.size() > last) bytes = res.body.substr((size_t)first, want);
        std::lock_guard<std::mutex> lock(s->mutex);
        s->inFlight.erase(index);
        if (bytes.size() == want) {
            s->chunks[index] = std::move(bytes);
            s->lastUse[index] = ++s->clock;
        }
        else {
            s->failed.insert(index);
        }
        s->cv.notify_all();
    });
}

class HttpByteStream : public Microsoft::WRL::RuntimeClass<
                           Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFByteStream> {
public:
    HttpByteStream(std::wstring url, std::wstring referrer, QWORD length, std::shared_ptr<ChunkStore> store)
        : url_(std::move(url)), referrer_(std::move(referrer)), length_(length), store_(std::move(store)) {}

    // Copies up to `size` bytes from offset `at`; short only at the end of
    // the file. Waits for pieces that haven't arrived.
    HRESULT readAt(QWORD at, BYTE* buffer, ULONG size, ULONG* done) {
        *done = 0;
        while (*done < size && at < length_) {
            const QWORD index = at / kChunk;
            for (QWORD i = index; i <= index + kReadAhead; i++) requestChunk(store_, url_, referrer_, i, length_);

            std::unique_lock<std::mutex> lock(store_->mutex);
            bool settled = store_->cv.wait_for(lock, std::chrono::seconds(30), [&] {
                return store_->closed || store_->chunks.count(index) ||
                       (store_->failed.count(index) && !store_->inFlight.count(index));
            });
            auto it = store_->chunks.find(index);
            if (!settled || store_->closed || it == store_->chunks.end())
                return *done > 0 ? S_OK : E_FAIL; // the next read retries a failed piece
            const std::string& chunk = it->second;
            const size_t offset = (size_t)(at - index * kChunk);
            const size_t n = std::min<size_t>(size - *done, chunk.size() - offset);
            std::memcpy(buffer + *done, chunk.data() + offset, n);
            *done += (ULONG)n;
            at += n;
            store_->lastUse[index] = ++store_->clock;

            // Over budget: drop the piece used longest ago.
            while (store_->chunks.size() > kMaxChunks) {
                auto oldest = store_->chunks.end();
                for (auto c = store_->chunks.begin(); c != store_->chunks.end(); ++c)
                    if (c->first != index && (oldest == store_->chunks.end() ||
                                              store_->lastUse[c->first] < store_->lastUse[oldest->first]))
                        oldest = c;
                if (oldest == store_->chunks.end()) break;
                store_->lastUse.erase(oldest->first);
                store_->chunks.erase(oldest);
            }
        }
        return S_OK;
    }

    STDMETHODIMP GetCapabilities(DWORD* caps) override {
        *caps = MFBYTESTREAM_IS_READABLE | MFBYTESTREAM_IS_SEEKABLE;
        return S_OK;
    }
    STDMETHODIMP GetLength(QWORD* length) override { *length = length_; return S_OK; }
    STDMETHODIMP SetLength(QWORD) override { return E_NOTIMPL; }
    STDMETHODIMP GetCurrentPosition(QWORD* pos) override {
        std::lock_guard<std::mutex> lock(posMutex_);
        *pos = pos_;
        return S_OK;
    }
    STDMETHODIMP SetCurrentPosition(QWORD pos) override {
        if (pos > length_) return E_INVALIDARG;
        std::lock_guard<std::mutex> lock(posMutex_);
        pos_ = pos;
        return S_OK;
    }
    STDMETHODIMP IsEndOfStream(BOOL* end) override {
        std::lock_guard<std::mutex> lock(posMutex_);
        *end = pos_ >= length_;
        return S_OK;
    }
    STDMETHODIMP Read(BYTE* buffer, ULONG size, ULONG* read) override {
        QWORD at = claim(size);
        return readAt(at, buffer, size, read);
    }
    STDMETHODIMP BeginRead(BYTE* buffer, ULONG size, IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndRead(IMFAsyncResult* result, ULONG* read) override;
    STDMETHODIMP Write(const BYTE*, ULONG, ULONG*) override { return E_ACCESSDENIED; }
    STDMETHODIMP BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) override { return E_ACCESSDENIED; }
    STDMETHODIMP EndWrite(IMFAsyncResult*, ULONG*) override { return E_ACCESSDENIED; }
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN origin, LONGLONG offset, DWORD, QWORD* newPos) override {
        std::lock_guard<std::mutex> lock(posMutex_);
        LONGLONG base = origin == msoBegin ? 0 : (LONGLONG)pos_;
        pos_ = (QWORD)std::clamp<LONGLONG>(base + offset, 0, (LONGLONG)length_);
        if (newPos) *newPos = pos_;
        return S_OK;
    }
    STDMETHODIMP Flush() override { return S_OK; }
    STDMETHODIMP Close() override {
        std::lock_guard<std::mutex> lock(store_->mutex);
        store_->closed = true;
        store_->cv.notify_all();
        return S_OK;
    }

private:
    // The bytes a read starting now will take: the position moves past
    // them at once, so reads queued one after another follow on.
    QWORD claim(ULONG size) {
        std::lock_guard<std::mutex> lock(posMutex_);
        QWORD at = pos_;
        pos_ = std::min(pos_ + size, length_);
        return at;
    }

    std::wstring url_, referrer_;
    QWORD length_;
    std::shared_ptr<ChunkStore> store_;
    std::mutex posMutex_;
    QWORD pos_ = 0;
};

// An asynchronous read (BeginRead): run on Media Foundation's queue for
// work that blocks, then reported to the caller's callback. It is also the
// result's object, which is how EndRead finds the byte count.
class ReadWork : public Microsoft::WRL::RuntimeClass<
                     Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFAsyncCallback> {
public:
    ReadWork(HttpByteStream* stream, BYTE* buffer, ULONG size, QWORD at)
        : stream_(stream), buffer_(buffer), size_(size), at_(at) {}
    ULONG done = 0;

    STDMETHODIMP GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult* work) override {
        ComPtr<IUnknown> state;
        ComPtr<IMFAsyncResult> caller;
        if (FAILED(work->GetState(&state)) || FAILED(state.As(&caller))) return E_UNEXPECTED;
        caller->SetStatus(stream_->readAt(at_, buffer_, size_, &done));
        MFInvokeCallback(caller.Get());
        return S_OK;
    }

private:
    ComPtr<HttpByteStream> stream_;
    BYTE* buffer_;
    ULONG size_;
    QWORD at_;
};

STDMETHODIMP HttpByteStream::BeginRead(BYTE* buffer, ULONG size, IMFAsyncCallback* callback, IUnknown* state) {
    auto work = Microsoft::WRL::Make<ReadWork>(this, buffer, size, claim(size));
    if (!work) return E_OUTOFMEMORY;
    ComPtr<IMFAsyncResult> result;
    HRESULT hr = MFCreateAsyncResult(static_cast<IMFAsyncCallback*>(work.Get()), callback, state, &result);
    if (FAILED(hr)) return hr;
    return MFPutWorkItem2(MFASYNC_CALLBACK_QUEUE_LONG_FUNCTION, 0, work.Get(), result.Get());
}

STDMETHODIMP HttpByteStream::EndRead(IMFAsyncResult* result, ULONG* read) {
    ComPtr<IUnknown> object;
    ComPtr<IMFAsyncCallback> callback;
    if (FAILED(result->GetObject(&object)) || FAILED(object.As(&callback))) return E_INVALIDARG;
    *read = static_cast<ReadWork*>(callback.Get())->done;
    return result->GetStatus();
}

// The first piece, fetched before the media engine is given the stream:
// it says how long the file is and whether the server takes Range requests.
struct Probe {
    std::mutex mutex;
    bool done = false;
    HttpResponse response;
};

bool isHttpUrl(const std::wstring& url) {
    return url.rfind(L"http://", 0) == 0 || url.rfind(L"https://", 0) == 0;
}

// The URL the media engine is told the stream came from - it picks the
// demuxer by its extension. One without a known extension (a CDN path
// with none) gets one from the response's Content-Type.
std::wstring hintUrl(const std::wstring& url, const std::string& contentType) {
    std::wstring path = url.substr(0, url.find_first_of(L"?#"));
    std::wstring ext;
    size_t dot = path.rfind(L'.'), slash = path.rfind(L'/');
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) ext = path.substr(dot + 1);
    for (auto& c : ext) c = (wchar_t)towlower(c);
    static const wchar_t* kKnown[] = { L"mp4", L"m4v", L"m4a", L"mov", L"webm", L"mkv", L"mp3", L"wav", L"aac",
                                       L"ogg", L"oga", L"ogv", L"flac", L"3gp", L"wma", L"wmv", L"asf" };
    for (const wchar_t* k : kKnown) if (ext == k) return url;
    static const std::pair<const char*, const wchar_t*> kByType[] = {
        { "video/mp4", L"mp4" }, { "audio/mp4", L"m4a" }, { "video/webm", L"webm" }, { "audio/webm", L"webm" },
        { "audio/mpeg", L"mp3" }, { "audio/mp3", L"mp3" }, { "audio/wav", L"wav" }, { "audio/x-wav", L"wav" },
        { "audio/aac", L"aac" }, { "audio/ogg", L"ogg" }, { "video/ogg", L"ogv" }, { "audio/flac", L"flac" },
        { "video/quicktime", L"mov" },
    };
    for (const auto& [type, e] : kByType)
        if (contentType.rfind(type, 0) == 0) return path + L"." + e;
    return url;
}

// --- Media Source Extensions ----------------------------------------------

// The media engine's hook for a MediaSource: MediaPlayer::loadMediaSource
// leaves the MSE object here and points the engine at "wtmse:<id>", which
// the engine then asks this to open (the same route Chromium's Media
// Foundation renderer takes for its own sources). Any other URL the engine
// opens itself.
class MseExtension
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFMediaEngineExtension> {
public:
    void setPending(ComPtr<IUnknown> source) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = std::move(source);
    }
    STDMETHODIMP CanPlayType(BOOL, BSTR, MF_MEDIA_ENGINE_CANPLAY* answer) override {
        *answer = MF_MEDIA_ENGINE_CANPLAY_NOT_SUPPORTED; // types are the engine's own business
        return S_OK;
    }
    STDMETHODIMP BeginCreateObject(BSTR url, IMFByteStream*, MF_OBJECT_TYPE type, IUnknown** cancelCookie,
                                   IMFAsyncCallback* callback, IUnknown* state) override {
        if (cancelCookie) *cancelCookie = nullptr;
        if (!url || wcsncmp(url, L"wtmse:", 6) != 0 || type != MF_OBJECT_MEDIASOURCE) return MF_E_UNSUPPORTED_SCHEME;
        ComPtr<IUnknown> source;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            source = pending_;
        }
        if (!source) return MF_E_UNSUPPORTED_SCHEME;
        ComPtr<IMFAsyncResult> result;
        HRESULT hr = MFCreateAsyncResult(source.Get(), callback, state, &result);
        if (FAILED(hr)) return hr;
        result->SetStatus(S_OK);
        return MFInvokeCallback(result.Get());
    }
    STDMETHODIMP CancelObjectCreation(IUnknown*) override { return S_OK; }
    STDMETHODIMP EndCreateObject(IMFAsyncResult* result, IUnknown** object) override {
        *object = nullptr;
        HRESULT hr = result->GetStatus();
        return FAILED(hr) ? hr : result->GetObject(object);
    }

private:
    std::mutex mutex_;
    ComPtr<IUnknown> pending_;
};

// Whether a decoder for `subtype` is installed - VP9 and Opus come with
// Store extensions (VP9 Video Extensions, Web Media Extensions), and MSE
// claims a type only when it can really be played.
bool hasDecoder(const GUID& category, const GUID& major, const GUID& subtype) {
    MFT_REGISTER_TYPE_INFO input{ major, subtype };
    IMFActivate** found = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(category, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                         &input, nullptr, &found, &count)))
        return false;
    for (UINT32 i = 0; i < count; i++) found[i]->Release();
    CoTaskMemFree(found);
    return count > 0;
}

// MediaSource/SourceBuffer events, from Media Foundation's threads.
struct MseEventQueue {
    std::mutex mutex;
    std::vector<mse::Event> events;
};
MseEventQueue& mseEvents() {
    static MseEventQueue queue;
    return queue;
}
void pushMseEvent(int id, int buffer, const wchar_t* name) {
    std::lock_guard<std::mutex> lock(mseEvents().mutex);
    mseEvents().events.push_back({ id, buffer, name });
}

class MseNotify
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFMediaSourceExtensionNotify> {
public:
    explicit MseNotify(int id) : id_(id) {}
    void STDMETHODCALLTYPE OnSourceOpen() override { pushMseEvent(id_, -1, L"sourceopen"); }
    void STDMETHODCALLTYPE OnSourceEnded() override { pushMseEvent(id_, -1, L"sourceended"); }
    void STDMETHODCALLTYPE OnSourceClose() override { pushMseEvent(id_, -1, L"sourceclose"); }
private:
    int id_;
};

class SourceBufferNotify
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFSourceBufferNotify> {
public:
    SourceBufferNotify(int id, int buffer) : id_(id), buffer_(buffer) {}
    void STDMETHODCALLTYPE OnUpdateStart() override { pushMseEvent(id_, buffer_, L"updatestart"); }
    void STDMETHODCALLTYPE OnAbort() override { pushMseEvent(id_, buffer_, L"abort"); }
    void STDMETHODCALLTYPE OnError(HRESULT) override { pushMseEvent(id_, buffer_, L"error"); }
    void STDMETHODCALLTYPE OnUpdate() override { pushMseEvent(id_, buffer_, L"update"); }
    void STDMETHODCALLTYPE OnUpdateEnd() override { pushMseEvent(id_, buffer_, L"updateend"); }
private:
    int id_, buffer_;
};

// How long the init segment at the start of an append is - its first
// bytes up to the media data - or 0 if it doesn't start with one. MP4:
// boxes up to the first moof/mdat (ftyp, moov, ...). WebM: the EBML
// header and segment info, up to the first Cluster.
size_t initSegmentLength(const uint8_t* data, size_t length) {
    if (length >= 4 && data[0] == 0x1A && data[1] == 0x45 && data[2] == 0xDF && data[3] == 0xA3) { // WebM
        for (size_t i = 4; i + 4 <= length; i++)
            if (data[i] == 0x1F && data[i + 1] == 0x43 && data[i + 2] == 0xB6 && data[i + 3] == 0x75) return i;
        return length;
    }
    if (length < 8 || (std::memcmp(data + 4, "ftyp", 4) != 0 && std::memcmp(data + 4, "moov", 4) != 0)) return 0;
    size_t at = 0;
    while (at + 8 <= length) { // MP4 boxes: a 32-bit size (1: a 64-bit one follows) and a type
        uint64_t size = (uint64_t)data[at] << 24 | data[at + 1] << 16 | data[at + 2] << 8 | data[at + 3];
        if (std::memcmp(data + at + 4, "moof", 4) == 0 || std::memcmp(data + at + 4, "mdat", 4) == 0) return at;
        if (size == 1 && at + 16 <= length) {
            size = 0;
            for (int k = 0; k < 8; k++) size = size << 8 | data[at + 8 + k];
        }
        if (size < 8 || at + size > length) return length;
        at += (size_t)size;
    }
    return at;
}

// Follows the top-level pieces going into a recording - MP4 boxes, or the
// WebM elements after the init segment (Clusters) - so that when a page
// aborts an append partway through one (seeking while a segment
// downloads), the recording can be cut back to the last whole one.
struct SegmentTracker {
    bool webm = false;
    bool lost = false;     // something it can't follow: it leaves the file alone
    uint64_t pos = 0;      // bytes in the file
    uint64_t boundary = 0; // where the last whole piece ended
    uint64_t skip = 0;     // what's left of the current piece's data
    bool open = false;     // in a WebM Cluster of unknown size: it ends where the next piece starts
    bool moof = false;     // an MP4 moof waits for its mdat
    std::string header;    // the next piece's header, so far

    void reset(bool isWebm, uint64_t at) {
        *this = SegmentTracker();
        webm = isWebm;
        pos = boundary = at;
    }
    bool whole() const { return header.empty() && !skip && !open && !moof; }

    void feed(const uint8_t* p, size_t n) {
        while (n && !lost) {
            if (skip) {
                size_t k = (size_t)std::min<uint64_t>(skip, n);
                skip -= k, pos += k, p += k, n -= k;
                if (!skip && !open && !moof) boundary = pos;
                continue;
            }
            header.push_back((char)*p++);
            pos++, n--;
            const uint8_t* h = reinterpret_cast<const uint8_t*>(header.data());
            uint64_t size = 0;
            if (!webm) { // a 32-bit size (1: a 64-bit one follows) and a type
                if (header.size() < 8) continue;
                size = (uint64_t)h[0] << 24 | h[1] << 16 | h[2] << 8 | h[3];
                if (size == 1) {
                    if (header.size() < 16) continue;
                    size = 0;
                    for (int k = 0; k < 8; k++) size = size << 8 | h[8 + k];
                }
                if (size == 0 || size < header.size()) { lost = true; return; } // to the end of the file, or broken
                skip = size - header.size();
                // A moof is only whole with the mdat after it.
                if (std::memcmp(h + 4, "moof", 4) == 0) moof = true;
                else if (std::memcmp(h + 4, "mdat", 4) == 0) moof = false;
            } else { // an id and a size, each a variable-length number
                auto length = [](uint8_t first) {
                    int l = 1;
                    while (l <= 8 && !(first & (0x80 >> (l - 1)))) l++;
                    return l;
                };
                int idLength = length(h[0]);
                if (idLength > 4) { lost = true; return; }
                if ((int)header.size() <= idLength) continue;
                int sizeLength = length(h[idLength]);
                if (sizeLength > 8) { lost = true; return; }
                if ((int)header.size() < idLength + sizeLength) continue;
                uint32_t id = 0;
                for (int k = 0; k < idLength; k++) id = id << 8 | h[k];
                size = h[idLength] & (0xFF >> sizeLength);
                bool unknown = size == (uint64_t)(0xFF >> sizeLength);
                for (int k = 1; k < sizeLength; k++) {
                    size = size << 8 | h[idLength + k];
                    unknown = unknown && h[idLength + k] == 0xFF;
                }
                const bool topLevel = id == 0x1F43B675 || id == 0x1C53BB6B || id == 0x1254C367 || id == 0x114D9B74 ||
                                      id == 0x1549A966 || id == 0x1654AE6B || id == 0x1A45DFA3 || id == 0x18538067;
                if (topLevel && open) { // the Cluster before ended with its last element
                    boundary = pos - header.size();
                    open = false;
                }
                if (unknown) {
                    if (id != 0x1F43B675) { lost = true; return; }
                    open = true; // its elements follow, as pieces of their own
                    size = 0;
                }
                skip = size;
            }
            header.clear();
            if (!skip && !open && !moof) boundary = pos;
        }
    }
};

// One SourceBuffer's appends, written to temporary files as they come
// (mse::recordings): a file for each init segment - each quality - so each
// is playable on its own. Appends go to the file of the init segment last
// appended; it isn't written again.
struct RecordingPart {
    std::wstring path;
    std::string init;
    SegmentTracker tracker;
};
struct BufferRecording {
    std::wstring mimeType;
    std::vector<RecordingPart> parts;
    int current = -1;
    FILE* out = nullptr; // the current part's file
    // Cuts the current part back to its last whole segment (abort(), or
    // another quality coming in the middle of one).
    void settle() {
        if (!out || current < 0) return;
        SegmentTracker& t = parts[current].tracker;
        if (t.lost || t.whole()) return;
        fflush(out);
        _chsize_s(_fileno(out), (long long)t.boundary);
        fseek(out, 0, SEEK_END);
        t.reset(t.webm, t.boundary);
    }
    void close() {
        if (out) fclose(out);
        out = nullptr;
        current = -1;
    }
};

std::wstring recordingDir() {
    wchar_t temp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, temp);
    std::wstring dir = std::wstring(temp, n) + L"WTEngine-media\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    // Once per run: recordings a WTEngine that's no longer running left
    // behind (it crashed, or was killed before its pages closed). Each
    // file's name starts with its process id.
    static bool cleaned = false;
    if (!cleaned) {
        cleaned = true;
        WIN32_FIND_DATAW found;
        HANDLE h = FindFirstFileW((dir + L"*.part").c_str(), &found);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                DWORD pid = wcstoul(found.cFileName, nullptr, 10);
                HANDLE process = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
                DWORD code = 0;
                bool running = process && GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
                if (process) CloseHandle(process);
                if (!running) DeleteFileW((dir + found.cFileName).c_str());
            } while (FindNextFileW(h, &found));
            FindClose(h);
        }
    }
    return dir;
}

void record(BufferRecording& rec, int id, int buffer, const uint8_t* data, size_t length) {
    size_t init = initSegmentLength(data, length);
    if (init > 0) {
        std::string segment(reinterpret_cast<const char*>(data), init);
        int part = -1;
        for (size_t i = 0; i < rec.parts.size(); i++)
            if (rec.parts[i].init == segment) part = (int)i;
        if (part != rec.current || !rec.out) {
            rec.settle();
            rec.close();
            if (part >= 0) { // a quality it had before: back to its file
                rec.out = _wfopen(rec.parts[part].path.c_str(), L"ab");
            } else {
                std::wstring path = recordingDir() + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(id) + L"-" +
                                    std::to_wstring(buffer) + L"-" + std::to_wstring(rec.parts.size()) + L".part";
                rec.out = _wfopen(path.c_str(), L"wb");
                if (rec.out) {
                    fwrite(data, 1, init, rec.out);
                    RecordingPart p;
                    p.path = path;
                    p.init = segment;
                    p.tracker.reset(data[0] == 0x1A, init);
                    rec.parts.push_back(std::move(p));
                    part = (int)rec.parts.size() - 1;
                }
            }
            if (rec.out) rec.current = part;
        }
        data += init;
        length -= init;
    }
    // Media before any init segment can't be played: dropped.
    if (rec.out && length) {
        fwrite(data, 1, length, rec.out);
        rec.parts[rec.current].tracker.feed(data, length);
    }
}

struct MseEntry {
    ComPtr<IMFMediaSourceExtension> source;
    std::vector<ComPtr<IMFSourceBuffer>> buffers; // by index; a removed one is null
    std::vector<BufferRecording> recordings;      // parallel to buffers
    ~MseEntry() {
        for (auto& r : recordings) {
            r.close();
            for (const auto& part : r.parts) DeleteFileW(part.path.c_str());
        }
    }
    MseEntry() = default;
    MseEntry(const MseEntry&) = delete;
    MseEntry& operator=(const MseEntry&) = delete;
};
std::map<int, MseEntry>& mseTable() {
    static std::map<int, MseEntry> table;
    return table;
}
std::map<std::wstring, int>& mseUrls() {
    static std::map<std::wstring, int> urls;
    return urls;
}

ComPtr<IMFMediaSourceExtension> createMse(int id) {
    if (!startMediaFoundation()) return nullptr;
    ComPtr<IMFMediaEngineClassFactoryEx> factory;
    if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        return nullptr;
    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 3))) return nullptr;
    auto notify = Microsoft::WRL::Make<MseNotify>(id);
    attrs->SetUnknown(MF_MSE_CALLBACK, notify.Get());
    static const bool vp9 = hasDecoder(MFT_CATEGORY_VIDEO_DECODER, MFMediaType_Video, MFVideoFormat_VP90);
    static const bool opus = hasDecoder(MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio, MFAudioFormat_Opus);
    attrs->SetUINT32(MF_MSE_VP9_SUPPORT, vp9 ? MF_MSE_VP9_SUPPORT_ON : MF_MSE_VP9_SUPPORT_OFF);
    attrs->SetUINT32(MF_MSE_OPUS_SUPPORT, opus ? MF_MSE_OPUS_SUPPORT_ON : MF_MSE_OPUS_SUPPORT_OFF);
    ComPtr<IMFMediaSourceExtension> source;
    if (FAILED(factory->CreateMediaSourceExtension(0, attrs.Get(), &source))) return nullptr;
    return source;
}

IMFSourceBuffer* sourceBuffer(int id, int buffer) {
    auto it = mseTable().find(id);
    if (it == mseTable().end() || buffer < 0 || buffer >= (int)it->second.buffers.size()) return nullptr;
    return it->second.buffers[buffer].Get();
}

} // namespace

namespace mse {

int create() {
    static int nextId = 1;
    int id = nextId++;
    ComPtr<IMFMediaSourceExtension> source = createMse(id);
    if (!source) return 0;
    mseTable()[id].source = source;
    return id;
}

void destroyAll() {
    mseTable().clear();
    mseUrls().clear();
    std::lock_guard<std::mutex> lock(mseEvents().mutex);
    mseEvents().events.clear();
}

bool isTypeSupported(const std::wstring& type) {
    // Asked of a MediaSource, so one is kept around for the question.
    static ComPtr<IMFMediaSourceExtension> probe = createMse(0);
    if (!probe || type.empty()) return false;
    BSTR t = SysAllocString(type.c_str());
    BOOL ok = probe->IsTypeSupported(t);
    SysFreeString(t);
    return ok != FALSE;
}

int readyState(int id) {
    auto it = mseTable().find(id);
    return it == mseTable().end() ? 1 : (int)it->second.source->GetReadyState();
}

double duration(int id) {
    auto it = mseTable().find(id);
    return it == mseTable().end() ? std::numeric_limits<double>::quiet_NaN() : it->second.source->GetDuration();
}

bool setDuration(int id, double seconds) {
    auto it = mseTable().find(id);
    return it != mseTable().end() && SUCCEEDED(it->second.source->SetDuration(seconds));
}

int addSourceBuffer(int id, const std::wstring& type, std::wstring& error) {
    auto it = mseTable().find(id);
    if (it == mseTable().end()) { error = L"InvalidStateError"; return -1; }
    MseEntry& entry = it->second;
    BSTR t = SysAllocString(type.c_str());
    const bool supported = entry.source->IsTypeSupported(t) != FALSE;
    int index = (int)entry.buffers.size();
    auto notify = Microsoft::WRL::Make<SourceBufferNotify>(id, index);
    ComPtr<IMFSourceBuffer> buffer;
    HRESULT hr = supported ? entry.source->AddSourceBuffer(t, notify.Get(), &buffer) : E_FAIL;
    SysFreeString(t);
    if (!supported) { error = L"NotSupportedError"; return -1; }
    if (FAILED(hr) || !buffer) {
        error = hr == MF_E_INVALIDREQUEST ? L"InvalidStateError" : L"QuotaExceededError";
        return -1;
    }
    entry.buffers.push_back(buffer);
    entry.recordings.emplace_back();
    entry.recordings.back().mimeType = type;
    return index;
}

bool removeSourceBuffer(int id, int buffer) {
    IMFSourceBuffer* b = sourceBuffer(id, buffer);
    if (!b) return false;
    MseEntry& entry = mseTable()[id];
    bool ok = SUCCEEDED(entry.source->RemoveSourceBuffer(b));
    entry.buffers[buffer].Reset();
    return ok;
}

bool endOfStream(int id, int error) {
    auto it = mseTable().find(id);
    MF_MSE_ERROR e = error == 1 ? MF_MSE_ERROR_NETWORK : error == 2 ? MF_MSE_ERROR_DECODE : MF_MSE_ERROR_NOERROR;
    return it != mseTable().end() && SUCCEEDED(it->second.source->SetEndOfStream(e));
}

bool append(int id, int buffer, const uint8_t* data, size_t length) {
    IMFSourceBuffer* b = sourceBuffer(id, buffer);
    if (!b || FAILED(b->Append(data, (DWORD)length))) return false;
    record(mseTable()[id].recordings[buffer], id, buffer, data, length);
    return true;
}

std::vector<Recording> recordings(int id) {
    std::vector<Recording> out;
    auto it = mseTable().find(id);
    if (it == mseTable().end()) return out;
    for (auto& r : it->second.recordings) {
        if (r.out) fflush(r.out); // so a copy has everything appended so far
        Recording rec{ r.mimeType, {} };
        for (const auto& part : r.parts) rec.files.push_back(part.path);
        if (!rec.files.empty()) out.push_back(std::move(rec));
    }
    return out;
}

bool recorded(int id) {
    auto it = mseTable().find(id);
    if (it == mseTable().end()) return false;
    for (const auto& r : it->second.recordings)
        if (!r.parts.empty()) return true;
    return false;
}

bool abort(int id, int buffer) {
    IMFSourceBuffer* b = sourceBuffer(id, buffer);
    if (!b || FAILED(b->Abort())) return false;
    mseTable()[id].recordings[buffer].settle(); // a segment cut off partway isn't kept
    return true;
}

bool remove(int id, int buffer, double start, double end) {
    IMFSourceBuffer* b = sourceBuffer(id, buffer);
    return b && SUCCEEDED(b->Remove(start, end));
}

bool updating(int id, int buffer) {
    IMFSourceBuffer* b = sourceBuffer(id, buffer);
    return b && b->GetUpdating() != FALSE;
}

std::vector<std::pair<double, double>> buffered(int id, int buffer) {
    std::vector<std::pair<double, double>> out;
    IMFSourceBuffer* b = sourceBuffer(id, buffer);
    ComPtr<IMFMediaTimeRange> ranges;
    if (!b || FAILED(b->GetBuffered(&ranges)) || !ranges) return out;
    for (DWORD i = 0, n = ranges->GetLength(); i < n; i++) {
        double s = 0, e = 0;
        ranges->GetStart(i, &s);
        ranges->GetEnd(i, &e);
        out.push_back({ s, e });
    }
    return out;
}

bool getNumber(int id, int buffer, const std::string& name, double& value) {
    IMFSourceBuffer* b = sourceBuffer(id, buffer);
    if (!b) return false;
    if (name == "timestampOffset") value = b->GetTimeStampOffset();
    else if (name == "appendWindowStart") value = b->GetAppendWindowStart();
    else if (name == "appendWindowEnd") value = b->GetAppendWindowEnd();
    else if (name == "mode") {
        ComPtr<IMFSourceBufferAppendMode> mode;
        if (FAILED(b->QueryInterface(IID_PPV_ARGS(&mode)))) return false;
        value = mode->GetAppendMode() == MF_MSE_APPEND_MODE_SEQUENCE ? 1 : 0;
    }
    else return false;
    return true;
}

bool setNumber(int id, int buffer, const std::string& name, double value) {
    IMFSourceBuffer* b = sourceBuffer(id, buffer);
    if (!b) return false;
    if (name == "timestampOffset") return SUCCEEDED(b->SetTimeStampOffset(value));
    if (name == "appendWindowStart") return SUCCEEDED(b->SetAppendWindowStart(value));
    if (name == "appendWindowEnd") return SUCCEEDED(b->SetAppendWindowEnd(value));
    if (name == "mode") {
        ComPtr<IMFSourceBufferAppendMode> mode;
        return SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&mode))) &&
               SUCCEEDED(mode->SetAppendMode(value == 1 ? MF_MSE_APPEND_MODE_SEQUENCE : MF_MSE_APPEND_MODE_SEGMENTS));
    }
    return false;
}

void registerUrl(const std::wstring& url, int id) { mseUrls()[url] = id; }
void revokeUrl(const std::wstring& url) { mseUrls().erase(url); }
int idForUrl(const std::wstring& url) {
    auto it = mseUrls().find(url.substr(0, url.find(L'#')));
    return it == mseUrls().end() ? 0 : it->second;
}

std::vector<Event> takeEvents() {
    std::vector<Event> out;
    std::lock_guard<std::mutex> lock(mseEvents().mutex);
    out.swap(mseEvents().events);
    return out;
}

} // namespace mse


struct MediaPlayer::Impl {
    ComPtr<IMFMediaEngine> engine;
    ComPtr<MseExtension> extension; // opens a MediaSource for the engine (loadMediaSource)
    std::shared_ptr<EventQueue> queue = std::make_shared<EventQueue>();
    // The frame each new video frame is converted into, then copied out of.
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> frame;
    UINT frameW = 0, frameH = 0;
    // An http(s) source: its first piece while it's on its way (see
    // update), then the pieces the stream has fetched.
    std::shared_ptr<Probe> probe;
    std::wstring referrer;
    std::shared_ptr<ChunkStore> store;

    // Reads blocked on the network give up, so nothing waits on them.
    void closeStore() {
        if (!store) return;
        std::lock_guard<std::mutex> lock(store->mutex);
        store->closed = true;
        store->cv.notify_all();
    }
    void setSource(const std::wstring& url) {
        BSTR src = SysAllocString(url.c_str());
        engine->SetSource(src); // copies it, and starts loading
        SysFreeString(src);
    }
};

MediaPlayer::MediaPlayer() : impl_(std::make_unique<Impl>()) {}

MediaPlayer::~MediaPlayer() {
    // Stops playback and its worker threads; the engine can't be used after.
    if (!impl_) return;
    impl_->closeStore();
    if (impl_->engine) impl_->engine->Shutdown();
}

std::unique_ptr<MediaPlayer> MediaPlayer::create() {
    if (!startMediaFoundation()) return nullptr;

    ComPtr<IMFMediaEngineClassFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))))
        return nullptr;

    std::unique_ptr<MediaPlayer> player(new MediaPlayer());
    Impl& impl = *player->impl_;
    ComPtr<Notify> notify;
    notify.Attach(new Notify(impl.queue));

    // Frame-server mode: with an output format and no window, the engine
    // draws nothing itself and hands each frame over (TransferVideoFrame).
    // Without a DXGI device manager it decodes in software.
    ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 3))) return nullptr;
    attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify.Get());
    attrs->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, kFrameFormat);
    impl.extension = Microsoft::WRL::Make<MseExtension>();
    attrs->SetUnknown(MF_MEDIA_ENGINE_EXTENSION, impl.extension.Get());
    if (FAILED(factory->CreateInstance(0, attrs.Get(), &impl.engine))) return nullptr;

    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&impl.wic))))
        return nullptr;
    return player;
}

bool MediaPlayer::loadMediaSource(int mseId) {
    auto it = mseTable().find(mseId);
    if (it == mseTable().end()) return false;
    Impl& impl = *impl_;
    url_ = L"wtmse:" + std::to_wstring(mseId);
    impl.closeStore();
    impl.store.reset();
    impl.probe.reset();
    impl.extension->setPending(it->second.source);
    impl.setSource(url_); // the engine asks the extension to open it, and the MediaSource opens (sourceopen)
    return true;
}

void MediaPlayer::load(const std::wstring& url, const std::wstring& referrer) {
    Impl& impl = *impl_;
    url_ = url;
    impl.closeStore();
    impl.store.reset();
    impl.probe.reset();
    impl.engine->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
    if (!isHttpUrl(url)) { // a local file: the media engine reads it directly
        impl.setSource(url);
        return;
    }
    // The first piece first (update() takes it from there).
    auto probe = std::make_shared<Probe>();
    impl.probe = probe;
    impl.referrer = referrer;
    HttpRequest req;
    req.url = url;
    req.referrer = referrer;
    req.headers.push_back({ "Range", "bytes=0-" + std::to_string(kChunk - 1) });
    std::weak_ptr<Probe> weak = probe;
    fetchHttpAsync(std::move(req), [weak](HttpResponse res) {
        if (auto p = weak.lock()) {
            std::lock_guard<std::mutex> lock(p->mutex);
            p->response = std::move(res);
            p->done = true;
        }
    });
}

void MediaPlayer::update() {
    Impl& impl = *impl_;
    if (!impl.probe) return;
    HttpResponse res;
    {
        std::lock_guard<std::mutex> lock(impl.probe->mutex);
        if (!impl.probe->done) return;
        res = std::move(impl.probe->response);
    }
    impl.probe.reset();

    // The file's length: from Content-Range ("bytes 0-1048575/5510872")
    // when the server took the Range request, or the whole body when it
    // sent all of it.
    QWORD length = 0;
    if (res.ok && res.status == 206) {
        for (const auto& [name, value] : res.headers) {
            if (name != "content-range") continue;
            size_t slash = value.rfind('/');
            if (slash != std::string::npos) length = std::strtoull(value.c_str() + slash + 1, nullptr, 10);
        }
    }
    else if (res.ok && res.status == 200) {
        length = res.body.size();
    }

    ComPtr<IMFMediaEngineEx> ex;
    if (length == 0 || res.body.empty() || FAILED(impl.engine.As(&ex))) {
        // Not something the stream can serve (an error status, a server
        // that ignores Range for a big file): the media engine fetches it
        // itself, and reports what's wrong as usual.
        impl.setSource(url_);
        return;
    }
    impl.store = std::make_shared<ChunkStore>();
    for (QWORD i = 0; i * kChunk < res.body.size() && i * kChunk < length; i++) {
        impl.store->chunks[i] = res.body.substr((size_t)(i * kChunk), (size_t)kChunk);
        impl.store->lastUse[i] = ++impl.store->clock;
    }
    auto stream = Microsoft::WRL::Make<HttpByteStream>(url_, impl.referrer, length, impl.store);
    BSTR hint = SysAllocString(hintUrl(url_, res.contentType).c_str());
    HRESULT hr = stream ? ex->SetSourceFromByteStream(stream.Get(), hint) : E_OUTOFMEMORY;
    SysFreeString(hint);
    if (FAILED(hr)) {
        impl.closeStore();
        impl.store.reset();
        impl.setSource(url_);
    }
}

void MediaPlayer::play() { impl_->engine->Play(); }
void MediaPlayer::pause() { impl_->engine->Pause(); }
void MediaPlayer::setAutoPlay(bool on) { impl_->engine->SetAutoPlay(on); }
bool MediaPlayer::paused() const { return impl_->engine->IsPaused() != FALSE; }
bool MediaPlayer::ended() const { return impl_->engine->IsEnded() != FALSE; }
bool MediaPlayer::seeking() const { return impl_->engine->IsSeeking() != FALSE; }
double MediaPlayer::currentTime() const { return impl_->engine->GetCurrentTime(); }
void MediaPlayer::setCurrentTime(double seconds) { impl_->engine->SetCurrentTime(seconds); }
double MediaPlayer::volume() const { return impl_->engine->GetVolume(); }
void MediaPlayer::setVolume(double v) { impl_->engine->SetVolume(v); }
bool MediaPlayer::muted() const { return impl_->engine->GetMuted() != FALSE; }
void MediaPlayer::setMuted(bool m) { impl_->engine->SetMuted(m); }
bool MediaPlayer::loop() const { return impl_->engine->GetLoop() != FALSE; }
void MediaPlayer::setLoop(bool l) { impl_->engine->SetLoop(l); }
double MediaPlayer::playbackRate() const { return impl_->engine->GetPlaybackRate(); }
void MediaPlayer::setPlaybackRate(double r) { impl_->engine->SetPlaybackRate(r); }
int MediaPlayer::readyState() const { return (int)impl_->engine->GetReadyState(); }
int MediaPlayer::networkState() const { return (int)impl_->engine->GetNetworkState(); }

double MediaPlayer::duration() const {
    if (impl_->engine->GetReadyState() == MF_MEDIA_ENGINE_READY_HAVE_NOTHING)
        return std::numeric_limits<double>::quiet_NaN();
    return impl_->engine->GetDuration(); // NaN / infinity as HTMLMediaElement uses them
}

double MediaPlayer::bufferedEnd() const {
    ComPtr<IMFMediaTimeRange> ranges;
    if (FAILED(impl_->engine->GetBuffered(&ranges)) || !ranges) return 0;
    DWORD n = ranges->GetLength();
    double end = 0;
    if (n > 0) ranges->GetEnd(n - 1, &end);
    return end;
}

int MediaPlayer::errorCode() const {
    ComPtr<IMFMediaError> error;
    if (FAILED(impl_->engine->GetError(&error)) || !error) return 0;
    return (int)error->GetErrorCode(); // MF_MEDIA_ENGINE_ERR_* are MediaError's codes
}

bool MediaPlayer::videoSize(int& width, int& height) const {
    DWORD w = 0, h = 0;
    if (!impl_->engine->HasVideo() || FAILED(impl_->engine->GetNativeVideoSize(&w, &h)) || !w || !h) return false;
    width = (int)w;
    height = (int)h;
    return true;
}

bool MediaPlayer::readFrame(std::vector<uint8_t>& bgra, int& width, int& height) {
    Impl& impl = *impl_;
    LONGLONG pts;
    if (impl.engine->OnVideoStreamTick(&pts) != S_OK) return false; // no new frame
    DWORD w = 0, h = 0;
    if (FAILED(impl.engine->GetNativeVideoSize(&w, &h)) || !w || !h) return false;

    if (!impl.frame || w != impl.frameW || h != impl.frameH) {
        impl.frame.Reset();
        if (FAILED(impl.wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppBGRA, WICBitmapCacheOnLoad, &impl.frame)))
            return false;
        impl.frameW = w;
        impl.frameH = h;
    }
    MFVideoNormalizedRect whole{ 0, 0, 1, 1 };
    RECT dst{ 0, 0, (LONG)w, (LONG)h };
    MFARGB black{ 0, 0, 0, 255 };
    if (FAILED(impl.engine->TransferVideoFrame(impl.frame.Get(), &whole, &dst, &black))) return false;

    WICRect all{ 0, 0, (INT)w, (INT)h };
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(impl.frame->Lock(&all, WICBitmapLockRead, &lock))) return false;
    UINT stride = 0, size = 0;
    BYTE* data = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data)) || !data) return false;

    const size_t row = (size_t)w * 4;
    bgra.resize(row * h);
    if (stride == row) std::memcpy(bgra.data(), data, row * h);
    else for (DWORD y = 0; y < h; y++) std::memcpy(bgra.data() + y * row, data + (size_t)y * stride, row);
    width = (int)w;
    height = (int)h;
    return true;
}

std::vector<std::wstring> MediaPlayer::takeEvents() {
    std::vector<std::wstring> out;
    std::lock_guard<std::mutex> lock(impl_->queue->mutex);
    out.swap(impl_->queue->events);
    return out;
}

std::wstring MediaPlayer::canPlayType(const std::wstring& mimeType) const {
    BSTR type = SysAllocString(mimeType.c_str());
    MF_MEDIA_ENGINE_CANPLAY answer = MF_MEDIA_ENGINE_CANPLAY_NOT_SUPPORTED;
    HRESULT hr = impl_->engine->CanPlayType(type, &answer);
    SysFreeString(type);
    if (FAILED(hr)) return L"";
    if (answer == MF_MEDIA_ENGINE_CANPLAY_PROBABLY) return L"probably";
    if (answer == MF_MEDIA_ENGINE_CANPLAY_MAYBE) return L"maybe";
    return L"";
}

std::wstring mediaCanPlayType(const std::wstring& mimeType) {
    // Asked of a media engine, so one is kept around for these questions.
    static std::unique_ptr<MediaPlayer> probe = MediaPlayer::create();
    if (!probe || mimeType.empty()) return L"";
    return probe->canPlayType(mimeType);
}
