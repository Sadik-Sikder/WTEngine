#include "OpenGLRenderer.h"
#include "Fetcher.h"
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

    for (auto& [size, font] : measureFonts) DeleteObject(font);
    if (measureDC) DeleteDC(measureDC);
    for (auto& [key, tex] : textCache) {
        glDeleteTextures(1, &tex.id);
    }
    for (auto& [key, tex] : imageCache) {
        if (tex.id) glDeleteTextures(1, &tex.id);
    }
    if (comInitialized) CoUninitialize();
}

// Rasterizes `text` into an off-screen GDI bitmap (white text on a black
// background), then converts that into an RGBA texture where each pixel's
// brightness becomes its alpha. Drawing that texture with glColor4f(color)
// then tints the (already anti-aliased) glyph shapes to any color we want,
// without baking a color into the cached texture itself.
const TextTexture& OpenGLRenderer::getOrCreateTextTexture(const std::wstring& text, float fontSize, bool bold) {
    std::wstring key = text + L"@" + std::to_wstring((int)fontSize) + (bold ? L"b" : L"");
    auto it = textCache.find(key);
    if (it != textCache.end()) return it->second;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    HFONT font = CreateFontW(
        -(int)fontSize, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT oldFont = (HFONT)SelectObject(memDC, font);

    SIZE sz{ 1, 1 };
    GetTextExtentPoint32W(memDC, text.c_str(), (int)text.size(), &sz);
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
    glGenTextures(1, &tex.id);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texW, texH, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    auto result = textCache.emplace(key, tex);
    return result.first->second;
}

void OpenGLRenderer::beginFrame(int width, int height, float scrollY) {
    drainPendingImageUploads();

    frameHeight = height;
    glViewport(0, 0, width, height);
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, width, height, 0, -1, 1); 
    glMatrixMode(GL_MODELVIEW);
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
    float fontSize, Color color, bool bold) {
    if (text.empty()) return;

    const TextTexture& tex = getOrCreateTextTexture(text, fontSize, bold);

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glColor4f(color.r, color.g, color.b, color.a);

    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(x, y);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(x + tex.width, y);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(x + tex.width, y + tex.height);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(x, y + tex.height);
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
        upload.ok = item.ok && decodeImage(item.bytes, upload.rgba, upload.width, upload.height);

        std::lock_guard<std::mutex> lock(uploadMutex);
        pendingUploads.push_back(std::move(upload));
    }

    if (comInit) CoUninitialize();
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
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, u.width, u.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, u.rgba.data());
            tex.width = u.width;
            tex.height = u.height;
            tex.state = ImageLoadState::Ready;
        }
        else {
            tex.state = ImageLoadState::Failed;
        }
        imageGen++;
    }
}

void OpenGLRenderer::drawImage(float x, float y, float w, float h, const std::wstring& url) {
    if (url.empty()) return;

    const ImageTexture& tex = getOrCreateImageTexture(url);
    if (tex.state != ImageLoadState::Ready) return; // still loading, or failed

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(x, y);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(x + w, y);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(x + w, y + h);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(x, y + h);
    glEnd();

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
float OpenGLRenderer::measureText(const std::wstring& text, float fontSize, bool bold) {
    if (text.empty()) return 0;

    if (!measureDC) measureDC = CreateCompatibleDC(nullptr);

    int size = (int)fontSize;
    auto it = measureFonts.find({ size, bold });
    if (it == measureFonts.end()) {
        HFONT font = CreateFontW(
            -size, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        it = measureFonts.emplace(std::make_pair(size, bold), font).first;
    }

    HFONT old = (HFONT)SelectObject(measureDC, it->second);
    SIZE sz{ 0, 0 };
    GetTextExtentPoint32W(measureDC, text.c_str(), (int)text.size(), &sz);
    SelectObject(measureDC, old);
    return static_cast<float>(sz.cx);
}

// glScissor's origin is the bottom-left corner, our coordinates are top-left.
void OpenGLRenderer::setClip(float x, float y, float w, float h) {
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
