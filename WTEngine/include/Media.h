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
