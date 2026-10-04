#include "OpenGLRenderer.h"

#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F // OpenGL 1.2; Windows' gl.h only declares 1.1
#endif
#ifndef GL_GENERATE_MIPMAP
#define GL_GENERATE_MIPMAP 0x8191 // OpenGL 1.4
#endif
#include "Fetcher.h"
#include "Svg.h"
#include <vector>
#include <algorithm>
#include <cmath>
#include <wincodec.h>

#pragma comment(lib, "windowscodecs.lib")

// Releases any COM interface (they all derive from IUnknown) when it goes
// out of scope, the same RAII pattern InetHandle uses in Fetcher.cpp.
struct ComRelease {
    IUnknown* p;
    explicit ComRelease(IUnknown* ptr) : p(ptr) {}
    ~ComRelease() { if (p) p->Release(); }
    ComRelease(const ComRelease&) = delete;
    ComRelease& operator=(const ComRelease&) = delete;
};

// Decodes any WIC-supported image format (PNG, JPEG, GIF, BMP, ...) from
// raw file bytes into top-down 32bpp RGBA pixels.
static bool decodeImage(const std::vector<unsigned char>& bytes,
                        std::vector<unsigned char>& outRgba, int& outW, int& outH) {
    if (bytes.empty()) return false;

    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWICImagingFactory, (void**)&factory)) || !factory) {
        return false;
    }
    ComRelease releaseFactory(factory);

    IWICStream* stream = nullptr;
    if (FAILED(factory->CreateStream(&stream)) || !stream) return false;
    ComRelease releaseStream(stream);

    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), (DWORD)bytes.size())))
        return false;

    IWICBitmapDecoder* decoder = nullptr;
    if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) || !decoder)
        return false;
    ComRelease releaseDecoder(decoder);

    IWICBitmapFrameDecode* frame = nullptr;
    if (FAILED(decoder->GetFrame(0, &frame)) || !frame) return false;
    ComRelease releaseFrame(frame);

    IWICFormatConverter* converter = nullptr;
    if (FAILED(factory->CreateFormatConverter(&converter)) || !converter) return false;
    ComRelease releaseConverter(converter);

    if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;

    UINT w = 0, h = 0;
    if (FAILED(converter->GetSize(&w, &h)) || w == 0 || h == 0) return false;

    UINT stride = w * 4;
    outRgba.resize((size_t)stride * h);
    if (FAILED(converter->CopyPixels(nullptr, stride, (UINT)outRgba.size(), outRgba.data())))
        return false;

    outW = (int)w;
    outH = (int)h;
    return true;
}

OpenGLRenderer::OpenGLRenderer() {
    // Needed for WIC (image decoding); harmless if something else already
    // initialized COM on this thread with a compatible concurrency model.
    comInitialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

    for (int i = 0; i < kImageDecodeThreads; i++)
        decodeThreads.emplace_back([this] { decodeThreadMain(); });
}

OpenGLRenderer::~OpenGLRenderer() {
    // Wake the pool so idle decoders can see shuttingDown and exit; one
    // still decoding finishes that image first. Join before tearing
    // anything down - the threads capture `this` by pointer, so none may
    // still be running once member destruction starts below. Downloads
    // still in flight on the network thread only hold a weak_ptr to the
    // queue, so they can't reach this renderer once it's gone.
    {
        std::lock_guard<std::mutex> lock(decodeQueue->mutex);
        decodeQueue->shuttingDown = true;
    }
    decodeQueue->cv.notify_all();
    for (auto& t : decodeThreads) if (t.joinable()) t.join();

    for (auto& [key, font] : measureFonts) DeleteObject(font);
    if (measureDC) DeleteDC(measureDC);
    for (auto& [key, tex] : textCache) {
        glDeleteTextures(1, &tex.id);
    }
    for (auto& [key, tex] : imageCache) {
        if (tex.id) glDeleteTextures(1, &tex.id);
    }
    for (auto& [key, id] : gradientCache_) glDeleteTextures(1, &id);
    if (comInitialized) CoUninitialize();
}

// Rasterizes `text` into an off-screen GDI bitmap (white text on a black
// background), then converts that into an RGBA texture where each pixel's
// brightness becomes its alpha. Drawing that texture with glColor4f(color)
// then tints the (already anti-aliased) glyph shapes to any color we want,
// without baking a color into the cached texture itself.
// The GDI font for a size/weight/style/face (nullptr or empty face = Segoe UI).
static HFONT createFont(int size, bool bold, bool italic, const std::wstring* family) {
    const wchar_t* face = family && !family->empty() ? family->c_str() : L"Segoe UI";
    return CreateFontW(-size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, italic ? TRUE : FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

const TextTexture& OpenGLRenderer::getOrCreateTextTexture(const std::wstring& text, float fontSize, bool bold,
                                                          bool italic, const std::wstring* family) {
    std::wstring key = text + L"@" + std::to_wstring((int)fontSize) + (bold ? L"b" : L"") + (italic ? L"i" : L"");
    if (family && !family->empty()) key += L"|" + *family;
    auto it = textCache.find(key);
    if (it != textCache.end()) {
        it->second.lastUsedFrame = frameNumber;
        return it->second;
    }

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    HFONT font = createFont((int)fontSize, bold, italic, family);
    HFONT oldFont = (HFONT)SelectObject(memDC, font);

    SIZE sz{ 1, 1 };
    GetTextExtentPoint32W(memDC, text.c_str(), (int)text.size(), &sz);
    // Italic glyphs lean past their advance width, which is all the extent
    // measures - leave room on the right so the last one isn't clipped.
    if (italic) sz.cx += (LONG)std::ceil(fontSize / 4);
    int texW = std::max(1, (int)sz.cx);
    int texH = std::max(1, (int)sz.cy);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = texW;
    bmi.bmiHeader.biHeight = -texH; // negative = top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP oldBitmap = (HBITMAP)SelectObject(memDC, bitmap);

    RECT rc{ 0, 0, texW, texH };
    FillRect(memDC, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(255, 255, 255));
    TextOutW(memDC, 0, 0, text.c_str(), (int)text.size());
    GdiFlush();

    // bits is BGRX; since the glyphs were drawn white-on-black, R==G==B==
    // coverage already, so brightness doubles as the alpha we need.
    auto* src = static_cast<unsigned char*>(bits);
    std::vector<unsigned char> rgba(static_cast<size_t>(texW) * texH * 4);
    for (int i = 0; i < texW * texH; i++) {
        unsigned char coverage = src[i * 4 + 2]; // B (== G == R)
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = coverage;
    }

    SelectObject(memDC, oldBitmap);
    SelectObject(memDC, oldFont);
    DeleteObject(bitmap);
    DeleteObject(font);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    TextTexture tex;
    tex.width = texW;
    tex.height = texH;
    tex.lastUsedFrame = frameNumber;
    glGenTextures(1, &tex.id);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // Clamp, not the default repeat: when a texture is drawn scaled (page
    // zoom), linear filtering would otherwise blend its left edge into its
    // right one, leaving a faint line at the edge of words and images.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texW, texH, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    textCacheBytes += rgba.size();
    auto result = textCache.emplace(key, tex);
    return result.first->second;
}

// Once the text cache is over budget, deletes the least recently drawn
// textures until it's down to 3/4 of the budget - the slack means the sort
// runs once per burst of new text, not every frame. Only called at the
// start of a frame, so no reference getOrCreateTextTexture handed out is
// still in use, and anything drawn last frame (i.e. on screen now) is kept
// even if that alone exceeds the budget.
void OpenGLRenderer::evictStaleTextTextures() {
    if (textCacheBytes <= kTextCacheBudget) return;

    std::vector<std::map<std::wstring, TextTexture>::iterator> stale;
    for (auto it = textCache.begin(); it != textCache.end(); ++it) {
        if (it->second.lastUsedFrame < frameNumber) stale.push_back(it);
    }
    std::sort(stale.begin(), stale.end(), [](const auto& a, const auto& b) {
        return a->second.lastUsedFrame < b->second.lastUsedFrame;
    });

    const size_t target = kTextCacheBudget / 4 * 3;
    for (auto it : stale) {
        if (textCacheBytes <= target) break;
        textCacheBytes -= static_cast<size_t>(it->second.width) * it->second.height * 4;
        glDeleteTextures(1, &it->second.id);
        textCache.erase(it);
    }
}

void OpenGLRenderer::beginFrame(int width, int height, float scrollY) {
    drainPendingImageUploads();
    evictStaleTextTextures(); // before bumping frameNumber, so last frame's text counts as in use
    frameNumber++;

    frameHeight = height;
    glViewport(0, 0, width, height);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width, height, 0, -1, 1); 
    glMatrixMode(GL_MODELVIEW);
    transformOffsetY = 0; // a new frame starts untransformed
    transformScale = 1;
    glLoadIdentity();
}

void OpenGLRenderer::endFrame() {
    // Nothing needed here for now
}

void OpenGLRenderer::drawRect(float x, float y, float w, float h, Color color) {
    glColor4f(color.r, color.g, color.b, color.a);
    glBegin(GL_QUADS);
    glVertex2f(x, y);
    glVertex2f(x + w, y);
    glVertex2f(x + w, y + h);
    glVertex2f(x, y + h);
    glEnd();
}

void OpenGLRenderer::drawText(float x, float y, const std::wstring& text,
    float fontSize, Color color, bool bold, bool italic, const std::wstring* family) {
    if (text.empty()) return;

    // Under a zoom (setPageTransform), rasterize at the size the text will
    // actually appear on screen and draw the quad that much smaller in page
    // coordinates - the transform scales it back up 1:1 - so zoomed text
    // is sharp instead of a stretched texture.
    float pixelSize = std::round(fontSize * transformScale);
    const TextTexture& tex = getOrCreateTextTexture(text, std::max(pixelSize, 1.0f), bold, italic, family);
    float w = tex.width / transformScale;
    float h = tex.height / transformScale;
    if (transformScale != 1.0f) {
        // Snap the quad's corner to a whole screen pixel, so each texel
        // lands exactly on one pixel - a fractional position would make
        // linear filtering smear the glyphs.
        x = std::round(x * transformScale) / transformScale;
        y = (std::round(transformOffsetY + y * transformScale) - transformOffsetY) / transformScale;
    }

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glColor4f(color.r, color.g, color.b, color.a);

    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(x, y);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(x + w, y);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(x + w, y + h);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(x, y + h);
    glEnd();

    glDisable(GL_TEXTURE_2D);
}

// Returns the cache slot for `url`, reserving it (state = Loading) and
// kicking off a background fetch+decode the first time this URL is seen.
// Never blocks: a URL that's still loading (or failed) just has no texture
// yet, and the caller (drawImage/preloadImage) treats that as "nothing to
// show right now".
const ImageTexture& OpenGLRenderer::getOrCreateImageTexture(const std::wstring& url) {
    auto it = imageCache.find(url);
    if (it != imageCache.end()) return it->second;

    auto result = imageCache.emplace(url, ImageTexture{});
    startLoadingImage(url);
    return result.first->second;
}

// Starts downloading `url` on the network thread; when the bytes arrive,
// they're queued for a decode thread. Never spawns a thread, so an
// image-heavy page uses the one network thread plus kImageDecodeThreads.
void OpenGLRenderer::startLoadingImage(const std::wstring& url) {
    std::weak_ptr<DecodeQueue> weak = decodeQueue;
    FetchOptions options;
    options.priority = FetchPriority::Image; // behind the page's own scripts/stylesheets on a busy host
    options.dest = FetchDest::Image;
    options.referrer = pageUrl_;
    fetchBytesAsync(url, std::move(options), [weak, url](bool ok, std::vector<unsigned char> bytes) {
        std::shared_ptr<DecodeQueue> queue = weak.lock();
        if (!queue) return; // the renderer is gone
        {
            std::lock_guard<std::mutex> lock(queue->mutex);
            if (queue->shuttingDown) return;
            queue->items.push_back({ url, ok, std::move(bytes) });
        }
        queue->cv.notify_one();
    });
}

// One decode thread's whole lifetime: take downloaded bytes off the queue,
// decode them (no GL calls, so no need for the GL context), post the
// result, repeat until shutdown. A failed download still posts a result
// (ok = false), so the image leaves the Loading state. WIC (used by
// decodeImage) needs COM initialized per thread, hence the
// Co(Un)InitializeEx - once per decode thread, not once per image.
void OpenGLRenderer::decodeThreadMain() {
    bool comInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

    for (;;) {
        DecodeItem item;
        {
            std::unique_lock<std::mutex> lock(decodeQueue->mutex);
            decodeQueue->cv.wait(lock, [this] { return decodeQueue->shuttingDown || !decodeQueue->items.empty(); });
            if (decodeQueue->shuttingDown) break; // pending decodes are pointless once the renderer is closing
            item = std::move(decodeQueue->items.front());
            decodeQueue->items.pop_front();
        }

        PendingImageUpload upload;
        upload.url = item.url;
        if (item.ok && looksLikeSvg(item.bytes)) {
            upload.ok = rasterizeSvg(item.bytes, upload.rgba, upload.width, upload.height,
                                     upload.naturalWidth, upload.naturalHeight);
        }
        else {
            upload.ok = item.ok && decodeImage(item.bytes, upload.rgba, upload.width, upload.height);
            upload.naturalWidth = upload.width;
            upload.naturalHeight = upload.height;
        }

        std::lock_guard<std::mutex> lock(uploadMutex);
        pendingUploads.push_back(std::move(upload));
    }

    if (comInit) CoUninitialize();
}

// Whether GL_GENERATE_MIPMAP exists (core since OpenGL 1.4). Windows' gl.h
// only declares 1.1, so the version is checked at runtime, once.
bool OpenGLRenderer::supportsAutoMipmaps() {
    static const bool supported = [] {
        const char* v = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        int major = 0, minor = 0;
        return v && sscanf_s(v, "%d.%d", &major, &minor) == 2 && (major > 1 || minor >= 4);
    }();
    return supported;
}

// Uploads every fetch/decode that finished since the last frame as a GL
// texture - the only part of image loading that must run on this (the GL
// context's) thread. Called once per frame, before anything is drawn.
void OpenGLRenderer::drainPendingImageUploads() {
    std::vector<PendingImageUpload> uploads;
    {
        std::lock_guard<std::mutex> lock(uploadMutex);
        if (pendingUploads.empty()) return;
        uploads.swap(pendingUploads);
    }

    for (auto& u : uploads) {
        ImageTexture& tex = imageCache[u.url]; // slot already reserved by getOrCreateImageTexture
        if (u.ok) {
            glGenTextures(1, &tex.id);
            glBindTexture(GL_TEXTURE_2D, tex.id);
            // Mipmaps where the driver can build them (OpenGL 1.4+): an image
            // drawn smaller than its pixels - a photo in a small box, or an
            // SVG, which is rasterized at 2-4x - then averages every pixel
            // it covers instead of sampling a few, so edges don't turn jagged.
            if (supportsAutoMipmaps()) {
                glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            }
            else {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            }
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            // Clamp, not the default repeat: when a texture is drawn scaled (page
            // zoom), linear filtering would otherwise blend its left edge into its
            // right one, leaving a faint line at the edge of words and images.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, u.width, u.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, u.rgba.data());
            tex.width = u.naturalWidth;
            tex.height = u.naturalHeight;
            tex.state = ImageLoadState::Ready;
        }
        else {
            tex.state = ImageLoadState::Failed;
        }
        imageGen++;
    }
}

// --- Rounded shapes ---------------------------------------------------------
// Drawn as triangles around a contour that follows the rounded corners.
// OpenGL's polygons have hard, aliased edges, so each shape also gets a
// 1px "fringe" strip along its edge that fades from the shape's alpha to
// zero, which reads as a smooth edge. A box-shadow is the same shape with
// a fringe as wide as its blur.

namespace {
    struct Pt { float x, y; };

    // Points around the rounded rectangle (x, y, w, h), clockwise from the
    // top-left corner, shrunk inward by `inset` (negative grows it) - every
    // radius shrinks by the same amount, so insets of one shape line up
    // point for point. `segments` per corner is fixed by the caller so
    // every contour of one shape has the same number of points.
    std::vector<Pt> roundedContour(float x, float y, float w, float h, const float radii[4],
                                   float inset, int segments) {
        const float kPi = 3.14159265f;
        x += inset; y += inset; w -= 2 * inset; h -= 2 * inset;
        // Corner centres sit at (radius - inset) from the inset edge, i.e.
        // fixed relative to the original rectangle; a corner whose radius
        // the inset swallows becomes a sharp one.
        float r[4];
        for (int i = 0; i < 4; i++) r[i] = std::max(radii[i] - inset, 0.0f);
        const Pt centres[4] = {
            { x + r[0], y + r[0] }, { x + w - r[1], y + r[1] },
            { x + w - r[2], y + h - r[2] }, { x + r[3], y + h - r[3] },
        };
        std::vector<Pt> pts;
        pts.reserve(4 * (segments + 1));
        for (int c = 0; c < 4; c++) {
            float start = kPi + c * kPi / 2; // top-left starts pointing left (y grows downwards)
            for (int s = 0; s <= segments; s++) {
                float a = start + (kPi / 2) * s / segments;
                pts.push_back({ centres[c].x + r[c] * std::cos(a), centres[c].y + r[c] * std::sin(a) });
            }
        }
        return pts;
    }

    int segmentsFor(const float radii[4]) {
        float biggest = std::max({ radii[0], radii[1], radii[2], radii[3] });
        return std::clamp((int)(biggest / 2), 2, 16);
    }

    bool anyRadius(const float* radii) {
        return radii && (radii[0] > 0 || radii[1] > 0 || radii[2] > 0 || radii[3] > 0);
    }

    // With a texture bound, how screen points map to texture coordinates:
    // u = ux*x + uy*y + u0, v = vx*x + vy*y + v0 - any linear mapping, so
    // the same shapes can carry an image placed anywhere or a gradient
    // running in any direction.
    struct TexMap {
        bool on = false;
        float ux = 0, uy = 0, u0 = 0, vx = 0, vy = 0, v0 = 0;
    };
    const TexMap kNoTexture{};

    // The texture's whole image stretched over the rectangle (x, y, w, h).
    TexMap rectMap(float x, float y, float w, float h) {
        return { true, 1 / w, 0, -x / w, 0, 1 / h, -y / h };
    }

    void vertex(const Pt& p, const TexMap& tm) {
        if (tm.on) glTexCoord2f(tm.ux * p.x + tm.uy * p.y + tm.u0, tm.vx * p.x + tm.vy * p.y + tm.v0);
        glVertex2f(p.x, p.y);
    }

    // A strip between two contours of equal length, `a` at alpha `alphaA`
    // and `b` at `alphaB`.
    void strip(const std::vector<Pt>& a, const std::vector<Pt>& b, Color c, float alphaA, float alphaB,
               const TexMap& tm) {
        glBegin(GL_TRIANGLE_STRIP);
        for (size_t i = 0; i <= a.size(); i++) {
            size_t k = i % a.size();
            glColor4f(c.r, c.g, c.b, c.a * alphaA); vertex(a[k], tm);
            glColor4f(c.r, c.g, c.b, c.a * alphaB); vertex(b[k], tm);
        }
        glEnd();
    }

    // A filled rounded rectangle whose edge fades out over `fade` pixels,
    // centred on the edge (1px for an ordinary smooth edge).
    void fillRounded(float x, float y, float w, float h, const float radii[4], Color c, const TexMap& tm,
                     float fade = 1) {
        if (w <= 0 || h <= 0) return;
        // The solid core can't shrink past the middle; a fade wider than
        // the shape just leaves it fainter.
        float half = std::min(fade / 2, std::min(w, h) / 2 - 0.25f);
        half = std::max(half, 0.0f);
        int seg = segmentsFor(radii);
        std::vector<Pt> inner = roundedContour(x, y, w, h, radii, half, seg);
        std::vector<Pt> outer = roundedContour(x, y, w, h, radii, -fade / 2, seg);

        glColor4f(c.r, c.g, c.b, c.a);
        glBegin(GL_TRIANGLE_FAN); // the shape is convex, so a fan from its centre covers it
        vertex({ x + w / 2, y + h / 2 }, tm);
        for (size_t i = 0; i <= inner.size(); i++) vertex(inner[i % inner.size()], tm);
        glEnd();
        strip(inner, outer, c, 1, 0, tm);
    }

    // A box filled through `tm`: rounded with a smooth edge if it has
    // radii, else a plain quad with crisp edges like every other box.
    void fillBox(float x, float y, float w, float h, const float* radii, Color c, const TexMap& tm) {
        if (w <= 0 || h <= 0) return;
        if (anyRadius(radii)) { fillRounded(x, y, w, h, radii, c, tm); return; }
        glColor4f(c.r, c.g, c.b, c.a);
        glBegin(GL_QUADS);
        vertex({ x, y }, tm);
        vertex({ x + w, y }, tm);
        vertex({ x + w, y + h }, tm);
        vertex({ x, y + h }, tm);
        glEnd();
    }

    // --- Gradients ----------------------------------------------------------

    // A gradient's stops with their positions resolved, against a gradient
    // line `length` pixels long, to fractions of it - filling in missing
    // ones and keeping them in order, the way CSS does.
    std::vector<std::pair<float, Color>> resolveStops(const std::vector<GradientStop>& stops, float length) {
        std::vector<std::pair<float, Color>> out;
        if (stops.empty()) return out;
        std::vector<float> pos(stops.size(), -1);
        for (size_t i = 0; i < stops.size(); i++)
            if (stops[i].hasPos) pos[i] = stops[i].px ? stops[i].pos / std::max(length, 1.0f) : stops[i].pos;
        if (pos.front() < 0) pos.front() = 0;
        if (pos.back() < 0) pos.back() = 1;
        float highest = pos.front();
        for (auto& p : pos) if (p >= 0) { p = std::max(p, highest); highest = p; } // no going backwards
        for (size_t i = 1; i < pos.size(); i++) {
            if (pos[i] >= 0) continue;
            size_t j = i;
            while (pos[j] < 0) j++; // the next stop that has a position (the last always does)
            for (size_t k = i; k < j; k++) pos[k] = pos[i - 1] + (pos[j] - pos[i - 1]) * (k - i + 1) / (j - i + 1);
            i = j;
        }
        for (size_t i = 0; i < stops.size(); i++) out.push_back({ pos[i], stops[i].color });
        return out;
    }

    // The colour at `t` along resolved stops, blended with premultiplied
    // alpha - so fading to `transparent` doesn't pass through grey.
    Color colorAt(const std::vector<std::pair<float, Color>>& stops, float t) {
        if (t <= stops.front().first) return stops.front().second;
        if (t >= stops.back().first) return stops.back().second;
        size_t i = 1;
        while (i < stops.size() && stops[i].first < t) i++;
        const auto& [p0, c0] = stops[i - 1];
        const auto& [p1, c1] = stops[i];
        float f = p1 > p0 ? (t - p0) / (p1 - p0) : 1;
        float a = c0.a + (c1.a - c0.a) * f;
        if (a <= 0) return { 0, 0, 0, 0 };
        auto mix = [&](float x0, float x1) { return (x0 * c0.a + (x1 * c1.a - x0 * c0.a) * f) / a; };
        return { mix(c0.r, c1.r), mix(c0.g, c1.g), mix(c0.b, c1.b), a };
    }

    void putPixel(std::vector<unsigned char>& rgba, size_t i, Color c) {
        rgba[i * 4 + 0] = (unsigned char)std::lround(std::clamp(c.r, 0.0f, 1.0f) * 255);
        rgba[i * 4 + 1] = (unsigned char)std::lround(std::clamp(c.g, 0.0f, 1.0f) * 255);
        rgba[i * 4 + 2] = (unsigned char)std::lround(std::clamp(c.b, 0.0f, 1.0f) * 255);
        rgba[i * 4 + 3] = (unsigned char)std::lround(std::clamp(c.a, 0.0f, 1.0f) * 255);
    }

    std::string stopsKey(const std::vector<std::pair<float, Color>>& stops) {
        std::string key;
        char buf[96];
        for (const auto& [p, c] : stops) {
            snprintf(buf, sizeof(buf), "%.4f:%.3f,%.3f,%.3f,%.3f;", p, c.r, c.g, c.b, c.a);
            key += buf;
        }
        return key;
    }
}

GLuint OpenGLRenderer::gradientTexture(const std::string& key, int width, int height, bool repeat,
                                       const std::vector<unsigned char>& rgba) {
    auto it = gradientCache_.find(key);
    if (it != gradientCache_.end()) return it->second;
    if (gradientCache_.size() >= 48) {
        for (auto& [k, id] : gradientCache_) glDeleteTextures(1, &id);
        gradientCache_.clear();
    }
    GLuint id = 0;
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    // A repeating gradient's texture is one period, tiled along u.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    gradientCache_[key] = id;
    return id;
}

// A linear gradient is baked into a 256x1 texture of its colours along the
// gradient line, and the box's texture coordinates run along that line, so
// any angle and any box size share one texture. A radial one is baked into
// a 256x256 texture over the box itself (it depends on the centre and the
// box's shape, which are part of its key).
void OpenGLRenderer::drawGradient(float x, float y, float w, float h, const Gradient& g, const float* radii) {
    if (w <= 0 || h <= 0 || g.stops.empty()) return;
    const float kPi = 3.14159265f;
    TexMap tm;
    GLuint texture = 0;

    if (!g.radial) {
        // The direction, as a unit vector (y grows downwards). "to <corner>"
        // points at the corner along the perpendicular to the diagonal that
        // joins the other two, as CSS defines it.
        float dx, dy;
        if (g.toCorner) {
            float n = std::sqrt(w * w + h * h);
            dx = g.cornerX * h / n;
            dy = g.cornerY * w / n;
        }
        else {
            float a = g.angle * kPi / 180;
            dx = std::sin(a);
            dy = -std::cos(a);
        }
        // The gradient line runs through the centre, long enough that its
        // ends' perpendiculars just touch the corners.
        float length = std::fabs(w * dx) + std::fabs(h * dy);
        auto stops = resolveStops(g.stops, length);
        float first = stops.front().first, period = stops.back().first - first;
        bool repeat = g.repeating && period > 0.001f;

        std::vector<unsigned char> rgba(256 * 4);
        for (int i = 0; i < 256; i++) {
            float t = (i + 0.5f) / 256;
            putPixel(rgba, i, colorAt(stops, repeat ? first + t * period : t));
        }
        texture = gradientTexture("L" + std::string(repeat ? "r" : "") + stopsKey(stops), 256, 1, repeat, rgba);

        // t = projection onto the line, 0 at its start and 1 at its end.
        float cx = x + w / 2, cy = y + h / 2;
        float ux = dx / length, uy = dy / length, u0 = 0.5f - (cx * dx + cy * dy) / length;
        if (repeat) { // u in periods from the first stop
            ux /= period; uy /= period; u0 = (u0 - first) / period;
        }
        tm = { true, ux, uy, u0, 0, 0, 0.5f };
    }
    else {
        // Centre and ending shape in pixels: a circle reaching the farthest
        // corner, or an ellipse through it with the proportions of the
        // closest sides (CSS's default "farthest-corner" size).
        float px = g.cx * w, py = g.cy * h;
        float fx = std::max(px, w - px), fy = std::max(py, h - py);
        float rx, ry;
        if (g.circle) rx = ry = std::sqrt(fx * fx + fy * fy);
        else {
            float sx = std::min(px, w - px), sy = std::min(py, h - py);
            if (sx > 0 && sy > 0) {
                float k = sx / sy;
                ry = std::sqrt((fx / k) * (fx / k) + fy * fy);
                rx = k * ry;
            }
            else { rx = fx * std::sqrt(2.0f); ry = fy * std::sqrt(2.0f); }
        }
        rx = std::max(rx, 1.0f);
        ry = std::max(ry, 1.0f);
        auto stops = resolveStops(g.stops, rx);
        float first = stops.front().first, period = stops.back().first - first;
        bool repeat = g.repeating && period > 0.001f;

        char shape[96];
        snprintf(shape, sizeof(shape), "R%d|%.3f,%.3f|%.3f,%.3f|", repeat ? 1 : 0, px / w, py / h, rx / w, ry / h);
        std::string key = shape + stopsKey(stops);
        auto cached = gradientCache_.find(key);
        if (cached != gradientCache_.end()) texture = cached->second;
        else {
            const int n = 256;
            std::vector<unsigned char> rgba((size_t)n * n * 4);
            for (int j = 0; j < n; j++) {
                for (int i = 0; i < n; i++) {
                    float ex = ((i + 0.5f) / n * w - px) / rx, ey = ((j + 0.5f) / n * h - py) / ry;
                    float t = std::sqrt(ex * ex + ey * ey);
                    if (repeat) t = first + std::fmod(std::fmod(t - first, period) + period, period);
                    putPixel(rgba, (size_t)j * n + i, colorAt(stops, t));
                }
            }
            texture = gradientTexture(key, n, n, false, rgba);
        }
        tm = rectMap(x, y, w, h);
    }

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);
    fillBox(x, y, w, h, radii, { 1, 1, 1, 1 }, tm);
    glDisable(GL_TEXTURE_2D);
}

void OpenGLRenderer::drawShadow(float x, float y, float w, float h, const float radii[4], float blur, Color color) {
    if (w <= 0 || h <= 0 || color.a <= 0) return;
    fillRounded(x, y, w, h, radii, color, kNoTexture, std::max(blur, 1.0f));
}

void OpenGLRenderer::drawRoundedRect(float x, float y, float w, float h, const float radii[4], Color color) {
    fillRounded(x, y, w, h, radii, color, kNoTexture);
}

void OpenGLRenderer::drawRoundedFrame(float x, float y, float w, float h, const float radii[4],
                                      float thickness, Color c) {
    if (w <= 0 || h <= 0 || thickness <= 0) return;
    int seg = segmentsFor(radii);
    if (thickness < 1.5f) {
        // Too thin for a solid core: fade in to the middle of the ring and out again.
        auto outer = roundedContour(x, y, w, h, radii, -0.5f, seg);
        auto mid = roundedContour(x, y, w, h, radii, thickness / 2, seg);
        auto inner = roundedContour(x, y, w, h, radii, thickness + 0.5f, seg);
        float a = std::min(thickness, 1.0f);
        strip(outer, mid, c, 0, a, kNoTexture);
        strip(mid, inner, c, a, 0, kNoTexture);
        return;
    }
    auto outerFade = roundedContour(x, y, w, h, radii, -0.5f, seg);
    auto outer = roundedContour(x, y, w, h, radii, 0.5f, seg);
    auto inner = roundedContour(x, y, w, h, radii, thickness - 0.5f, seg);
    auto innerFade = roundedContour(x, y, w, h, radii, thickness + 0.5f, seg);
    strip(outerFade, outer, c, 0, 1, kNoTexture);
    strip(outer, inner, c, 1, 1, kNoTexture);
    strip(inner, innerFade, c, 1, 0, kNoTexture);
}

void OpenGLRenderer::drawImage(float x, float y, float w, float h, const std::wstring& url, const float* radii,
                               const float* tile) {
    if (url.empty()) return;

    const ImageTexture& tex = getOrCreateImageTexture(url);
    if (tex.state != ImageLoadState::Ready) return; // still loading, or failed

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    // The vertex colour multiplies the texture, so a rounded edge's fading
    // alpha softens the image's own edge too.
    TexMap tm = tile ? rectMap(tile[0], tile[1], tile[2], tile[3]) : rectMap(x, y, w, h);
    fillBox(x, y, w, h, radii, { 1, 1, 1, 1 }, tm);
    glDisable(GL_TEXTURE_2D);
}

bool OpenGLRenderer::preloadImage(const std::wstring& url, int& outWidth, int& outHeight) {
    if (url.empty()) return false;

    const ImageTexture& tex = getOrCreateImageTexture(url);
    if (tex.state != ImageLoadState::Ready) return false;

    outWidth = tex.width;
    outHeight = tex.height;
    return true;
}

// Measures with the same font the textures are rasterized with, but without
// creating a texture (layout measures many strings that are never drawn).
float OpenGLRenderer::measureText(const std::wstring& text, float fontSize, bool bold,
                                  bool italic, const std::wstring* family) {
    if (text.empty()) return 0;

    if (!measureDC) measureDC = CreateCompatibleDC(nullptr);

    int size = (int)fontSize;
    auto key = std::make_tuple(size, bold, italic, family ? *family : std::wstring());
    auto it = measureFonts.find(key);
    if (it == measureFonts.end()) it = measureFonts.emplace(key, createFont(size, bold, italic, family)).first;

    HFONT old = (HFONT)SelectObject(measureDC, it->second);
    SIZE sz{ 0, 0 };
    GetTextExtentPoint32W(measureDC, text.c_str(), (int)text.size(), &sz);
    SelectObject(measureDC, old);
    return static_cast<float>(sz.cx);
}

void OpenGLRenderer::setPageTransform(float offsetY, float scale) {
    transformOffsetY = offsetY;
    transformScale = scale;
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0, offsetY, 0);
    glScalef(scale, scale, 1);
}

void OpenGLRenderer::resetTransform() {
    transformOffsetY = 0;
    transformScale = 1;
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

// glScissor's origin is the bottom-left corner, our coordinates are top-left.
// glScissor also ignores the modelview matrix, so the page transform is
// applied here by hand.
void OpenGLRenderer::setClip(float x, float y, float w, float h) {
    x *= transformScale; w *= transformScale; h *= transformScale;
    y = transformOffsetY + y * transformScale;
    int left = (int)std::floor(x);
    int top = (int)std::floor(y);
    int right = (int)std::ceil(x + w);
    int bottom = (int)std::ceil(y + h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(left, frameHeight - bottom, std::max(right - left, 0), std::max(bottom - top, 0));
}

void OpenGLRenderer::clearClip() {
    glDisable(GL_SCISSOR_TEST);
}
