#pragma once

#define NOMINMAX
#include "Renderer.h"
#include <windows.h>
#include <GL/gl.h>
#include <GLFW/glfw3.h>
#include <string>
#include <map>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>

// A text string rasterized once (via GDI) and uploaded as an alpha-only
// OpenGL texture, cached so repeated draws of the same string/size don't
// re-rasterize every frame.
struct TextTexture {
    GLuint id = 0;
    int width = 0;
    int height = 0;
};

enum class ImageLoadState { Loading, Ready, Failed };

// An <img>, uploaded as an OpenGL texture and cached by URL once its
// background fetch+decode finishes. `state` starts at Loading the instant
// the URL is first seen (so repeated calls don't launch duplicate fetches),
// moves to Ready once uploaded, or Failed if the fetch/decode didn't work
// (so a broken image isn't retried every frame).
struct ImageTexture {
    GLuint id = 0;
    int width = 0;
    int height = 0;
    ImageLoadState state = ImageLoadState::Loading;
};

// A background thread's finished decode, waiting for the main (GL) thread
// to upload it as a texture.
struct PendingImageUpload {
    std::wstring url;
    std::vector<unsigned char> rgba;
    int width = 0;
    int height = 0;
    bool ok = false;
};

class OpenGLRenderer : public Renderer {
public:
    OpenGLRenderer();
    ~OpenGLRenderer() override;

    void beginFrame(int width, int height, float scrollY) override;
    void endFrame() override;

    void drawRect(float x, float y, float w, float h, Color color) override;
    void drawText(float x, float y, const std::wstring& text,
        float fontSize, Color color) override;
    float measureText(const std::wstring& text, float fontSize) override;
    void drawImage(float x, float y, float w, float h, const std::wstring& url) override;
    bool preloadImage(const std::wstring& url, int& outWidth, int& outHeight) override;
    int imageGeneration() const override { return imageGen; }
    void setClip(float x, float y, float w, float h) override;
    void clearClip() override;

private:
    std::map<std::wstring, TextTexture> textCache;
    int frameHeight = 0; // framebuffer height, for flipping scissor coordinates
    HDC measureDC = nullptr;
    std::map<int, HFONT> measureFonts; // font size -> font, for measureText
    bool comInitialized = false; // whether we own COM's lifetime (needed for WIC image decoding)
    const TextTexture& getOrCreateTextTexture(const std::wstring& text, float fontSize);

    // --- Background image loading -------------------------------------
    // A fixed-size pool of worker threads (not one thread per image, which
    // would grow unbounded on an image-heavy page) pulls URLs off a shared
    // queue and does the fetch + decode (network/CPU work, no GL calls).
    // The main thread uploads finished pixels as a texture in beginFrame(),
    // since only the thread that owns the GL context may call GL functions.
    static constexpr int kImageLoaderThreads = 4;

    std::map<std::wstring, ImageTexture> imageCache;
    std::vector<std::thread> loaderThreads; // the fixed pool, started once in the constructor
    std::mutex queueMutex;
    std::condition_variable queueCv;
    std::deque<std::wstring> loadQueue;     // guarded by queueMutex
    bool shuttingDown = false;              // guarded by queueMutex
    std::mutex uploadMutex;
    std::vector<PendingImageUpload> pendingUploads; // guarded by uploadMutex
    int imageGen = 0; // bumped each time an upload completes; lets Engine know to re-layout

    const ImageTexture& getOrCreateImageTexture(const std::wstring& url);
    void startLoadingImage(const std::wstring& url);
    void imageLoaderThreadMain();
    void drainPendingImageUploads();
};
