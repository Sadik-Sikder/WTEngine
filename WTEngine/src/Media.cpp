#include "Media.h"
#include "Fetcher.h"

#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <wrl/implements.h>
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

} // namespace

struct MediaPlayer::Impl {
    ComPtr<IMFMediaEngine> engine;
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
    if (FAILED(MFCreateAttributes(&attrs, 2))) return nullptr;
    attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify.Get());
    attrs->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, kFrameFormat);
    if (FAILED(factory->CreateInstance(0, attrs.Get(), &impl.engine))) return nullptr;

    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&impl.wic))))
        return nullptr;
    return player;
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
