#pragma once

#include <string>
#include <vector>

struct Color {
    float r, g, b, a;
};

// A CSS linear or radial gradient, as layout parsed it. Stop positions are
// left as written (a fraction, or pixels) and resolved by the renderer
// against the gradient's actual length in the box it's drawn into.
struct GradientStop {
    Color color;
    bool hasPos = false; // no position: spread evenly between its neighbours
    bool px = false;     // `pos` is in pixels, else a fraction (50% = 0.5)
    float pos = 0;
};
struct Gradient {
    bool radial = false;
    bool repeating = false;
    // Linear: the direction as a CSS angle in degrees (0 = towards the top,
    // 90 = towards the right; the default, 180, is downwards) - or, for
    // "to top right" and the like, a corner: cornerX -1/+1 (left/right),
    // cornerY -1/+1 (top/bottom), whose angle depends on the box's shape.
    float angle = 180;
    bool toCorner = false;
    int cornerX = 0, cornerY = 0;
    // Radial: a circle or an ellipse (farthest-corner size, the default),
    // centred at (cx, cy) as fractions of the box.
    bool circle = false;
    float cx = 0.5f, cy = 0.5f;
    std::vector<GradientStop> stops;
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
    // `bold`/`italic` select the weight and style; `family` is an installed
    // font's face name (nullptr or empty = Segoe UI, the default). measureText
    // must be given the same font the text will be drawn with, since glyph
    // widths differ between them.
    virtual void drawText(float x, float y, const std::wstring& text,
        float fontSize, Color color, bool bold = false, bool italic = false,
        const std::wstring* family = nullptr) = 0;
    virtual float measureText(const std::wstring& text, float fontSize, bool bold = false,
        bool italic = false, const std::wstring* family = nullptr) = 0;

    // A filled rectangle with rounded corners, and a rounded border ring
    // `thickness` wide just inside the rectangle's edge. `radii` are the
    // corner radii (top-left, top-right, bottom-right, bottom-left), already
    // scaled to fit (LayoutBox::cornerRadii). Edges are anti-aliased.
    virtual void drawRoundedRect(float x, float y, float w, float h, const float radii[4], Color color) = 0;
    virtual void drawRoundedFrame(float x, float y, float w, float h, const float radii[4],
                                  float thickness, Color color) = 0;

    // Draws the image at `url` (already resolved to an absolute URL or local
    // path) into the given box, its corners rounded by `radii` if given.
    // With `tile` ({x, y, w, h}), the image is placed at that rectangle
    // instead of stretched over the box, and the box clips it - how a
    // background image covering a rounded box is drawn. Fetches and decodes
    // lazily on first use and caches the result; does nothing if fetching or
    // decoding fails.
    virtual void drawImage(float x, float y, float w, float h, const std::wstring& url,
                           const float* radii = nullptr, const float* tile = nullptr) = 0;

    // Fills the box with a gradient, its corners rounded by `radii` if given.
    virtual void drawGradient(float x, float y, float w, float h, const Gradient& g,
                              const float* radii = nullptr) = 0;

    // A box-shadow's shape: the (rounded) rectangle in `color`, its edge
    // fading out over `blur` pixels - half inside the rectangle, half outside.
    virtual void drawShadow(float x, float y, float w, float h, const float radii[4], float blur,
                            Color color) = 0;

    // Starts (if not already cached) fetching and decoding the image at
    // `url` on a background thread, and returns its natural pixel size via
    // (outWidth, outHeight) if that's already finished. Returns false right
    // away (never blocks) while still loading, or if it failed.
    virtual bool preloadImage(const std::wstring& url, int& outWidth, int& outHeight) = 0;

    // The page now being shown, sent as the referrer of the images it
    // loads (sites that block hotlinking check it). Set on each page load.
    virtual void setPageUrl(const std::wstring& /*url*/) {}

    // Bumped each time a background image load finishes and is uploaded.
    // The caller polls this to know when to re-layout so a box sized from
    // an image's natural size (preloadImage returned false at layout time)
    // can pick up the real size once it's known.
    virtual int imageGeneration() const = 0;

    // Restrict drawing to a rectangle (in the current coordinate space - see
    // setPageTransform - y down) until clearClip().
    virtual void setClip(float x, float y, float w, float h) = 0;
    virtual void clearClip() = 0;

    // Page zoom. Until resetTransform(), everything drawn is scaled by
    // `scale` and moved down by `offsetY` window pixels (the space above
    // the page): callers draw in page coordinates. Text is rasterized at
    // the scaled size rather than stretched, so zoomed text stays sharp.
    // beginFrame() also resets it.
    virtual void setPageTransform(float offsetY, float scale) = 0;
    virtual void resetTransform() = 0;
};
