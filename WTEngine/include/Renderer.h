#pragma once

#include <string>

struct Color {
    float r, g, b, a;
};

// Parses a CSS color: #rgb/#rgba/#rrggbb/#rrggbbaa, rgb()/rgba() (comma or
// space syntax, numbers or percentages), a named color, or "transparent".
// tryParseColor returns false for anything else (so a caller can tell
// "inherit"/garbage apart from a real color); parseColor instead falls back
// to light gray. Both defined in Engine.cpp.
bool tryParseColor(const std::wstring& str, Color& out);
Color parseColor(const std::wstring& str);

class Renderer {
public:
    virtual ~Renderer() = default;

    virtual void beginFrame(int width, int height, float scrollY) = 0;
    virtual void endFrame() = 0;

    virtual void drawRect(float x, float y, float w, float h, Color color) = 0;
    // `bold` selects the bold weight of the same face; measureText must be
    // given the same flag the text will be drawn with, since bold glyphs
    // are wider.
    virtual void drawText(float x, float y, const std::wstring& text,
        float fontSize, Color color, bool bold = false) = 0;
    virtual float measureText(const std::wstring& text, float fontSize, bool bold = false) = 0;

    // Draws the image at `url` (already resolved to an absolute URL or local
    // path) into the given box. Fetches and decodes lazily on first use and
    // caches the result; does nothing if fetching or decoding fails.
    virtual void drawImage(float x, float y, float w, float h, const std::wstring& url) = 0;

    // Starts (if not already cached) fetching and decoding the image at
    // `url` on a background thread, and returns its natural pixel size via
    // (outWidth, outHeight) if that's already finished. Returns false right
    // away (never blocks) while still loading, or if it failed.
    virtual bool preloadImage(const std::wstring& url, int& outWidth, int& outHeight) = 0;

    // Bumped each time a background image load finishes and is uploaded.
    // The caller polls this to know when to re-layout so a box sized from
    // an image's natural size (preloadImage returned false at layout time)
    // can pick up the real size once it's known.
    virtual int imageGeneration() const = 0;

    // Restrict drawing to a rectangle (window coordinates, y down) until clearClip().
    virtual void setClip(float x, float y, float w, float h) = 0;
    virtual void clearClip() = 0;
};
