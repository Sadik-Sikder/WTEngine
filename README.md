# WTEngine

A web browser engine for Windows, written from scratch in C++20.

WTEngine fetches pages over HTTP(S), parses HTML and CSS, runs the page's JavaScript, lays the page out and paints it with OpenGL. It has its own HTML parser, CSS engine, layout engine, DOM bindings and renderer. The JavaScript interpreter (quickjs-ng) and the system libraries it builds on are the only borrowed parts.

It's a learning project, not a replacement for Chrome. Many real sites load and work, though: Wikipedia, BBC News and the DuckDuckGo homepage all render.

## What it can do

**Loading pages**
- HTTP/HTTPS through its own async network stack (Boost.Beast + OpenSSL), with cookies, redirects, gzip and the request headers browsers send.
- Pages, stylesheets, scripts and images all load in the background, so the window never freezes waiting on the network.
- Local files open too.

**HTML and CSS**
- An HTML parser that recovers from common sloppy markup.
- CSS selectors, including combinators, attribute selectors and most pseudo-classes, plus the cascade, inheritance, custom properties, `@media` queries and `:hover`.
- Block, inline, float, flexbox, grid and table layout, plus absolute, fixed and sticky positioning.
- Borders, rounded corners, shadows, linear and radial gradients, background images, `opacity` and `overflow` clipping.
- Lists, `white-space`, and any font installed on Windows through `font-family`. Web fonts (`@font-face`) aren't loaded yet.
- Images (PNG, JPEG, GIF, WebP and more through WIC) and SVG.

**JavaScript**
- quickjs-ng with a DOM API that frameworks like React run on:
  - elements, events, `classList`, `dataset`;
  - `innerHTML`, `querySelector`, `getBoundingClientRect` and `getComputedStyle`;
  - `MutationObserver`, `IntersectionObserver` and `ResizeObserver`.
- `fetch`, `XMLHttpRequest`, timers, `requestAnimationFrame`, `postMessage`, `localStorage`/`sessionStorage`.
- `URL`, `location`, `history`, `navigator` and `matchMedia`.
- `Intl` (number, date, plural, list and relative-time formatting, collation, segmentation), built on the ICU library that ships with Windows. The output matches Chrome's.
- A watchdog stops runaway scripts before the window hangs.

**Media**
- `<video>` and `<audio>`, played through Windows Media Foundation:
  - formats: MP4/H.264, WebM/VP9, MP3, WAV;
  - controls: play/pause, seeking, volume, fullscreen;
  - scripting: the `HTMLMediaElement` API and `new Audio()`.

**Browser UI**
- Address bar, back/forward, reload, links and forms (text fields, buttons, checkboxes, dropdowns).
- Find in page, page zoom, and a developer console with a JavaScript prompt.

## Building

Requirements:
- **Windows 10** (version 1903 or later) or **Windows 11**, **x64**.
- **Visual Studio 2022 or later** with the C++ desktop workload.
- **CMake 3.15+**, if you build with CMake.
- **[vcpkg](https://github.com/microsoft/vcpkg)**, for Boost.Beast, OpenSSL and zlib.

### 1. Install the dependencies (once)

```bat
git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
C:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics
C:\dev\vcpkg\vcpkg install boost-beast:x64-windows-static-md openssl:x64-windows-static-md zlib:x64-windows-static-md
setx VCPKG_ROOT C:\dev\vcpkg
```

Open a new terminal afterwards so `VCPKG_ROOT` is picked up. GLFW and quickjs-ng are already in the repository.

### 2. Build

Use either Visual Studio or CMake.

**Visual Studio:** open `WTEngine.sln`, choose the **x64** platform (Debug or Release), and build.

**CMake:** from the `WTEngine` folder:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

### 3. Run

```bat
build\Release\WTEngine.exe https://en.wikipedia.org/wiki/Web_browser
```

Pass a URL or a local file path. With no argument, a built-in start page opens.

## Controls

| Key | Action |
|---|---|
| Ctrl+L / F6 | Go to the address bar |
| Alt+Left / Alt+Right | Back / forward |
| F5 / Ctrl+R | Reload |
| Ctrl+F, F3 | Find in page, next match |
| Ctrl + / Ctrl − / Ctrl+0 | Zoom in / out / reset (Ctrl+wheel works too) |
| F12 | Developer console |
| Esc | Leave fullscreen video |

## Project layout

```
WTEngine/
  src/          the engine: parser, CSS, layout, renderer, JS bindings, networking, media, Intl
  include/      headers
  docs/         HOW_IT_WORKS.md - a detailed tour of the code
  resources/    the app icon
  libs/glfw/    prebuilt GLFW (x64)
  third_party/  quickjs-ng (JavaScript engine)
```

If you want to understand or change the code, start with **[docs/HOW_IT_WORKS.md](WTEngine/docs/HOW_IT_WORKS.md)**. It covers how a page goes from bytes to pixels, file by file, and lists every known limitation.

## Limitations

The larger gaps:

- **Streaming video:** sites that stream through Media Source Extensions (YouTube, Netflix) don't play, and DRM-protected video isn't supported.
- **ES modules:** `<script type="module">` is skipped.
- **Scrolling:** only the page itself scrolls, not elements inside it.
- **Paint and layout:** layout is a full pass on every change, with no incremental layout. Painting uses fixed-function OpenGL on the main thread.
- **Platform:** Windows only.

## Third-party code

- [quickjs-ng](https://github.com/quickjs-ng/quickjs): JavaScript engine (MIT, see `third_party/quickjs-ng/LICENSE`).
- [GLFW](https://www.glfw.org/): windowing and input (zlib/libpng license).
- [Boost.Beast](https://github.com/boostorg/beast), [OpenSSL](https://www.openssl.org/) and [zlib](https://zlib.net/), via vcpkg.
- Windows system libraries: Media Foundation (video and audio), ICU (`Intl`), WIC and Direct2D (images and SVG).
