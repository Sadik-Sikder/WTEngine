// <video> and <audio>: each media element's player (Media.h), drawing its
// frames, poster and controls, fullscreen, and what scripts see of it.
#include "Engine.h"
#include "Fetcher.h"
#include "JSEngine.h"
#include "MediaMux.h"
#include "Renderer.h"
#include <cmath>
#include <cstdio>
#include <share.h> // _SH_DENYNO: _wfsopen lets others read a file being written
#include <algorithm>
#include <cwctype>
#include <functional>
#include <set>
#include <thread>
#define NOMINMAX
#include <windows.h>
#include <shlobj.h> // SHGetKnownFolderPath: the Downloads folder
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace {

bool hasAttr(const Element* el, const wchar_t* name) {
    return el->attrs.count(name) != 0;
}

std::wstring attrValue(const Element* el, const wchar_t* name) {
    auto it = el->attrs.find(name);
    return it == el->attrs.end() ? L"" : it->second;
}

// What a media element plays: its src attribute, else its first <source>
// child of a type the player can play (or that doesn't say).
std::wstring mediaSource(const Element* el) {
    auto src = el->attrs.find(L"src");
    if (src != el->attrs.end()) return src->second;
    for (const auto& child : el->children) {
        if (child->type != Node::ELEMENT) continue;
        const auto* s = static_cast<const Element*>(child.get());
        if (s->tag != L"source") continue;
        std::wstring url = attrValue(s, L"src");
        if (url.empty()) continue;
        std::wstring type = attrValue(s, L"type");
        if (!type.empty() && mediaCanPlayType(type).empty()) continue;
        return url;
    }
    return L"";
}

// The first <video> at or under `el`.
Element* findVideo(Element* el) {
    if (el->tag == L"video") return el;
    for (const auto& child : el->children) {
        if (child->type != Node::ELEMENT) continue;
        if (Element* v = findVideo(static_cast<Element*>(child.get()))) return v;
    }
    return nullptr;
}

// The largest rectangle of the picture's proportions that fits the box,
// centred in it - how a video sits in its element (letterboxed).
void containIn(float x, float y, float w, float h, int pw, int ph, float out[4]) {
    float s = std::min(w / pw, h / ph);
    out[2] = pw * s;
    out[3] = ph * s;
    out[0] = x + (w - out[2]) / 2;
    out[1] = y + (h - out[3]) / 2;
}

// "1:05", or "1:02:03" from an hour on.
std::wstring clockTime(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) seconds = 0;
    long t = (long)seconds;
    wchar_t buf[32];
    if (t >= 3600) swprintf(buf, 32, L"%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
    else swprintf(buf, 32, L"%ld:%02ld", t / 60, t % 60);
    return buf;
}

// Where the controls' parts are, for drawing, clicks and the cursor alike:
// a bar along the bottom of a video (all of an <audio>'s box) with
// play/pause on the left, then the time and the progress track, and on the
// right mute, a volume slider (if there's room) and, for a video,
// fullscreen.
struct ControlsLayout {
    float barY, barH;
    float playX;           // play/pause button, kButton wide
    float timeX;           // the time; none if the box is narrow
    bool showTime;
    float trackX, trackW;  // the progress track
    float muteX;           // mute button
    float volumeX, volumeW; // volume slider; volumeW 0 = none
    float fullscreenX;     // fullscreen button; negative = none (audio)
};
constexpr float kButton = 28;

ControlsLayout controlsLayout(const LayoutBox& b, float top) {
    ControlsLayout c;
    const bool audio = b.el->tag == L"audio";
    c.barH = audio ? (float)b.height : std::min(40.0f, (float)b.height);
    c.barY = top + b.height - c.barH;
    c.playX = b.x + 6.0f;
    c.showTime = b.width >= 220;
    c.timeX = c.playX + kButton + 6;
    float right = b.x + b.width - 6.0f;
    c.fullscreenX = -1;
    if (!audio) {
        c.fullscreenX = right - kButton;
        right = c.fullscreenX - 2;
    }
    c.volumeW = b.width >= 320 ? 52.0f : 0.0f;
    c.volumeX = right - c.volumeW;
    if (c.volumeW > 0) right = c.volumeX - 6;
    c.muteX = right - kButton;
    c.trackX = c.timeX + (c.showTime ? 92 : 0);
    c.trackW = std::max(c.muteX - 10 - c.trackX, 0.0f);
    return c;
}

enum class MediaPart { Video, Play, Time, Track, Mute, Volume, Fullscreen };

MediaPart partAt(const ControlsLayout& c, float x, float y) {
    if (y < c.barY) return MediaPart::Video;
    if (c.fullscreenX >= 0 && x >= c.fullscreenX && x < c.fullscreenX + kButton) return MediaPart::Fullscreen;
    if (c.volumeW > 0 && x >= c.volumeX - 4 && x <= c.volumeX + c.volumeW + 4) return MediaPart::Volume;
    if (x >= c.muteX && x < c.muteX + kButton) return MediaPart::Mute;
    if (c.trackW > 0 && x >= c.trackX - 6 && x <= c.trackX + c.trackW + 6) return MediaPart::Track;
    if (x < c.playX + kButton) return MediaPart::Play;
    return MediaPart::Time; // the time, or a gap between parts
}

// A slider: its track, filled up to `fraction`, and a knob there.
void drawSlider(Renderer& r, float x, float mid, float w, double fraction, Color ink, float knob) {
    const float radii[4] = { 2, 2, 2, 2 };
    Color faint = ink;
    faint.a = 0.3f;
    r.drawRoundedRect(x, mid - 2, w, 4, radii, faint);
    float at = x + (float)(w * std::clamp(fraction, 0.0, 1.0));
    if (at > x) r.drawRoundedRect(x, mid - 2, at - x, 4, radii, ink);
    const float k[4] = { knob / 2, knob / 2, knob / 2, knob / 2 };
    r.drawRoundedRect(at - knob / 2, mid - knob / 2, knob, knob, k, ink);
}

// The fullscreen button's icon: four corners of a frame - pointing out to
// enter fullscreen, in to leave it.
void drawCorners(Renderer& r, float x, float y, float w, float h, bool inward, Color ink) {
    const float t = 2, len = 5;
    struct Corner { float x, y, dx, dy; };
    Corner corners[4] = { { x, y, 1, 1 }, { x + w, y, -1, 1 }, { x, y + h, 1, -1 }, { x + w, y + h, -1, -1 } };
    for (Corner c : corners) {
        if (inward) {
            c.x += c.dx * len;
            c.y += c.dy * len;
            c.dx = -c.dx;
            c.dy = -c.dy;
        }
        r.drawRect(c.dx > 0 ? c.x : c.x - len, c.dy > 0 ? c.y : c.y - t, len, t, ink);
        r.drawRect(c.dx > 0 ? c.x : c.x - t, c.dy > 0 ? c.y : c.y - len, t, len, ink);
    }
}

const std::wstring kSymbolFont = L"Segoe UI Symbol";

// How long a playing video's controls stay up after the mouse last moved.
constexpr auto kControlsLinger = std::chrono::seconds(3);

} // namespace

std::shared_ptr<Node> Engine::ownerOf(Element* el) {
    if (el->parent)
        for (auto& c : el->parent->children) if (c.get() == el) return c;
    return jsContext() ? owningNode(jsContext(), el) : nullptr;
}

Engine::MediaElement* Engine::mediaFor(Element* el, bool create) {
    if (!el || (el->tag != L"video" && el->tag != L"audio")) return nullptr;
    auto it = media_.find(el);
    if (it == media_.end()) {
        if (!create) return nullptr;
        // The player holds its element, so it can't outlive it.
        std::shared_ptr<Node> owner = ownerOf(el);
        if (!owner) return nullptr;
        std::unique_ptr<MediaPlayer> player = MediaPlayer::create();
        if (!player) return nullptr;
        if (hasAttr(el, L"muted")) player->setMuted(true); // the attribute is only the default
        MediaElement m;
        m.node = std::move(owner);
        m.player = std::move(player);
        m.inPage = inPage(el);
        it = media_.emplace(el, std::move(m)).first;
    }

    // A new source - src set, or <source> children changed - is loaded.
    MediaElement& m = it->second;
    std::wstring raw = mediaSource(el);
    // A blob: URL (a MediaSource's) is taken as it is; resolveUrl only knows http(s).
    std::wstring url = raw.empty() ? L"" : raw.rfind(L"blob:", 0) == 0 ? raw : resolveUrl(pageBaseUrl, raw);
    if (url != m.src) {
        m.src = url;
        m.loaded = false;
        m.everPlayed = false;
        m.hasFrame = false;
        if (measurer) measurer->releasePixels(m.player.get());
        // preload="none" waits until it's played.
        if (!url.empty() && (attrValue(el, L"preload") != L"none" || hasAttr(el, L"autoplay"))) startMediaLoad(m);
    }
    return &m;
}

void Engine::startMediaLoad(MediaElement& m) {
    if (m.loaded || m.src.empty()) return;
    m.loaded = true;
    // autoplay: starts once enough has loaded, as in browsers (play, then
    // playing, after canplay).
    m.player->setAutoPlay(hasAttr(static_cast<Element*>(m.node.get()), L"autoplay"));
    // From a web page, its requests carry the page as their referrer.
    bool webPage = pageBaseUrl.rfind(L"http://", 0) == 0 || pageBaseUrl.rfind(L"https://", 0) == 0;
    // A MediaSource's blob: URL plays the stream the page builds in it.
    if (int mseId = mse::idForUrl(m.src)) { m.player->loadMediaSource(mseId); return; }
    m.player->load(m.src, webPage ? pageBaseUrl : L"");
}

bool Engine::mediaProperty(Element* el, const std::string& name, double& out) {
    MediaElement* m = mediaFor(el, false);
    if (!m) return false;
    MediaPlayer& p = *m->player;
    if (name == "currentTime") out = p.currentTime();
    else if (name == "duration") out = p.duration();
    else if (name == "paused") out = p.paused();
    else if (name == "ended") out = p.ended();
    else if (name == "seeking") out = p.seeking();
    else if (name == "volume") out = p.volume();
    else if (name == "muted") out = p.muted();
    else if (name == "playbackRate") out = p.playbackRate();
    else if (name == "readyState") out = m->loaded ? p.readyState() : 0;
    else if (name == "networkState") out = m->loaded ? p.networkState() : m->src.empty() ? 3 : 1;
    else if (name == "bufferedEnd") out = p.bufferedEnd();
    else if (name == "error") out = p.errorCode();
    else if (name == "videoWidth" || name == "videoHeight") {
        int w = 0, h = 0;
        p.videoSize(w, h);
        out = name == "videoWidth" ? w : h;
    }
    else return false;
    return true;
}

void Engine::setMediaProperty(Element* el, const std::string& name, double value) {
    if (name == "load") { // load(): start over, even with the same source
        auto it = media_.find(el);
        if (it != media_.end()) it->second.src = L"\x01"; // matches no real URL, so mediaFor reloads
    }
    MediaElement* m = mediaFor(el, true);
    if (!m) return;
    MediaPlayer& p = *m->player;
    if (name == "play") {
        startMediaLoad(*m);
        p.play();
    }
    else if (name == "pause") p.pause();
    else if (name == "currentTime") p.setCurrentTime(std::max(value, 0.0));
    else if (name == "volume") p.setVolume(std::clamp(value, 0.0, 1.0));
    else if (name == "muted") p.setMuted(value != 0);
    else if (name == "playbackRate") p.setPlaybackRate(value);
}

void Engine::pollMedia() {
    // Gathered first, fired after: a listener can create players (media_
    // grows) or drop elements from the page. Each keeps its node alive.
    struct Fired {
        std::shared_ptr<Node> node;
        std::vector<std::wstring> events;
    };
    std::vector<Fired> fired;
    bool sizeChanged = false;
    for (auto& [key, m] : media_) {
        auto* el = static_cast<Element*>(m.node.get());
        m.player->update();
        bool loop = hasAttr(el, L"loop");
        if (m.player->loop() != loop) m.player->setLoop(loop);

        std::vector<std::wstring> events = m.player->takeEvents();
        if (events.empty()) continue;
        for (const auto& e : events) {
            if (e == L"loadedmetadata" || e == L"resize") sizeChanged = true;
            if (e == L"play") m.everPlayed = true;
        }
        fired.push_back({ m.node, std::move(events) });
    }
    // A video's own size arrived: it may size its box (makeImageBox).
    if (sizeChanged) {
        domState.changeCount++;
        domState.domDirty = true;
    }

    std::vector<std::shared_ptr<Node>> fullscreenEvents;
    fullscreenEvents.swap(fullscreenEvents_);
    if (JSContext* ctx = jsContext()) {
        for (const auto& f : fired) {
            auto* el = static_cast<Element*>(f.node.get());
            for (const auto& e : f.events) fireEvent(ctx, el, e.c_str(), false, false);
        }
        // fullscreenchange at the element that entered or left fullscreen,
        // bubbling to document - or at document, if it left the page.
        for (const auto& node : fullscreenEvents) {
            auto* el = static_cast<Element*>(node.get());
            Element* target = inPage(el) ? el : document ? document->body.get() : nullptr;
            fireEvent(ctx, target, L"fullscreenchange", true, false);
            fireEvent(ctx, target, L"webkitfullscreenchange", true, false);
        }
    }

    // Taken out of the page (by any script this frame): a media element
    // stops, and a fullscreen one leaves fullscreen, as in browsers. Only a
    // DOM change can have done that.
    if (domState.domDirty) {
        for (auto& [key, m] : media_) {
            bool now = inPage(static_cast<Element*>(m.node.get()));
            if (m.inPage && !now) m.player->pause();
            m.inPage = now;
        }
        if (fullscreen_ && !inPage(static_cast<Element*>(fullscreen_.get()))) setFullscreen(nullptr);
    }
}

void Engine::clearMedia() {
    for (auto& [key, m] : media_) if (measurer) measurer->releasePixels(m.player.get());
    media_.clear();
    mediaDrag_ = {};
    if (fullscreen_) { // the window goes back to normal
        fullscreen_.reset();
        fullscreenChanged_ = true;
    }
    fullscreenEvents_.clear();
    mse::destroyAll(); // the page's MediaSources, with the players that played them
}

// --- Fullscreen -----------------------------------------------------------

void Engine::setFullscreen(Element* el) {
    Element* current = fullscreen_ ? static_cast<Element*>(fullscreen_.get()) : nullptr;
    if (el == current) return;
    std::shared_ptr<Node> owner;
    if (el && !(owner = ownerOf(el))) return;
    if (fullscreen_) fullscreenEvents_.push_back(fullscreen_); // fired next frame (pollMedia)
    fullscreen_ = std::move(owner);
    if (fullscreen_) fullscreenEvents_.push_back(fullscreen_);
    domState.fullscreenElement = el;
    fullscreenChanged_ = true;
}

bool Engine::takeFullscreenChange(bool& on) {
    if (!fullscreenChanged_) return false;
    fullscreenChanged_ = false;
    on = fullscreen_ != nullptr;
    return true;
}

Element* Engine::fullscreenVideo() const {
    if (!fullscreen_) return nullptr;
    auto* el = static_cast<Element*>(fullscreen_.get());
    // The whole page in fullscreen is just the page; a player's container
    // in fullscreen shows the video in it.
    if (el->tag == L"html" || el->tag == L"body") return nullptr;
    return findVideo(el);
}

bool Engine::fullscreenVideoBox(LayoutBox& out) const {
    Element* video = fullscreenVideo();
    if (!video) return false;
    out = LayoutBox{};
    out.x = 0;
    out.y = scrollY; // the view's top, in page coordinates
    out.width = (int)(width / zoom_);
    out.height = viewHeight();
    out.el = video;
    out.media = true;
    return true;
}

// --- Drawing ---------------------------------------------------------------

void Engine::drawMedia(Renderer& renderer, const LayoutBox& b, float screenY, bool fullscreen) {
    Element* el = b.el;
    MediaElement* m = mediaFor(el, false);

    if (el->tag == L"video") {
        if (m) {
            int w, h;
            if (m->player->readFrame(m->frame, w, h)) {
                renderer.uploadPixels(m->player.get(), m->frame.data(), w, h);
                m->hasFrame = true;
                m->frameW = w;
                m->frameH = h;
            }
        }
        // The poster, until playing starts; then the video's frames.
        std::wstring poster = attrValue(el, L"poster");
        bool showPoster = !poster.empty() && (!m || !m->hasFrame || !m->everPlayed);
        float r[4];
        if (showPoster) {
            std::wstring src = resolveUrl(pageBaseUrl, poster);
            int pw, ph;
            if (!src.empty() && renderer.preloadImage(src, pw, ph) && pw > 0 && ph > 0) {
                containIn((float)b.x, screenY, (float)b.width, (float)b.height, pw, ph, r);
                renderer.drawImage(r[0], r[1], r[2], r[3], src);
            }
        }
        else if (m && m->hasFrame) {
            containIn((float)b.x, screenY, (float)b.width, (float)b.height, m->frameW, m->frameH, r);
            renderer.drawPixels(m->player.get(), r[0], r[1], r[2], r[3]);
        }
    }

    // In fullscreen there are always controls: there's no page left to
    // offer any.
    if (fullscreen || hasAttr(el, L"controls")) drawMediaControls(renderer, b, screenY, m, fullscreen);
}

void Engine::drawMediaControls(Renderer& renderer, const LayoutBox& b, float top, MediaElement* m, bool fullscreen) {
    const bool audio = b.el->tag == L"audio";
    const bool paused = !m || m->player->paused();
    // A playing video's controls show only while the mouse moves over it
    // (or a knob is being dragged).
    const bool over = fullscreen || mouseTarget_ == b.el;
    const bool moving = std::chrono::steady_clock::now() - lastMouseMove_ < kControlsLinger;
    const bool dragging = mediaDrag_.node.get() == b.el;
    if (!audio && !paused && !(over && moving) && !dragging) return;

    const ControlsLayout c = controlsLayout(b, top);
    const float mid = c.barY + c.barH / 2;
    Color ink;
    if (audio) {
        float radius = std::min(c.barH / 2, 27.0f);
        const float radii[4] = { radius, radius, radius, radius };
        renderer.drawRoundedRect((float)b.x, c.barY, (float)b.width, c.barH, radii, { 0.945f, 0.953f, 0.957f, 1 });
        ink = { 0.13f, 0.13f, 0.13f, 1 };
    }
    else {
        renderer.drawRect((float)b.x, c.barY, (float)b.width, c.barH, { 0, 0, 0, 0.45f });
        ink = { 1, 1, 1, 1 };
        // A big play button in the middle while paused.
        if (paused && b.height >= 100) {
            const float d = 56, radii[4] = { d / 2, d / 2, d / 2, d / 2 };
            float cx = b.x + b.width / 2.0f, cy = top + (b.height - c.barH) / 2;
            renderer.drawRoundedRect(cx - d / 2, cy - d / 2, d, d, radii, { 0, 0, 0, 0.55f });
            renderer.drawText(cx - 9, cy - 17, L"\u25B6", 26, ink, false, false, &kSymbolFont);
        }
    }

    // Play / pause.
    if (paused) {
        renderer.drawText(c.playX + 7, mid - 11, L"\u25B6", 16, ink, false, false, &kSymbolFont);
    }
    else {
        renderer.drawRect(c.playX + 8, mid - 7, 4, 14, ink);
        renderer.drawRect(c.playX + 16, mid - 7, 4, 14, ink);
    }

    double now = m ? m->player->currentTime() : 0;
    double duration = m ? m->player->duration() : NAN;
    if (c.showTime)
        renderer.drawText(c.timeX, mid - 9, clockTime(now) + L" / " + clockTime(duration), 12, ink);

    // The progress track: what has downloaded in a lighter shade, what has
    // played in full, and a knob where it's at.
    if (c.trackW > 0) {
        bool known = m && duration > 0 && std::isfinite(duration);
        if (known) {
            const float radii[4] = { 2, 2, 2, 2 };
            Color buffered = ink;
            buffered.a = 0.25f;
            double loadedTo = std::min(m->player->bufferedEnd() / duration, 1.0);
            if (loadedTo > 0) renderer.drawRoundedRect(c.trackX, mid - 2, (float)(c.trackW * loadedTo), 4, radii, buffered);
            drawSlider(renderer, c.trackX, mid, c.trackW, now / duration, ink, 12);
        }
        else {
            Color faint = ink;
            faint.a = 0.3f;
            const float radii[4] = { 2, 2, 2, 2 };
            renderer.drawRoundedRect(c.trackX, mid - 2, c.trackW, 4, radii, faint);
        }
    }

    // Mute: a speaker, struck through while muted; then the volume.
    bool muted = m ? m->player->muted() : hasAttr(b.el, L"muted");
    renderer.drawText(c.muteX + 5, mid - 10, muted ? L"\U0001F507" : L"\U0001F50A", 14, ink, false, false, &kSymbolFont);
    if (c.volumeW > 0) drawSlider(renderer, c.volumeX, mid, c.volumeW, muted ? 0 : m ? m->player->volume() : 1, ink, 10);

    if (c.fullscreenX >= 0) drawCorners(renderer, c.fullscreenX + 7, mid - 6, 14, 12, fullscreen, ink);
}

// --- Mouse -----------------------------------------------------------------

bool Engine::mediaBoxAt(int docX, int docY, LayoutBox& out) const {
    if (fullscreenVideoBox(out)) return true;
    if (domState.domDirty) return false; // boxes may be stale
    for (auto it = layoutRoot.boxes.rbegin(); it != layoutRoot.boxes.rend(); ++it) {
        if (it->media && hasAttr(it->el, L"controls") && boxContains(*it, docX, docY)) {
            out = *it;
            return true;
        }
    }
    return false;
}

bool Engine::overMediaControl(int x, int y) const {
    int docX, docY;
    LayoutBox box;
    if (!toPage(x, y, docX, docY) || !mediaBoxAt(docX, docY, box)) return false;
    MediaPart part = partAt(controlsLayout(box, (float)(box.y + boxShift(box))), (float)docX, (float)docY);
    return part != MediaPart::Video && part != MediaPart::Time;
}

bool Engine::mediaClick(int x, int y, double now) {
    int docX, docY;
    LayoutBox box;
    if (!toPage(x, y, docX, docY) || !mediaBoxAt(docX, docY, box)) return false;
    Element* el = box.el;
    const bool inFullscreen = fullscreenVideo() != nullptr;
    MediaElement* m = mediaFor(el, true);
    if (!m) return true;
    std::shared_ptr<Node> keep = m->node; // through the listeners below

    // The page's click listeners first; preventDefault() cancels the
    // control's own action.
    if (JSContext* ctx = jsContext()) {
        if (fireMouseEvent(ctx, el, L"click", mouseInfoAt(x, y))) return true;
        if (!inFullscreen && !stillInPage(el)) return true;
    }
    MediaPlayer& p = *m->player;

    const ControlsLayout c = controlsLayout(box, (float)(box.y + boxShift(box)));
    const float fx = (float)docX, fy = (float)docY;
    const MediaPart part = partAt(c, fx, fy);
    switch (part) {
    case MediaPart::Fullscreen:
        setFullscreen(inFullscreen ? nullptr : el);
        break;
    case MediaPart::Mute:
        p.setMuted(!p.muted());
        break;
    case MediaPart::Volume:
    case MediaPart::Track: {
        // Grabs the knob: it follows the mouse until the button is released.
        bool volume = part == MediaPart::Volume;
        mediaDrag_.node = m->node;
        mediaDrag_.volume = volume;
        mediaDrag_.x0 = volume ? c.volumeX : c.trackX;
        mediaDrag_.width = volume ? c.volumeW : c.trackW;
        mediaDrag_.fraction = -1;
        dragMedia(x);
        break;
    }
    case MediaPart::Play:
    case MediaPart::Video: {
        if (p.paused() || p.ended()) {
            startMediaLoad(*m);
            p.play();
        }
        else {
            p.pause();
        }
        // A double-click on the picture toggles fullscreen (the two clicks
        // have toggled playing twice, leaving it as it was).
        if (part == MediaPart::Video) {
            bool twice = el == lastVideoClickEl_ && now - lastVideoClick_ < 0.4;
            lastVideoClick_ = twice ? -1 : now;
            lastVideoClickEl_ = el;
            if (twice) setFullscreen(inFullscreen ? nullptr : el);
        }
        break;
    }
    case MediaPart::Time:
        break;
    }
    return true;
}

void Engine::dragMedia(int x) {
    if (!mediaDrag_.node) return;
    MediaElement* m = mediaFor(static_cast<Element*>(mediaDrag_.node.get()), false);
    if (!m || mediaDrag_.width <= 0) { endMediaDrag(); return; }
    MediaPlayer& p = *m->player;
    double f = std::clamp((x / zoom_ - mediaDrag_.x0) / mediaDrag_.width, 0.0f, 1.0f);
    if (std::fabs(f - mediaDrag_.fraction) < 0.001) return; // the mouse hasn't moved
    mediaDrag_.fraction = f;
    if (mediaDrag_.volume) {
        p.setVolume(f);
        if (f > 0 && p.muted()) p.setMuted(false); // turning it up unmutes, as in browsers
        return;
    }
    double duration = p.duration();
    if (duration > 0 && std::isfinite(duration)) p.setCurrentTime(f * duration);
}

// --- Saving media ---------------------------------------------------------
// Ctrl+S (main.cpp): what the page's media elements played goes to the
// Downloads folder. A plain source is the file itself, fetched again
// through our own stack (cookies and referrer as before). A MediaSource's
// stream is what its SourceBuffers were given (mse::recordings) - video
// and audio in separate files, as the page fetched them. Copies run on
// background threads; each tells the window when it's done (takeNotices).

namespace {

std::wstring downloadsFolder() {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &path)) && path) out = path;
    CoTaskMemFree(path);
    if (out.empty()) {
        wchar_t home[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
        out = (n && n < MAX_PATH ? std::wstring(home, n) : std::wstring(L".")) + L"\\Downloads";
    }
    return out;
}

// A file name from the page title: no characters Windows forbids, not too long.
std::wstring safeFileName(std::wstring name) {
    for (auto& c : name) if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    size_t a = name.find_first_not_of(L" ."), b = name.find_last_not_of(L" .");
    name = a == std::wstring::npos ? L"" : name.substr(a, b - a + 1);
    if (name.size() > 120) name = name.substr(0, 120);
    return name.empty() ? L"media" : name;
}

// `folder\base.ext`, or `base (2).ext` and so on if that's taken - created
// empty there and then, so saves running at once can't pick the same name.
// The caller writes over it (or deletes it if that fails).
std::wstring freePath(const std::wstring& folder, const std::wstring& base, const std::wstring& ext) {
    std::wstring path = folder + L"\\" + base + L"." + ext;
    for (int n = 2; n < 1000; n++) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            break;
        }
        if (GetLastError() != ERROR_FILE_EXISTS) break; // can't be written: the save itself will say so
        path = folder + L"\\" + base + L" (" + std::to_wstring(n) + L")." + ext;
    }
    return path;
}

std::wstring fileNameOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// A MediaSource buffer's file extension, from its MIME type.
std::wstring extensionForType(const std::wstring& mime) {
    if (mime.rfind(L"video/webm", 0) == 0 || mime.rfind(L"audio/webm", 0) == 0) return L"webm";
    if (mime.rfind(L"audio/mp4", 0) == 0) return L"m4a";
    if (mime.rfind(L"audio/mpeg", 0) == 0) return L"mp3";
    return L"mp4";
}

// A plain source's extension, from its URL (or what its element plays).
std::wstring extensionForUrl(const std::wstring& url, bool audio) {
    std::wstring path = url.substr(0, url.find_first_of(L"?#"));
    size_t dot = path.rfind(L'.'), slash = path.find_last_of(L"/\\");
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) && path.size() - dot <= 5) {
        std::wstring ext = path.substr(dot + 1);
        for (auto& c : ext) c = (wchar_t)towlower(c);
        if (!ext.empty() && std::all_of(ext.begin(), ext.end(), [](wchar_t c) { return iswalnum(c); })) return ext;
    }
    return audio ? L"mp3" : L"mp4";
}

// "3:07", or "1:02:09".
std::wstring clock(double seconds) {
    long s = std::lround(std::max(0.0, seconds));
    wchar_t text[32];
    if (s >= 3600) swprintf(text, 32, L"%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
    else swprintf(text, 32, L"%ld:%02ld", s / 60, s % 60);
    return text;
}

// A stream's recordings as files (on a background thread). Each
// SourceBuffer's best part - the quality with the most media - goes into
// one file with the others' (video and audio together), the segments in
// time order (mux::merge): MP4 if they're all MP4, else WebM or Matroska.
// The other qualities' parts are saved on their own, and anything merge
// can't read is copied as it was recorded.
void saveStream(const std::vector<mse::Recording>& recordings, double total, const std::wstring& folder,
                const std::wstring& base, const std::function<void(std::wstring)>& notify) {
    struct Part {
        std::wstring file, mime;
        mux::Info info;
    };
    std::vector<Part> best, others;
    for (const auto& rec : recordings) {
        std::vector<Part> parts;
        for (const auto& f : rec.files) parts.push_back({ f, rec.mimeType, mux::inspect(f) });
        auto top = std::max_element(parts.begin(), parts.end(), [](const Part& a, const Part& b) {
            return a.info.ok != b.info.ok ? !a.info.ok : a.info.seconds < b.info.seconds;
        });
        best.push_back(*top);
        for (auto it = parts.begin(); it != parts.end(); ++it)
            if (it != top) others.push_back(*it);
    }
    auto isVideo = [](const Part& p) { return p.info.ok ? p.info.video : p.mime.rfind(L"audio", 0) != 0; };
    // One part on its own: tidied (a merge of one), else as recorded.
    auto saveOne = [&](const Part& p, const std::wstring& name) {
        std::wstring tidied = mux::mergedExtension({ p.info });
        std::wstring to = freePath(folder, name, tidied.empty() ? extensionForType(p.mime) : tidied);
        if ((!tidied.empty() && mux::merge({ p.file }, to)) || CopyFileW(p.file.c_str(), to.c_str(), FALSE)) return fileNameOf(to);
        DeleteFileW(to.c_str());
        return std::wstring();
    };

    std::vector<std::wstring> saved;
    double seconds = 0; // what the merged file holds: as much as its shortest track
    std::vector<mux::Info> infos;
    for (const auto& p : best) infos.push_back(p.info);
    const std::wstring kind = mux::mergedExtension(infos);
    bool together = !kind.empty();
    if (together) {
        std::vector<std::wstring> files;
        seconds = best[0].info.seconds;
        for (const auto& p : best) {
            files.push_back(p.file);
            seconds = std::min(seconds, p.info.seconds);
        }
        std::wstring to = freePath(folder, base, kind);
        if (mux::merge(files, to)) {
            saved.push_back(fileNameOf(to));
        } else {
            DeleteFileW(to.c_str());
            together = false;
        }
    }
    if (!together) { // each track in its own file
        seconds = 0;
        for (const auto& p : best) {
            std::wstring name = saveOne(p, best.size() > 1 ? base + (isVideo(p) ? L" - video" : L" - audio") : base);
            if (!name.empty()) saved.push_back(name);
        }
    }
    int extra = 0, videos = 1, audios = 1;
    for (const auto& p : others) {
        const bool video = isVideo(p);
        std::wstring name = base + (video ? L" - video " + std::to_wstring(++videos) : L" - audio " + std::to_wstring(++audios));
        if (!saveOne(p, name).empty()) extra++;
    }

    if (saved.empty()) { notify(L"Couldn't save " + base); return; }
    std::wstring text = L"Saved " + saved[0] + (saved.size() > 1 ? L" and " + std::to_wstring(saved.size() - 1) + L" more" : L"") + L" to Downloads";
    if (seconds > 0 && std::isfinite(total) && total > 0 && seconds < total - 1)
        text += L" - " + clock(seconds) + L" of " + clock(total) + L"; the rest wasn't loaded";
    if (extra) text += L" (+" + std::to_wstring(extra) + L" in another quality)";
    notify(text);
}

} // namespace

// Whether an element's media is worth saving: it has a source, a stream
// has had something appended, and it isn't under a second long (an
// interface sound, or a probe of what plays - not what the person means).
bool Engine::worthSaving(const MediaElement& m) {
    if (m.src.empty()) return false;
    if (int mseId = mse::idForUrl(m.src)) return mse::recorded(mseId);
    const double length = m.player ? m.player->duration() : NAN;
    return !(std::isfinite(length) && length < 1);
}

bool Engine::hasMediaToSave() const {
    for (const auto& [el, m] : media_)
        if (worthSaving(m)) return true;
    return false;
}

void Engine::saveMedia() {
    auto notices = saveNotices_;
    auto notify = [notices](std::wstring text) {
        std::lock_guard<std::mutex> lock(notices->mutex);
        notices->list.push_back(std::move(text));
    };
    const std::wstring folder = downloadsFolder();
    const std::wstring base = safeFileName(title());
    int started = 0;
    std::set<std::wstring> seen; // a source two elements play is saved once
    for (auto& [el, m] : media_) {
        if (!worthSaving(m) || !seen.insert(m.src).second) continue;
        const bool audio = static_cast<const Element*>(el)->tag == L"audio";
        if (int mseId = mse::idForUrl(m.src)) {
            auto recordings = mse::recordings(mseId);
            const double total = mse::duration(mseId);
            started++;
            notify(L"Saving " + base + L"...");
            std::thread([recordings, total, folder, base, notify] { saveStream(recordings, total, folder, base, notify); }).detach();
            continue;
        }
        std::wstring to = freePath(folder, base + (audio ? L" - audio" : L""), extensionForUrl(m.src, audio));
        started++;
        if (m.src.rfind(L"http://", 0) == 0 || m.src.rfind(L"https://", 0) == 0) {
            FetchOptions options;
            options.dest = FetchDest::Empty;
            options.referrer = pageBaseUrl;
            notify(L"Downloading " + fileNameOf(to) + L"...");
            fetchBytesAsync(m.src, std::move(options), [to, notify](bool ok, std::vector<unsigned char> bytes) {
                FILE* f = ok ? _wfsopen(to.c_str(), L"wb", _SH_DENYNO) : nullptr;
                if (f) {
                    ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
                    fclose(f);
                } else ok = false;
                if (!ok) DeleteFileW(to.c_str());
                notify(ok ? L"Saved " + fileNameOf(to) + L" to Downloads" : L"Couldn't download " + fileNameOf(to));
            });
        } else { // a local file
            std::wstring from = m.src;
            std::thread([from, to, notify] {
                bool ok = CopyFileW(from.c_str(), to.c_str(), FALSE) != FALSE;
                if (!ok) DeleteFileW(to.c_str());
                notify(ok ? L"Saved " + fileNameOf(to) + L" to Downloads" : L"Couldn't save " + fileNameOf(to));
            }).detach();
        }
    }
    if (!started) notify(L"Nothing to save: this page has no video or audio that has loaded");
}

std::vector<std::wstring> Engine::takeNotices() {
    std::lock_guard<std::mutex> lock(saveNotices_->mutex);
    std::vector<std::wstring> out;
    out.swap(saveNotices_->list);
    return out;
}
