// Svg.cpp - see Svg.h.
#define NOMINMAX
#include "Svg.h"
#include "DOM.h"
#include "CSS.h"
#include "Renderer.h"
#include <windows.h>
#include <d2d1_3.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <sstream>
#include <unordered_map>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

// --- Intrinsic size ---------------------------------------------------------

// An SVG length in CSS pixels; false for a percentage or anything else that
// needs a containing block to mean something.
static bool svgLength(const std::wstring& v, float& out) {
    size_t used = 0;
    float n;
    try { n = std::stof(v, &used); } catch (...) { return false; }
    std::wstring unit = v.substr(used);
    while (!unit.empty() && iswspace(unit.back())) unit.pop_back();
    if (unit.empty() || unit == L"px") out = n;
    else if (unit == L"pt") out = n * 4.0f / 3.0f;
    else if (unit == L"pc") out = n * 16.0f;
    else if (unit == L"in") out = n * 96.0f;
    else if (unit == L"cm") out = n * 96.0f / 2.54f;
    else if (unit == L"mm") out = n * 96.0f / 25.4f;
    else if (unit == L"em" || unit == L"rem") out = n * 16.0f;
    else return false;
    return out > 0;
}

void svgIntrinsicSize(const std::map<std::wstring, std::wstring>& attrs, float& width, float& height) {
    auto get = [&](const wchar_t* k) { auto it = attrs.find(k); return it == attrs.end() ? std::wstring() : it->second; };
    float w = 0, h = 0;
    bool hasW = svgLength(get(L"width"), w), hasH = svgLength(get(L"height"), h);

    // viewBox="min-x min-y width height", spaces and/or commas.
    float vbW = 0, vbH = 0;
    std::wstring vb = get(L"viewbox");
    if (vb.empty()) vb = get(L"viewBox");
    for (auto& c : vb) if (c == L',') c = L' ';
    std::wistringstream ss(vb);
    float vx, vy;
    bool hasVB = (ss >> vx >> vy >> vbW >> vbH) && vbW > 0 && vbH > 0;

    if (hasW && hasH) { width = w; height = h; }
    else if (hasW && hasVB) { width = w; height = w * vbH / vbW; }
    else if (hasH && hasVB) { height = h; width = h * vbW / vbH; }
    else if (hasVB) { width = vbW; height = vbH; }
    else if (hasW) { width = w; height = 150; }
    else if (hasH) { width = 300; height = h; }
    else { width = 300; height = 150; }
    width = std::clamp(width, 1.0f, 4096.0f);
    height = std::clamp(height, 1.0f, 4096.0f);
}

static std::wstring utf8ToWide(const char* s, size_t n) {
    if (n == 0) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, (int)n, out.data(), len);
    return out;
}

// The attributes of the document's root <svg ...> tag, names lowercased.
static std::map<std::wstring, std::wstring> rootSvgAttrs(const std::vector<unsigned char>& bytes) {
    std::map<std::wstring, std::wstring> attrs;
    std::string head(bytes.begin(), bytes.begin() + std::min<size_t>(bytes.size(), 16384));
    size_t at = 0;
    while ((at = head.find("<svg", at)) != std::string::npos) {
        char next = at + 4 < head.size() ? head[at + 4] : '>';
        if (isspace((unsigned char)next) || next == '>' || next == '/') break;
        at += 4;
    }
    if (at == std::string::npos) return attrs;
    size_t end = head.find('>', at);
    std::wstring tag = utf8ToWide(head.data() + at + 4, (end == std::string::npos ? head.size() : end) - at - 4);

    size_t i = 0;
    while (i < tag.size()) {
        while (i < tag.size() && (iswspace(tag[i]) || tag[i] == L'/')) i++;
        size_t nameStart = i;
        while (i < tag.size() && !iswspace(tag[i]) && tag[i] != L'=' && tag[i] != L'/') i++;
        std::wstring name = tag.substr(nameStart, i - nameStart);
        for (auto& c : name) c = (wchar_t)towlower(c);
        while (i < tag.size() && iswspace(tag[i])) i++;
        std::wstring value;
        if (i < tag.size() && tag[i] == L'=') {
            i++;
            while (i < tag.size() && iswspace(tag[i])) i++;
            if (i < tag.size() && (tag[i] == L'"' || tag[i] == L'\'')) {
                wchar_t q = tag[i++];
                size_t close = tag.find(q, i);
                if (close == std::wstring::npos) close = tag.size();
                value = tag.substr(i, close - i);
                i = close + 1;
            }
            else {
                size_t s = i;
                while (i < tag.size() && !iswspace(tag[i])) i++;
                value = tag.substr(s, i - s);
            }
        }
        if (!name.empty()) attrs[name] = value;
        else if (i < tag.size()) i++;
    }
    return attrs;
}

bool looksLikeSvg(const std::vector<unsigned char>& bytes) {
    size_t i = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) i = 3; // UTF-8 BOM
    while (i < bytes.size() && isspace(bytes[i])) i++;
    if (i >= bytes.size() || bytes[i] != '<') return false;
    std::string head(bytes.begin() + i, bytes.begin() + std::min<size_t>(bytes.size(), i + 4096));
    return head.find("<svg") != std::string::npos;
}

// --- Rasterizing ------------------------------------------------------------

bool rasterizeSvg(const std::vector<unsigned char>& bytes, std::vector<unsigned char>& rgba,
                  int& pixelWidth, int& pixelHeight, int& naturalWidth, int& naturalHeight) {
    float nw, nh;
    svgIntrinsicSize(rootSvgAttrs(bytes), nw, nh);

    // Drawn at 2x (4x for small icons) so it stays sharp when shown larger
    // than its intrinsic size or zoomed, capped at 4096 px a side.
    float scale = std::max(nw, nh) <= 64 ? 4.0f : 2.0f;
    scale = std::min(scale, 4096.0f / std::max(nw, nh));
    UINT pw = std::max(1u, (UINT)std::ceil(nw * scale));
    UINT ph = std::max(1u, (UINT)std::ceil(nh * scale));

    ComPtr<ID2D1Factory1> factory;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                 reinterpret_cast<void**>(factory.GetAddressOf()))))
        return false;
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return false;
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)))
        return false;

    // A software render target drawing straight into that WIC bitmap. Only
    // a device context (ID2D1DeviceContext5) can load and draw SVG.
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1RenderTarget> target;
    if (FAILED(factory->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target))) return false;
    ComPtr<ID2D1DeviceContext5> dc;
    if (FAILED(target.As(&dc))) return false; // Windows older than 10 1703: no SVG support

    ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), (DWORD)bytes.size())))
        return false;
    ComPtr<ID2D1SvgDocument> doc;
    if (FAILED(dc->CreateSvgDocument(stream.Get(), D2D1::SizeF(nw, nh), &doc))) return false;

    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale));
    dc->DrawSvgDocument(doc.Get());
    if (FAILED(dc->EndDraw())) return false;

    std::vector<unsigned char> bgra((size_t)pw * ph * 4);
    if (FAILED(bitmap->CopyPixels(nullptr, pw * 4, (UINT)bgra.size(), bgra.data()))) return false;

    // Premultiplied BGRA -> straight RGBA, which is what the GL blending expects.
    rgba.resize(bgra.size());
    for (size_t i = 0; i < bgra.size(); i += 4) {
        unsigned a = bgra[i + 3];
        auto un = [a](unsigned c) { return (unsigned char)(a ? std::min(255u, (c * 255 + a / 2) / a) : 0); };
        rgba[i + 0] = un(bgra[i + 2]);
        rgba[i + 1] = un(bgra[i + 1]);
        rgba[i + 2] = un(bgra[i + 0]);
        rgba[i + 3] = (unsigned char)a;
    }
    // Fully transparent pixels have no colour of their own (black, above).
    // Filtering - mipmaps when drawn smaller, bilinear when larger - blends
    // that black into the shape's edge as a dark fringe. So give each one
    // the average colour of its non-transparent neighbours, spreading
    // outwards a few pixels: invisible itself (alpha stays 0), but what
    // the edge blends with.
    std::vector<unsigned char> filled(pw * ph);
    for (size_t p = 0; p < filled.size(); p++) filled[p] = rgba[p * 4 + 3] != 0;
    for (int pass = 0; pass < 4; pass++) {
        std::vector<size_t> newly;
        for (UINT y = 0; y < ph; y++) {
            for (UINT x = 0; x < pw; x++) {
                size_t p = (size_t)y * pw + x;
                if (filled[p]) continue;
                unsigned sum[3] = {}, n = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        int nx = (int)x + dx, ny = (int)y + dy;
                        if (nx < 0 || ny < 0 || nx >= (int)pw || ny >= (int)ph) continue;
                        size_t q = (size_t)ny * pw + nx;
                        if (!filled[q]) continue;
                        for (int k = 0; k < 3; k++) sum[k] += rgba[q * 4 + k];
                        n++;
                    }
                }
                if (!n) continue;
                for (int k = 0; k < 3; k++) rgba[p * 4 + k] = (unsigned char)(sum[k] / n);
                newly.push_back(p);
            }
        }
        if (newly.empty()) break;
        for (size_t p : newly) filled[p] = 1;
    }

    pixelWidth = (int)pw;
    pixelHeight = (int)ph;
    naturalWidth = (int)std::lround(nw);
    naturalHeight = (int)std::lround(nh);
    return true;
}

// --- Inline <svg> -> document -----------------------------------------------

// SVG names are case-sensitive, but the HTML parser lowercases every tag
// and attribute name. These are the mixed-case ones SVG defines.
static const std::unordered_map<std::wstring, std::wstring>& mixedCaseNames() {
    static const std::unordered_map<std::wstring, std::wstring> names = [] {
        std::unordered_map<std::wstring, std::wstring> m;
        for (const wchar_t* n : {
                 // elements
                 L"altGlyph", L"altGlyphDef", L"altGlyphItem", L"animateColor", L"animateMotion",
                 L"animateTransform", L"clipPath", L"feBlend", L"feColorMatrix", L"feComponentTransfer",
                 L"feComposite", L"feConvolveMatrix", L"feDiffuseLighting", L"feDisplacementMap",
                 L"feDistantLight", L"feDropShadow", L"feFlood", L"feFuncA", L"feFuncB", L"feFuncG",
                 L"feFuncR", L"feGaussianBlur", L"feImage", L"feMerge", L"feMergeNode", L"feMorphology",
                 L"feOffset", L"fePointLight", L"feSpecularLighting", L"feSpotLight", L"feTile",
                 L"feTurbulence", L"foreignObject", L"glyphRef", L"linearGradient", L"radialGradient",
                 L"textPath",
                 // attributes
                 L"attributeName", L"attributeType", L"baseFrequency", L"baseProfile", L"calcMode",
                 L"clipPathUnits", L"diffuseConstant", L"edgeMode", L"filterUnits", L"gradientTransform",
                 L"gradientUnits", L"kernelMatrix", L"kernelUnitLength", L"keyPoints", L"keySplines",
                 L"keyTimes", L"lengthAdjust", L"limitingConeAngle", L"markerHeight", L"markerUnits",
                 L"markerWidth", L"maskContentUnits", L"maskUnits", L"numOctaves", L"pathLength",
                 L"patternContentUnits", L"patternTransform", L"patternUnits", L"pointsAtX", L"pointsAtY",
                 L"pointsAtZ", L"preserveAlpha", L"preserveAspectRatio", L"primitiveUnits", L"refX", L"refY",
                 L"repeatCount", L"repeatDur", L"requiredExtensions", L"requiredFeatures",
                 L"specularConstant", L"specularExponent", L"spreadMethod", L"startOffset", L"stdDeviation",
                 L"stitchTiles", L"surfaceScale", L"systemLanguage", L"tableValues", L"targetX", L"targetY",
                 L"textLength", L"viewBox", L"viewTarget", L"xChannelSelector", L"yChannelSelector",
                 L"zoomAndPan" }) {
            std::wstring lower = n;
            for (auto& c : lower) c = (wchar_t)towlower(c);
            m[lower] = n;
        }
        return m;
    }();
    return names;
}

static std::wstring svgName(const std::wstring& name) {
    auto it = mixedCaseNames().find(name);
    return it == mixedCaseNames().end() ? name : it->second;
}

static void escapeXml(const std::wstring& s, std::wstring& out) {
    for (wchar_t c : s) {
        switch (c) {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        case L'"': out += L"&quot;"; break;
        default: out.push_back(c);
        }
    }
}

// Replaces every case-insensitive "currentcolor" in `v` with `color`.
static std::wstring withCurrentColor(const std::wstring& v, const std::wstring& color) {
    std::wstring lower = v;
    for (auto& c : lower) c = (wchar_t)towlower(c);
    std::wstring out;
    size_t i = 0, at;
    while ((at = lower.find(L"currentcolor", i)) != std::wstring::npos) {
        out.append(v, i, at - i);
        out += color;
        i = at + 12;
    }
    out.append(v, i, std::wstring::npos);
    return out;
}

static void serialize(const Element* el, bool root, const std::wstring& color, std::wstring& out) {
    std::wstring name = svgName(el->tag);
    out += L"<" + name;
    if (root) out += L" xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\"";

    std::map<std::wstring, std::wstring> attrs;
    for (const auto& [k, v] : el->attrs) {
        if (k == L"xmlns" || k.rfind(L"xmlns:", 0) == 0) continue; // declared above
        if (k == L"style") continue; // folded in below
        // A link (<use href>, a gradient's href): written as xlink:href,
        // which every SVG 1.1 renderer understands.
        std::wstring attrName = (k == L"href") ? L"xlink:href" : svgName(k);
        attrs[attrName] = withCurrentColor(v, color);
    }
    // style="fill: red" -> fill="red": inline style is CSS, which a plain
    // SVG renderer may not apply, while presentation attributes it always
    // does. (A style declaration outranks the attribute, as in CSS.)
    auto style = el->attrs.find(L"style");
    if (style != el->attrs.end())
        for (const auto& [k, v] : CSS::parseDeclarations(style->second)) attrs[svgName(k)] = withCurrentColor(v, color);

    // Colours as plain #rrggbb: the page's CSS can use any syntax (hsl(),
    // rgb() with spaces, ...) but an SVG renderer may only read the
    // classic ones. Transparency moves to the matching -opacity attribute.
    static const std::pair<const wchar_t*, const wchar_t*> kColorAttrs[] = {
        { L"fill", L"fill-opacity" }, { L"stroke", L"stroke-opacity" }, { L"stop-color", L"stop-opacity" },
        { L"flood-color", L"flood-opacity" }, { L"lighting-color", nullptr }, { L"color", nullptr },
    };
    for (const auto& [name, opacityName] : kColorAttrs) {
        auto it = attrs.find(name);
        Color c;
        if (it == attrs.end() || !tryParseColor(it->second, c)) continue; // none, url(#id), ...
        wchar_t hex[8];
        swprintf(hex, 8, L"#%02x%02x%02x", (int)std::lround(c.r * 255), (int)std::lround(c.g * 255), (int)std::lround(c.b * 255));
        it->second = hex;
        if (c.a < 1 && opacityName && !attrs.count(opacityName)) attrs[opacityName] = std::to_wstring(c.a);
    }

    for (const auto& [k, v] : attrs) {
        out += L" " + k + L"=\"";
        escapeXml(v, out);
        out += L"\"";
    }
    if (el->children.empty()) { out += L"/>"; return; }
    out += L">";
    for (const auto& child : el->children) {
        if (child->type == Node::TEXT) escapeXml(static_cast<const TextNode*>(child.get())->text, out);
        else serialize(static_cast<const Element*>(child.get()), false, color, out);
    }
    out += L"</" + name + L">";
}

static std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

static std::wstring base64(const std::string& in) {
    static const char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < in.size()) v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < in.size()) v |= (unsigned char)in[i + 2];
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out.push_back(i + 1 < in.size() ? kTable[(v >> 6) & 63] : L'=');
        out.push_back(i + 2 < in.size() ? kTable[v & 63] : L'=');
    }
    return out;
}

std::wstring svgDataUri(const Element* svg, const std::wstring& currentColor) {
    // currentColor as a plain #rrggbb, which any SVG renderer can read
    // (the CSS color may be hsl(), a name, ...).
    Color c{ 0, 0, 0, 1 };
    if (!currentColor.empty()) tryParseColor(currentColor, c);
    wchar_t hex[8];
    swprintf(hex, 8, L"#%02x%02x%02x", (int)std::lround(c.r * 255), (int)std::lround(c.g * 255), (int)std::lround(c.b * 255));

    std::wstring xml;
    serialize(svg, true, hex, xml);
    return L"data:image/svg+xml;base64," + base64(toUtf8(xml));
}
