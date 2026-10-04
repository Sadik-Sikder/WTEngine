// Svg.h - SVG images: sizing, rasterizing (Direct2D), and turning an inline
// <svg> element back into a document the rasterizer can read.
#pragma once
#include <map>
#include <string>
#include <vector>

struct Element;

// The size an SVG wants to be, in CSS pixels, from its root element's
// width/height/viewBox attributes: both width and height if given; one of
// them plus the viewBox's aspect ratio; else the viewBox's own size; else
// 300x150, the CSS default for a replaced element with no size.
void svgIntrinsicSize(const std::map<std::wstring, std::wstring>& rootAttrs, float& width, float& height);

// Whether downloaded image bytes are an SVG document rather than a bitmap.
bool looksLikeSvg(const std::vector<unsigned char>& bytes);

// Rasterizes an SVG document with Direct2D (Windows 10 1703+) into
// top-down RGBA pixels (straight alpha). It's drawn at a multiple of its
// intrinsic size so it stays sharp when shown bigger or zoomed; the
// intrinsic size, which layout should use, comes back separately. Call on
// a thread with COM initialized. False if it can't be parsed or drawn.
bool rasterizeSvg(const std::vector<unsigned char>& bytes, std::vector<unsigned char>& rgba,
                  int& pixelWidth, int& pixelHeight, int& naturalWidth, int& naturalHeight);

// An inline <svg> element (and its subtree) as a standalone SVG document in
// a data: URI, ready to load like any image URL. `currentColor` (a CSS
// color, empty = black) is what fill="currentColor" means inside it - the
// element's inherited text color, as in browsers. Restores the camelCase
// names the HTML parser lowercased (viewBox, linearGradient, ...).
std::wstring svgDataUri(const Element* svg, const std::wstring& currentColor);
