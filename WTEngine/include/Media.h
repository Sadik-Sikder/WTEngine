// Media.h
#pragma once
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

// One <video>/<audio> element's playback, on Windows Media Foundation's
// media engine: it fetches the URL itself (file:, http:, https:), decodes,
// keeps time and plays the sound. Video frames are copied out (readFrame)
// for the renderer to draw. The methods follow HTMLMediaElement's; what
// happens comes back as DOM event names (takeEvents) for the page's
// scripts. Windows headers stay in Media.cpp.
class MediaPlayer {
public:
    // nullptr if Media Foundation isn't available (an N edition of Windows
    // without the Media Feature Pack).
    static std::unique_ptr<MediaPlayer> create();
    ~MediaPlayer();

    // Plays MediaSource `mseId` (mse::create's) - for a <video> whose src is
    // the MediaSource's blob: URL. False if there's no such MediaSource.
    bool loadMediaSource(int mseId);

    // Starts loading `url` (absolute), dropping whatever was loaded before.
    // An http(s) URL is fetched through Fetcher, sending the cookies and
    // `referrer` (the page) as any other request does; its first piece
    // arrives in the background, and update() hands it to the player.
    void load(const std::wstring& url, const std::wstring& referrer = L"");
    // Call once a frame (from the thread that calls load).
    void update();
    const std::wstring& url() const { return url_; }

    void play();
    void pause();
    // Start playing on its own once enough has loaded (the autoplay attribute).
    void setAutoPlay(bool on);
    bool paused() const;
    bool ended() const;
    bool seeking() const;
    double currentTime() const;
    void setCurrentTime(double seconds);
    double duration() const; // NaN until known; +Infinity for a live stream
    double volume() const;
    void setVolume(double v); // 0..1
    bool muted() const;
    void setMuted(bool m);
    bool loop() const;
    void setLoop(bool l);
    double playbackRate() const;
    void setPlaybackRate(double r);
    // HTMLMediaElement's readyState (0 nothing .. 4 enough data) and
    // networkState (0 empty .. 3 no source).
    int readyState() const;
    int networkState() const;
    // How far the download has got, in seconds (the end of the buffered range).
    double bufferedEnd() const;
    // MediaError's code (1 aborted .. 4 source not supported), 0 for none.
    int errorCode() const;

    // The video's natural size; false until known (or for audio only).
    bool videoSize(int& width, int& height) const;

    // If a frame newer than the last one read is ready, copies it into
    // `bgra` (width * height * 4 bytes, rows top to bottom) and returns
    // true. Call from the thread that draws.
    bool readFrame(std::vector<uint8_t>& bgra, int& width, int& height);

    // The DOM events ("loadedmetadata", "play", "timeupdate", ...) that
    // happened since the last call, oldest first.
    std::vector<std::wstring> takeEvents();

    // Whether this media engine plays `mimeType` - see mediaCanPlayType.
    std::wstring canPlayType(const std::wstring& mimeType) const;

private:
    MediaPlayer();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::wstring url_;
};

// HTMLMediaElement.canPlayType: "probably", "maybe" or "".
std::wstring mediaCanPlayType(const std::wstring& mimeType);

// Media Source Extensions: a MediaSource, in which a page's script builds
// the stream itself - appending the segments it fetched to SourceBuffers -
// played by a <video> (MediaPlayer::loadMediaSource). Windows' own MSE
// (IMFMediaSourceExtension) does the demuxing and buffering. Each
// MediaSource is known by an id; call everything from the main thread.
// What happens comes back as events (takeEvents), as for MediaPlayer.
namespace mse {
// A new MediaSource; 0 if MSE isn't available.
int create();
// Gone with its page: every MediaSource, and the blob: URLs naming them.
void destroyAll();
// MediaSource.isTypeSupported - "video/mp4; codecs=\"avc1.42E01E\"".
bool isTypeSupported(const std::wstring& type);
// 1 closed, 2 open, 3 ended (MediaSource.readyState).
int readyState(int id);
double duration(int id);
bool setDuration(int id, double seconds);
// A new SourceBuffer's index, or -1 with `error` set: "NotSupportedError"
// (the type), "QuotaExceededError" (no more buffers) or "InvalidStateError".
int addSourceBuffer(int id, const std::wstring& type, std::wstring& error);
bool removeSourceBuffer(int id, int buffer);
// 0 done, 1 network error, 2 decode error.
bool endOfStream(int id, int error);
// One SourceBuffer's operations. append copies the bytes; it, abort and
// remove report back with updatestart/update/updateend (or error/abort).
bool append(int id, int buffer, const uint8_t* data, size_t length);
bool abort(int id, int buffer);
bool remove(int id, int buffer, double start, double end);
bool updating(int id, int buffer);
std::vector<std::pair<double, double>> buffered(int id, int buffer);
// timestampOffset, appendWindowStart, appendWindowEnd; mode 0 segments, 1 sequence.
bool getNumber(int id, int buffer, const std::string& name, double& value);
bool setNumber(int id, int buffer, const std::string& name, double value);
// blob: URLs naming a MediaSource (URL.createObjectURL), for a <video src>.
void registerUrl(const std::wstring& url, int id);
void revokeUrl(const std::wstring& url);
int idForUrl(const std::wstring& url); // 0 if none

struct Event {
    int id;
    int buffer; // -1: the MediaSource itself
    std::wstring name;
};
std::vector<Event> takeEvents();

// What each SourceBuffer of MediaSource `id` was given, for saving it
// (Engine::saveMedia): its MIME type and the temporary files its appends
// went to - one per init segment (each quality), holding it and then the
// media segments in the order they came, which mux::merge puts in time
// order. Only what the page fetched is there: the parts that were played,
// or buffered ahead. Deleted with the page.
struct Recording {
    std::wstring mimeType;
    std::vector<std::wstring> files;
};
std::vector<Recording> recordings(int id);
// Whether MediaSource `id` has recorded anything yet (cheap, unlike recordings()).
bool recorded(int id);
} // namespace mse
