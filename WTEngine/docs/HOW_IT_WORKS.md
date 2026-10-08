# WTEngine — How It Works

A working reference for the current codebase. It describes what the code does today, not what is planned. File and function names are given so you can jump straight to the source.

_Snapshot: latest commit `112fd0f` ("Evaluate @media (min-width:Npx)/(max-width:Npx) against the live viewport"), plus this commit's documentation of it._

---

## 1. What it is

WTEngine is a small, from-scratch web browser engine for Windows. It:

1. Loads a page from an `http(s)://` URL or a local file (Boost.Beast/Asio + OpenSSL).
2. Parses the HTML into a DOM tree and the `<style>` blocks into CSS rules.
3. Runs the page's `<script>`s in an embedded JavaScript engine (quickjs-ng) with a minimal DOM API.
4. Lays the DOM out into a flat list of rectangles (`LayoutBox`).
5. Paints those rectangles with legacy OpenGL inside a GLFW window.
6. Handles links, form controls, scrolling, an address bar and back/forward history.

Everything is one process, one UI thread, plus a small pool of image-loader threads.

| Area | Technology |
|---|---|
| Language | C++20 |
| Windowing / input | GLFW 3 (`libs/glfw/glfw3.lib`, headers in `include/GLFW`) |
| Drawing | Fixed-function OpenGL (`glBegin/glEnd`, `glOrtho`) |
| Text rasterizing | GDI (Segoe UI) → alpha texture |
| Image decoding | WIC (Windows Imaging Component) |
| Networking | Boost.Beast/Asio (sync HTTP/HTTPS client) + OpenSSL (via vcpkg) |
| JavaScript | quickjs-ng, vendored in `third_party/quickjs-ng` |
| Platform | Windows, x64 only |

## 2. Building and running

There are two equivalent ways to build. Both produce the same program (x64 only, C++20).

**Prerequisite: vcpkg.** `Fetcher.cpp`'s HTTP(S) client needs `boost-beast`, `openssl`, and `zlib` (response decompression - §13), all built for the `x64-windows-static-md` triplet (static libs, dynamic CRT — matching how `qjs`/`glfw3` are already linked). These come from [vcpkg](https://github.com/microsoft/vcpkg), not `third_party/` (see §13). One-time setup:
```
git clone https://github.com/microsoft/vcpkg C:\dev\vcpkg
C:\dev\vcpkg\bootstrap-vcpkg.bat -disableMetrics
C:\dev\vcpkg\vcpkg install boost-beast:x64-windows-static-md openssl:x64-windows-static-md zlib:x64-windows-static-md
```
Then set `VCPKG_ROOT` (e.g. `C:\dev\vcpkg`) as an environment variable — both build paths below read it.

**A. Visual Studio (`WTEngine.sln` → `WTEngine.vcxproj`).** Open the solution, pick **x64** (Debug or Release), build. The vendored quickjs-ng is compiled directly into the project from four C files (`dtoa.c`, `libregexp.c`, `libunicode.c`, `quickjs.c`) as C11 with `/experimental:c11atomics`. Boost/OpenSSL/zlib include and library paths are wired via `$(VCPKG_ROOT)\installed\x64-windows-static-md\...` in the `Debug|x64`/`Release|x64` `ItemDefinitionGroup`s - note zlib's actual lib filenames are `zs.lib`/`zsd.lib` (Release/Debug), not `zlib.lib`, this triplet's own naming convention. **Any new `.cpp` file must be added to the project** (VS: right-click → Add → Existing Item; it lands in the `ClCompile` list in `WTEngine.vcxproj`).

**B. CMake.** `CMakeLists.txt` builds quickjs-ng as the static library `qjs` (its own `CMakeLists.txt`, unmodified), points `CMAKE_TOOLCHAIN_FILE` at vcpkg's toolchain (from `VCPKG_ROOT`, before the first `project()` call — required, since CMake only honors it that early), and links `glfw3`, `opengl32`, `wininet` (URL-parsing utilities only — see §13), `qjs`, `Boost::beast`, `OpenSSL::SSL`/`OpenSSL::Crypto`, `ZLIB::ZLIB`, and the Windows socket/crypto libs. **Any new `.cpp` file must be added to the `add_executable(WTEngine …)` list.**

```
cmake -S . -B build -A x64
cmake --build build --config Debug
build\Debug\WTEngine.exe [url-or-file]
```

> **Keep the two lists in sync.** The source list is written twice (`WTEngine.vcxproj` and `CMakeLists.txt`). Forgetting one is the usual cause of "unresolved external symbol" after adding a file. Win32 (32-bit) configurations exist in the `.vcxproj` but cannot link, because `libs/glfw/glfw3.lib` is 64-bit only.

- With no command-line argument the built-in demo page (`kDefaultPage` in `main.cpp`) is shown.
- With an argument, it is loaded through `navigate()`. That can be a URL or a local file path.
- `console.log(...)` and every uncaught JS error appear in the developer console (F12, §11), and are mirrored to the console window (stdout) and the debugger's Output window.

### Suggested reading order
If you're new to the code, read in this order; each step builds on the previous one:

1. `DOM.h` — the data model (30 lines).
2. `main.cpp` — the frame loop and input, which shows how everything is wired.
3. `Engine.cpp` (`loadHTML`, `doLayout`, `render`) — the coordinator.
4. `Layout.h` / `Layout.cpp` — where a DOM tree becomes boxes; most feature work lands here.
5. `HTMLParser.cpp` and `CSS.cpp` — input side.
6. `EngineForms.cpp` — interactive controls.
7. `JSBinding.cpp` — JavaScript ↔ DOM. Skim the first half; the memory-management comments are the important part.
8. `OpenGLRenderer.cpp` — only needed if you change drawing.

## 3. Source layout

```
src/                       include/
  main.cpp                   Engine.h        — Engine class, FormSubmission
  Engine.cpp                 DOM.h           — Node / TextNode / Element / Document
  EngineForms.cpp            HTMLParser.h
  HTMLParser.cpp             CSS.h           — selectors, rules, matching
  CSS.cpp                    Layout.h        — LayoutBox, LayoutRoot
  Layout.cpp                 Renderer.h      — abstract drawing interface, Color
  OpenGLRenderer.cpp         OpenGLRenderer.h
  Fetcher.cpp                Fetcher.h       — URLs, HTTP, local files
  JSEngine.cpp               JSEngine.h      — quickjs runtime wrapper
  JSBinding.cpp              JSBinding.h     — DOM/timers/events exposed to JS
  AddressBar.cpp             AddressBar.h
  TextEditor.cpp             TextEditor.h    — caret + selection for one line
  PageLoader.cpp             PageLoader.h    — background page fetch, §5
  DevConsole.cpp             DevConsole.h    — console log + F12 panel, §11
  FindBar.cpp                FindBar.h       — the Ctrl+F find bar, §10
  ResourceLoader.cpp         ResourceLoader.h — background script/stylesheet fetch, §5
  WebStorage.cpp             WebStorage.h    — localStorage/sessionStorage data, §11
  Svg.cpp                    Svg.h           — SVG sizing, Direct2D rasterizing, §14
  CookieJar.cpp              CookieJar.h     — HTTP cookies, §13
                             PageHistory.h   — back/forward stacks (header only)
third_party/quickjs-ng/    vendored JS engine, built unmodified
libs/glfw/                 prebuilt glfw3.lib
```

## 4. The big picture

```
                    ┌──────────────────────── main.cpp ────────────────────────┐
  GLFW callbacks ──►│ App { window, Engine*, OpenGLRenderer*, AddressBar,       │
  (mouse/key/char/  │       PageHistory, pendingUrl, pendingHistory }           │
   scroll)          └───────┬────────────────────────────────────┬──────────────┘
                            │ navigate(url)                      │ every frame
                            ▼                                    ▼
                  Fetcher (Boost.Beast/Asio)               Engine::render()
                            │ html                               │
                            ▼                                    ▼
                   Engine::loadHTML(html, baseUrl)         Renderer (OpenGL)
                            │                              drawRect / drawText /
        ┌───────────────────┼───────────────────┐          drawImage
        ▼                   ▼                   ▼
   HTMLParser          beginScripts()       doLayout()
   → Document          quickjs + DOM        LayoutRoot::layout()
   (DOM + CSS rules)   bindings             → vector<LayoutBox>
```

The key design choice: **layout produces a flat, absolute-positioned list of boxes**, and everything downstream (painting, link hit-testing, form-control hit-testing, click dispatch) just walks that list. There is no render tree, no per-element geometry stored on the DOM.

## 5. Program flow (`main.cpp`)

### Startup — `wmain`
1. `timeBeginPeriod(1)` so the frame-cap `Sleep` is accurate.
2. Create a 900×600 GLFW window, enable alpha blending.
3. Create `Engine`, `OpenGLRenderer`; call `engine.setRenderer()` (so layout can measure text) and `engine.setTopInset(AddressBar::kHeight)` (the page starts 40 px down).
4. Register the GLFW callbacks. They all reach the `App` struct through `glfwGetWindowUserPointer`.
5. Load the command-line URL, or the default page.

### The frame loop
Each iteration (capped at ~60 fps with `Sleep`):

1. **Apply pending actions** set by input callbacks: history back/forward (`pendingHistory`), navigation (`pendingUrl`), and a queued form submission (`engine.takeSubmission`). Callbacks never navigate directly; they set a flag and the loop does it. That keeps fetching out of the callback stack.
2. `applyFinishedNavigation` — shows a background page fetch's result the moment it's ready (below).
3. `drawFrame(app)`, steps 4–8:
4. Clear, set the viewport, `engine.onResize()`.
5. `renderer.beginFrame()` then `engine.render()` — draws the page.
6. `bar.draw()`, the console and the find bar — drawn **after** the page so they cover content scrolled under them.
7. Choose the cursor (I-beam over text fields/address bar, hand over links/buttons/nav buttons).
8. Swap buffers.
9. Poll events, sleep the remainder of the 16.6 ms budget.

**Live resizing.** While the user drags a window edge, Windows runs its own modal sizing loop *inside* `glfwPollEvents()`, so the main loop above doesn't run again until the mouse is released - the window used to show stale, stretched content for the whole drag. `drawFrame` is therefore also called from GLFW's window-refresh callback (`onWindowRefresh`), which Windows triggers on each size change during that loop; `App::drawing` guards against a refresh delivered from inside a frame. `Engine::onResize` keeps those frames cheap: a height-only change just re-clamps the scroll (layout depends only on width), and a width change relayouts immediately only if the last layout took ≤ 30 ms. Otherwise the relayout is deferred (`relayoutPending_`, checked at the top of `render()`):
- **always by one frame**, so the window first repaints at its new size with the old layout instead of sitting unpainted (or showing Windows' stretched snapshot) while the relayout runs - most visible on maximize, snap and restore;
- **while the user is dragging an edge, until the size holds still for 150 ms** (or the drag ends), so the drag stays smooth. GLFW doesn't report drags, so `main.cpp` wraps GLFW's window procedure (`sizingWindowProc`) to catch `WM_ENTERSIZEMOVE`/`WM_EXITSIZEMOVE` and tells the engine (`Engine::setLiveResize`). A maximize isn't a drag, so it doesn't wait.

Measured on Wikipedia's Tiger article, Release build, on battery: after a maximize, the window paints at full size ~110 ms in (most of that is Windows' maximize animation), and the reflowed layout is up ~530 ms in.

**Build type matters a lot here.** Measured on Wikipedia's Tiger article (on battery): a full layout is ~4.6 s in a Debug build but ~0.36 s in Release - the Debug STL's checked iterators dominate. Judge resize smoothness (or any layout-heavy behavior) with a Release build; in Debug, the one deferred relayout after a resize still freezes the window for seconds on a page that size.

### Navigation (`PageLoader`)
Page fetching is **backgrounded**: `navigate(url, postBody?)` calls `app.pageLoader.start(url, postBody, replace)`, which hands the request to Fetcher's network thread (`fetchPageAsync`, §13) and returns immediately - so the frame loop, and the window's message pump with it, keeps running while a fetch is in flight, however long it takes. (It originally ran on a detached thread per navigation; see §13's "The network thread" for why everything now shares one.) `applyFinishedNavigation` polls `pageLoader.poll(...)` once per frame; the moment a result is ready, it shows the page (`visitPage`) or a generated red error page, exactly as if the fetch had been synchronous.

- **Why:** a single blocking `fetchPage()` call used to freeze the *entire application* - unresponsive, un-closeable except by killing the process - for as long as the underlying fetch took. The original theory (still true, just not the dominant cause - see below) was WinINet's dual-stack connect behavior: on a host whose IPv6 route is black-holed, WinINet waits through the OS's full TCP connect timeout (~20-30s) before falling back to the working IPv4 address.
- **Superseding:** starting a new fetch (or `goHistory`'s Back/Forward, which calls `pageLoader.cancel()`) abandons whatever was previously in flight. `PageLoader::current_` is the only owner of the request's result slot - the network thread holds just a `weak_ptr` - so dropping it is the whole mechanism: a request still waiting for a connection slot is skipped before anything is sent (`FetchOptions::stillWanted`), and one already on the wire finishes but its result is dropped. See `PageLoader.h`'s comments for how this is made thread-safe (the tricky part: not destroying the result slot's mutex while a `lock_guard` still holds it - a real bug caught during development via a Debug-CRT "unlock of unowned mutex" assertion).
- **Timeout:** `poll()` also gives up on a fetch that's been running longer than 8 seconds, reporting it as failed ("Timed out") from the application's side - independent of whatever the OS is still doing with the underlying connection.
- **The networking layer was later replaced** (WinINet → Boost.Beast/Asio + OpenSSL, §13) specifically to fix the above: `beast::tcp_stream::connect()` applies a short *per-address* deadline (`expires_after`, a few seconds) instead of waiting out the OS's own connect timeout, so a black-holed address is abandoned quickly.
- **This did not fully fix the freeze.** Diagnostic timing (wall-clock around `resolve()`/`connect()`) showed the top-level fetch itself completing in ~1 second even against the site that used to hang for 20-30s - yet the application still went unresponsive for tens of seconds afterward. The actual dominant cause at that point was `Engine::runScripts()` and `loadLinkedStylesheets()` each calling `fetchPage()` synchronously on the main UI thread, once per `<script src>`/`<link rel=stylesheet>` tag - entirely bypassing `PageLoader`. That's now fixed too - see `ResourceLoader` below - but fixing it surfaced a third, larger bottleneck that's still open; see §13's `ResourceLoader` note and the Engine limitation at the end of this doc.
- `visitPage` saves the scroll offset of the page being left, pushes a new `HistoryEntry`, then `showEntry`.
- `showEntry` calls `engine.loadHTML(html, url)`, restores the scroll position, and updates the window title and address bar.
- **Window title** (`updateWindowTitle`, also called every frame right after `engine.render`): `"<title> - WTEngine"` from `Engine::title()` (the first `<title>` outside any `<svg>`, whitespace-collapsed - `documentTitle` in `JSBinding.cpp`), falling back to `"<url> - WTEngine"` for a page without one. Because it's re-read every frame, a script's `document.title = ...` shows up too; `App::shownTitle` keeps it from calling `glfwSetWindowTitle` unless the text changed.
- `goHistory(±1)` re-displays a stored entry **from its saved HTML** — no network request, and a POSTed result is not re-sent.
- **Reload** (`reload()` - the ↻ button, F5, Ctrl+R) is the opposite: it re-fetches the current URL (`NavigationKind::Reload`), so it picks up changes, including edits to a local file. When it arrives, `PageHistory::reloadCurrent` swaps it into the current entry keeping both Back *and* Forward (unlike `replaceCurrent`, used by JS `location.replace()`/`.reload()`, which drops Forward), and the scroll position is kept - whatever it is when the new page arrives, since the old page stays up while it loads. Content still loading below (images) can make the new page shorter at that moment, pulling the position up. Always a GET: a page that came from a form POST is reloaded by URL rather than re-submitting the form, which browsers only do after asking. The built-in start page (no URL) is just re-shown, which still re-runs its scripts.
- **Stop:** while any navigation is in flight (`PageLoader::loading()` - a reload, a link, the address bar, a form), the Reload button draws as ✕ (`AddressBar` gets the loading state every frame via `setNavEnabled`). That is also the only on-screen sign that a click on Reload registered - a page that comes back unchanged otherwise looks like nothing happened. Clicking ✕, or Esc with no field focused, calls `pageLoader.cancel()`: the current page stays up and the address bar goes back to its URL (unless the user is editing it).

### Progressive resource loading (`ResourceLoader`)
A page's own `<script src>` and `<link rel="stylesheet">` fetches are **also backgrounded**, and concurrently rather than serially - fixing the bypass `PageLoader` couldn't cover (above). `Engine::parseAndBuild` and `beginScripts` don't fetch anything themselves: they collect every external stylesheet/script URL in document order and hand the list to a `ResourceLoader` (one per resource kind: `styleLoader_`, `scriptLoader_`).

`ResourceLoader::start()` hands every URL to Fetcher's network thread at once (`fetchPageAsync` at `FetchPriority::Blocking`, §13) - it owns no threads itself; the network thread's per-host limit and priorities decide what actually goes out when. Each URL gets a `shared_ptr<Slot>` owned by `slots_`; the network thread holds only a `weak_ptr`. Superseding a batch (a new `loadHTML()`, i.e. a new `start()`) just replaces `slots_`: the old batch's requests still waiting for a connection are skipped before anything is sent (`FetchOptions::stillWanted` sees the slot expired), and any already on the wire finish with their results dropped - the same abandon-in-place pattern `PageLoader` uses, per URL instead of per navigation. (This class used to run its own pool of 4 blocking threads per resource kind.)

The page paints once immediately after `parseAndBuild`/`beginScripts` return - with only inline `<style>` rules and whatever prefix of inline `<script>`s could run synchronously (see below) - then fills in progressively:

- **Stylesheets** apply independently, in *whatever order their fetches complete* - safe because each one's CSS rules get a precomputed "order band" (`kStyleOrderBand * (its 1-based position among `<link>`s)`) added to their specificity tie-break `order` field at *collection* time, not at *append* time. The old synchronous code relied on append order matching document order, which relied on fetching happening serially; that assumption no longer holds, so tie-breaking had to move from "wherever this landed in `doc->styles`" to "this stylesheet's true position in the document," computed up front.
- **Scripts** must still run in strict document order (side effects, shared globals) despite fetching concurrently and completing in arbitrary order. `Engine::advanceScripts()` walks `scriptTasks_` from a cursor, running each task the instant it's ready (inline is always ready; external once its `ResourceLoader` slot is `done`) and **stopping at the first one that isn't** - even if a later task already finished fetching. It's called once synchronously right after starting the fetches (so a page with no external scripts behaves exactly as it did when this all ran synchronously - no added delay), then again every frame from `pollResources()`.
- `Engine::pollResources()` (called from `render()`, once per frame) is the only place newly-arrived resources get applied. It reuses `domState.domDirty` - the same per-frame "did something change, so re-layout" flag already used for JS timers/DOM mutations (§4's frame loop) - rather than inventing a second signal; applying a stylesheet or running a script just sets it.
- Cancellation follows `PageLoader`'s pattern: a new `loadHTML()` call replaces `styleTasks_`/`scriptTasks_`/both loaders' slots outright. Old in-flight or already-skipped work from an abandoned navigation never touches the new page's state.

### Input handling

| Input | Behaviour |
|---|---|
| Left click in the top 40 px | Back/Forward/Reload buttons (`AddressBar::navButtonAt`) - Reload is a Stop ✕ while a page loads - or focus/place the caret in the address bar |
| Left click on the page | 1. `Engine::onClick` (form controls): if it hit a control it runs that control's JS click listeners, then performs the control's action unless a listener called `preventDefault()`; the click is then finished. 2. Otherwise find a link with `linkAt`. 3. `Engine::dispatchClick` runs JS click listeners. 4. If a listener did **not** call `preventDefault()` and there was a link, navigate |
| Mouse 4/5, Alt+←/→ | Back / forward |
| Ctrl+L, F6 | Focus the address bar |
| F5, Ctrl+R | Reload (works while typing in a field too; ignored on key-repeat) |
| Esc (nothing focused) | Stop the page load in progress |
| F12 | Open/close the developer console (§11); opening focuses its input line |
| Keys / chars | Go to the address bar if focused, otherwise to the page: `keydown`/`keyup` fire at the focused field or `document` first (§11) - `preventDefault()` on `keydown` cancels the key's action below and the character it would type - then to the focused page input. The browser's own shortcuts above (F5, Ctrl+L, Ctrl+F, F12, zoom, Alt+←/→) never reach the page |
| Mouse movement | `Engine::updateHover` fires `mouseover`/`mouseout`/`mouseenter`/`mouseleave` when the element under the mouse changes, and updates `:hover` |
| Tab / Shift+Tab | Next/previous text field |
| Ctrl+V / C / A | Paste / copy / select all (clipboard via GLFW). Copy is blocked for password fields |
| Enter | Address bar: `normalizeInput` then navigate. Page field: submit its form |
| Esc | Blur, and restore the address bar text |
| Scroll wheel | `Engine::scroll(-yoffset * 40)`; over the console, scrolls its log; with Ctrl held, zooms |
| ↑ ↓, PageUp/PageDown, Space / Shift+Space, Home/End (nothing focused) | Scroll a line (40 px) / a screenful minus a line / to the top or bottom (`Engine::scrollLines`/`scrollPages`/`scrollToEdge`) |
| Ctrl+= / Ctrl+− / Ctrl+0, Ctrl+wheel | Zoom in / out / reset, through Chrome's steps (25%–500%); works while typing too. The address bar shows "125%" when not at 100%; clicking it resets |
| Ctrl+F; F3 / Shift+F3, Ctrl+G | Find in page; next / previous match (Enter / Shift+Enter in the find field too; Esc closes) |

`AddressBar::normalizeInput` leaves anything containing `://` alone, treats drive-letter paths, UNC paths and existing files as local, and otherwise prepends `https://`.

## 6. The DOM (`DOM.h`)

```cpp
struct Node    { enum Type { ELEMENT, TEXT } type; };
struct TextNode : Node { std::wstring text; };
struct Element : Node {
    std::wstring tag;                       // lower-case
    std::map<std::wstring,std::wstring> attrs;
    std::vector<std::shared_ptr<Node>> children;   // owning
    Element* parent;                        // non-owning
};
struct Document { shared_ptr<Element> root, body; vector<CSS::Rule> styles; };
```

`Document::root` is the top-level element that contains `<body>` (normally `<html>`). Its purpose is **ownership**: `body->parent` points at it, and click bubbling walks `parent` up past `<body>`, so it must stay alive after parsing. (It used to be left unset, so `<html>` was freed at the end of `parse()` and any click that bubbled that far read freed memory.) If you add code that walks `parent`, remember the chain ends at `root`, whose own `parent` is `nullptr`.

- Ownership flows **down** through `shared_ptr` children. `parent` and every `Element*` stored elsewhere (`LayoutBox::el`, `Engine::focusedEl`, JS wrappers) are **raw, non-owning** pointers. They are valid only while the node is in the tree (or in `DOMBindingState::detachedNodes`).
- Form state lives **in the DOM as attributes**: text value → `value`, checkbox → `checked`, selected `<option>` → `selected`. That is why picking an option or typing never needs a re-layout, and why `collectFields` can read a form straight from the tree.

## 7. HTML parsing (`HTMLParser.cpp`)

A hand-written, forgiving, single-pass recursive-descent parser. All text is `std::wstring` (UTF-16).

- **Skipped:** `<!DOCTYPE>`, `<!-- comments -->`, `<? … ?>`.
- **Void tags** (no children, no end tag): `br hr img input meta link area base col embed source track wbr`. `<x/>` also self-closes.
- **Raw-text tags** — `script style title textarea`: content is read verbatim up to the matching end tag. `script` and `style` content is kept as one `TextNode` (no entity decoding, so `&&` in JS survives). `title` content is kept too, but entity-decoded like normal text (`Tom &amp; Jerry`); layout skips `<title>` as before, it only feeds the window title and `document.title`. `textarea` content is **dropped**.
- **Entities:** `&amp; &lt; &gt; &quot; &apos; &nbsp; &copy; &mdash; &ndash; &hellip;` plus numeric `&#N;` / `&#xH;` (BMP only). Decoded in text nodes and attribute values.
- **Text nodes** keep whitespace at either end as a single space (whitespace-only text becomes `" "`) instead of trimming it, so layout knows where the HTML had a gap: `<em>x</em>,` has none, `<em>x</em> ,` has one. Whitespace between words is left as is; layout splits on it. Places that want plain text - an `<option>`'s label/value, a `<button>`'s label - trim and collapse it, as browsers do.
- **Names:** tag names may contain letters, digits, `-`, `:` and `_` (custom elements like `<my-widget>`); attribute names run to whitespace, `/`, `>`, `=` or a quote, as in HTML (so `xlink:href`, `@click`, `:src` stay whole). Both are lowercased.
- **Body discovery:** `findBody` looks anywhere in the tree for `<body>`. If there is none, a synthetic `<body>` wraps all top-level nodes. This is what makes `innerHTML` work: the fragment is parsed as a whole "document" and its synthetic body's children are reused. If `<body>` is itself top-level (a page with `<head>` and `<body>` but no `<html>` wrapper), a synthetic `<html>` becomes `root` and wraps every top-level node, so the `<head>` - its `<title>` and scripts - isn't dropped.
- **Stylesheets:** all `<style>` text is concatenated and parsed once into `Document::styles`. `<link rel="stylesheet" href="...">` isn't touched here at all (`<link>` is just a void tag to the parser) - see `Engine::parseAndBuild` in §10 for where those get fetched.

**End tags** (`parseElement`, with `open_` - the names of the elements being parsed, outermost first): an element's own end tag closes it. An end tag naming an element it's *inside* closes it too and is left for that element to consume, so `</tr>` closes an unclosed `<td>` and `</ul>` an unclosed `<li>`. Any other end tag is a stray and is ignored, as browsers do.

**Implied end tags** (`closedByStartTag`): some start tags end the current element instead of nesting in it - `<td>`/`<th>` end at the next cell, row or row group, `<tr>` at the next row or group, `<thead>`/`<tbody>`/`<tfoot>` at the next group, `<li>` at the next `<li>`, `<dt>`/`<dd>` at the next of either, `<option>` at the next `<option>`/`<optgroup>`. That's what makes old-style `<tr><td>a<td>b` tables parse. Not implemented: the rest of HTML's tree-building rules - an unclosed `<p>` isn't ended by a following block, misnested formatting tags (`<b><i></b></i>`) aren't reopened, and stray content inside a table isn't moved out of it.

**Doctype:** a `<!DOCTYPE>` is otherwise skipped, but sets `Document::quirks`: no doctype, or an HTML 4 transitional/frameset one without a system identifier, means quirks mode; anything else (`<!DOCTYPE html>`) standards mode. Layout uses it for one rule only (§9, Tables).

## 8. CSS (`CSS.h`, `CSS.cpp`)

**Parsing** (`parseStylesheet`): strips `/* */` comments, splits comma groups, and produces one `Rule` per selector with `chain`, `declarations`, `specificity` and source `order`. A rule's declaration body is found via brace-depth counting (`matchBrace`, shared with `skipAtRule`'s own `@media {...}` skipping), not a plain search for the next `}` - real-world minified CSS uses native nesting (a selector block declared directly inside another rule's body, e.g. `.a{ .b{...} .c{...} }`, no `@` involved), and a naive next-`}` search stops at the *inner* rule's close instead of the outer one's, desyncing every rule parsed afterward for the rest of the file.

**`@`-rules:** a *bare-media-type* `@media` block - `@media screen{...}`, `@media print{...}`, `@media all{...}`, or no type at all - is evaluated outright (`isSimpleScreenMedia`): `screen`/`all`/none are always true (this engine only ever renders on-screen) and `print` is always false, and its content is parsed as if it weren't wrapped at all (the parse cursor just moves past the block's own opening `{`; the loop's existing stray-`}` handling consumes the closing one once the content's exhausted - no separate recursive parser needed).

A `@media` condition combining a type with `(min-width:Npx)`/`(max-width:Npx)` via `and` (`parseMediaCondition`) is also evaluated, but differently: unlike a bare type, it can't be decided at parse time (the viewport can change - a resize), so `parseStylesheet` instead *always* descends into the block and tags every `Rule` found inside with the width bound (`Rule::mediaMinWidth`/`mediaMaxWidth`, tracked via a small stack of `{endPos, minWidth, maxWidth}` scopes - `endPos` from `matchBrace`, popped once the parse cursor reaches it; nested media blocks AND their bounds together). `LayoutRoot::computeStyle` checks a rule's bounds against the live `viewportWidth` right alongside `CSS::matches` - since `layout()` already does a full relayout on every resize (no incremental layout anywhere in this engine), this makes width queries resize-reactive for free, with no separate reactivity system needed.

Every other `@`-rule - `@supports`, `@import`, `@font-face`, `@keyframes`, a comma-separated media query list, or a feature other than `min-width`/`max-width` (`prefers-color-scheme`, `hover`, `orientation`, `resolution`, ...) - is still always skipped, never conditionally applied.

**Selectors supported** (`parseSelector`/`parseCompound`):
- Simple selectors: `tag`, `.class`, `#id`, `*`, compounded (`div.card#x[title]:hover`). Identifiers may use backslash escapes (`.md\:flex`, as Tailwind emits); hex escapes (`\31 0`) aren't decoded.
- All four combinators: descendant (`a b`), child (`a > b`), next sibling (`a + b`), subsequent sibling (`a ~ b`).
- Attribute selectors (`AttrSelector`): `[attr]`, `[attr=v]`, `~=`, `|=`, `^=`, `$=`, `*=`, quoted or bare values, and the trailing ` i` case-insensitive flag.
- Pseudo-classes (`PseudoClass`): `:hover` (below); structural `:first-child`, `:last-child`, `:only-child`, `:nth-child()`, `:nth-last-child()`, `:first-of-type`, `:last-of-type`, `:only-of-type`, `:nth-of-type()`, `:nth-last-of-type()` (all `An+B` forms, `odd`, `even`); `:root`, `:empty`, `:link`/`:any-link`; and `:not()` with one or more comma-separated *compound* arguments. `:visited`, `:active`, `:focus`, `:focus-visible`, `:focus-within` and `:target` parse but never match (the engine has no such state), so a selector list containing them still loads.
- **Not supported** (the whole selector is skipped, as before): pseudo-elements (`::before`, the legacy `:before`, …), `:is()`/`:where()`/`:has()`/`:lang()`, `:nth-child(An+B of S)`, combinators inside `:not()`, and any other pseudo-class. Specificity counts attribute selectors and pseudo-classes as classes, and `:not()` as its (most specific) argument.

`splitTop` (used to split both selector lists and declaration blocks) doesn't split inside `(...)`, `[...]` or quotes, so `:not(.a, .b)` and `url(data:...;base64,...)` stay whole.

**Matching** (`CSS::matches` → `matchFrom`): right to left. The last compound must match the element; each earlier one is then tried against the candidates its combinator allows - the nearest ancestor for `>`, every ancestor for a space, the previous element sibling for `+`, every previous sibling for `~` (siblings come from `parent->children`) - backtracking if a candidate fails further left. A chain of *only* descendant combinators keeps the original greedy behavior (the nearest matching ancestor is always at least as good as a farther one), so the common case costs no more than before.

**`:hover`:** matching takes an optional `CSS::HoverSet` - the element under the mouse and all its ancestors - which `:hover` checks by pointer only. `main.cpp` calls `Engine::updateHover(x, y)` every frame after rendering; it hit-tests with `elementAt` (the same topmost-box test `dispatchClick` uses), and when the set changes it relayouts only if `CSS::hoverCouldAffect` finds a `:hover` compound that matches an element that entered or left the set. Two guards: no relayout while a `<select>` dropdown is open (a relayout would close it; the change is retried each frame), and none at all if the last layout took over `kHoverRelayoutBudgetMs` (50 ms) - `:hover` is a full relayout here, so on a large page (Wikipedia: ~1.7 s) hover styles are simply ignored rather than freezing the window on every mouse move. After a JS DOM mutation the old set's elements may have been freed (`Engine::hoverSetLive`), so they're never dereferenced; a change is then assumed to matter.

**Rule index** (`LayoutRoot::RuleIndex`, rebuilt at the start of every `layout()`): each rule is bucketed by its rightmost compound's id, else its first class, else its tag, else "universal". `computeStyle` only tests an element against the universal bucket plus the buckets for its own tag, id and classes, instead of every rule on the page. On Wikipedia's Tiger article this took layout from 5.1 s to 1.7 s (Debug) with 1456 rules - more than double the 623 rules that parsed before the new selector support, and still faster than the old 2.2 s. The optional `ClassCache*` still avoids re-splitting `class=""` for each check.

**Ancestor filter** (`LayoutRoot::AncestorFilter`): most rules left in a bucket still fail, on their ancestor part - `.navbox a` is in the `a` bucket, so every link was tested against it. So each rule also gets a 512-bit Bloom filter of the tags, ids and classes it requires of the element's ancestors (every compound whose right-hand combinator is a descendant or child one - a compound left of `+`/`~` is a sibling, not an ancestor), built in `rebuildRuleIndex`. `ancestorFilter()` keeps the same kind of filter for `ancestorStack`, as a stack beside it, revalidated against it on each call (it's pushed, popped and swapped wholesale in many places). A rule whose required bits aren't all set is skipped without calling `CSS::matches`. On the Tiger article this cut rule tests per layout from 522k to 134k - the 18,138 rules that matched still did - and matching from ~79 ms to ~20 ms (Release).

**Applying declarations** (`applyDecl`): each declaration's property is looked up once in a map to a small enum (`Prop`), and the chain of branches compares that number instead of the name; a property the engine doesn't support stops at the lookup.

**Cascade** (in `LayoutRoot::computeStyle`): matched rules are stable-sorted by specificity `(ids, classes, tags)` then source order and applied in that order; then the inline `style=""` attribute is applied last (always wins). `!important` is stripped but gives no extra priority.

**Properties the engine actually honours:**

| Property | Handling |
|---|---|
| `background`, `background-color` | Any CSS color (see `color` below). `background-color` ignores an invalid value. The `background` shorthand takes its colour from its last layer (a shorthand with no colour, like `none` or only an image, clears it), and its layers as below |
| `background-image`, `-size`, `-position`, `-repeat`, and the shorthand's layers | Comma-separated layers (`BackgroundLayer`), each a `url(...)` or a gradient, painted inside the border with the first listed on top. Size: `auto`, `cover`, `contain`, or one/two lengths/percentages (an `auto` side keeps the image's proportions). Position: keywords, percentages (that point of the image on that point of the box) and lengths, in the 1/2-value forms (the 3/4-value offset forms keep only their keywords). Repeat: `repeat`/`no-repeat`/`repeat-x`/`repeat-y` per axis (`space`/`round` tile like `repeat`). In the shorthand: `<position> / <size>`, a repeat, the image, in any order. The shorthand and longhands override each other in cascade order, property by property (`Recorded` in `computeStyle`); a longhand's list repeats across the layers. A relative `url()` in an external stylesheet is resolved against the stylesheet (`absolutizeCssUrls`, when it arrives). An image covering the whole box follows `border-radius`; a smaller or tiled one is clipped to the box's rectangle |
| `linear-gradient()`, `radial-gradient()`, `repeating-*` | Linear: an angle (`deg`/`turn`/`rad`/`grad`), `to <side>` or `to <corner>` (whose angle depends on the box's shape, as CSS defines). Radial: `circle` or `ellipse` (always farthest-corner sized) and `at <position>` (percentages/keywords). Colour stops with 0-2 positions each, in % or px; missing ones spread evenly. The old `-webkit-`/`-moz-` syntax (a bare side names the start) is accepted. Colours are blended with premultiplied alpha, so fading to `transparent` doesn't pass through grey |
| `box-shadow` | Comma-separated `[inset] <x> <y> [<blur> [<spread>]] [<color>]` (no colour = the text colour); the first is on top. The shadow follows `border-radius` (grown by the spread) and its edge fades over the blur. **Inset shadows aren't drawn.** As in CSS, none of a shadow is painted under its box: for a box whose background isn't opaque, the shadow is drawn clipped to the four bands around the box's rectangle (`Engine::paintShadows`), so `box-shadow: 0 1px #ccc` is a 1px line under it. With rounded corners, the bits inside the rectangle but outside the curve are cut away too. Applies to blocks, images and form controls |
| `color` | `#rgb`, `#rgba`, `#rrggbb`, `#rrggbbaa`, `rgb()`/`rgba()` and `hsl()`/`hsla()` (comma or space syntax, numbers or `%`, optional alpha; hue in `deg`/`rad`/`grad`/`turn` or unitless degrees), the 148 CSS named colors, and `transparent` (`tryParseColor`, `Engine.cpp`). Inherited (`LayoutRoot::TextPaint`, threaded down alongside `inheritedFontSize`). An invalid value, `inherit` or `currentcolor` keeps the inherited color. `<a href>` defaults to blue, which an author rule on the link overrides |
| `font-weight` | `bold`/`bolder`/numeric >= 600 = bold; `normal`/`lighter`/numeric < 600 = regular. Only two weights exist (GDI Segoe UI regular/bold). Inherited. `<b>`, `<strong>`, `<th>` and `<h1>`-`<h6>` default to bold. Bold text is measured bold too, so wrapping stays correct |
| `margin`, `padding` | Shorthand: 1/2/3/4 space-separated values, expanded per the usual CSS rule (`T`, `T/R`, `T/R-L/B`, `T/R/B/L`) |
| `margin-top/right/bottom/left`, `padding-top/right/bottom/left` | Individually, px, `em` (of the element's font size), `rem` (of the 14px root) or `%` (of the containing block's width - CSS's rule for percentage margin/padding on every side, top/bottom included). Defaults and margin collapsing: see §9 |
| `width` | px, `%` of the containing block's width, or `vw`/`vh`; unset (or `auto`) fills the container, as always |
| `min-width`, `max-width` | px, `%`, `vw`/`vh` (`max-width: none`), in the same box as `width` (per `box-sizing`). Clamp blocks (an auto width that fills the container included), images - keeping their proportions when their height wasn't set, so `img { max-width: 100% }` shrinks them - form controls, absolute elements, and flex items after flexing (the space a clamped item gives up isn't passed on) |
| `margin: auto` (`margin-left`/`margin-right: auto`, or `auto` in the shorthand) | A block narrower than its container takes the leftover space in its auto margin(s): both centre it (`margin: 0 auto`), one pushes it to the other side. For an absolute element with `left` and `right` both set, the space between them is what's shared - so `left: 0; right: 0; width: …; margin: auto` centres it. In a flex row, auto margins on items take the line's leftover space before `justify-content` (which then does nothing), and items without a width are sized to their content rather than given the engine's usual implicit `flex-grow` - so `margin-left: auto` pushes an item to the end, as in a nav bar |
| `border`, `border-width`, `border-color` | `border: 1px solid <color>`-style shorthand (any CSS color, tokens split outside parentheses so `rgb(1, 2, 3)` works; `none`/`hidden` = width 0), or the longhands. The style keyword (`solid`/`dashed`/…) is accepted but ignored - every border draws the same way |
| `border-radius`, `border-*-*-radius` | 1-4 values (px, `%`, `em`/`rem`, or `0`) for the four corners; an elliptical `/ <vertical>` part is dropped, so corners are always circular. A `%` is of the box's smaller side, so `50%` makes a square a circle but a wide box a pill, not an ellipse. Radii are scaled down together where neighbours would overlap, as in CSS (`LayoutBox::cornerRadii`). Applies to background/border boxes, `<img>` (rounded avatars), and form controls |
| `--name: value`, `var(--name[, fallback])` | Custom properties, inherited (`CSSVars`, carried on `TextPaint`). Every one an element defines is collected first, then `var()` is substituted into every declaration before it's applied, so order in the cascade doesn't matter. A `var()` with neither a value nor a fallback makes its whole declaration invalid, as does a cycle. Each element only adds a layer when it changes a value, so Tailwind-style `* { --tw-...: ... }` rules don't copy the whole set per element. Names are matched case-insensitively (the declaration parser lowercases them), unlike real CSS |
| `box-sizing` | `content-box` (default) or `border-box`; see §9 |
| `grid-template-columns`, `grid-template-rows` | Space-separated tracks: px, `%`, `em`/`rem`, `vw`/`vh`, `fr`, `auto`/`min-content`/`max-content`/`fit-content()` (sized to their content), `minmax(<min>, <max>)` (sized by its max), and `repeat(N, <track>)`; see §9's Grid subsection. `fr` and content-sized rows are treated as auto (no definite container height to distribute against) |
| `grid-template-areas` | One or more quoted strings (single or double quotes), each a grid row, each whitespace-separated token a column's area name (`.` = no area); see §9 |
| `grid-template` | Shorthand - the area-string form (e.g. `"a a" 40px "b c" 1fr / 100px 1fr`) or the plain `<rows> / <columns>` form; see §9 |
| `grid-area` | On a grid item: the area name to place into (must match a name in the container's `grid-template-areas`) |
| `grid-column`, `grid-row` | On a grid item: each side is `auto`, a line number (negative counts from the end, so `1 / -1` spans every column), or `span N` - e.g. `"span 8"`, `"2 / 4"`, `"2 / span 3"`, `"span 2 / 5"`, with or without spaces around the `/`. Also the `-start`/`-end` longhands. Either axis can be set alone; the other is auto-placed. `grid-area` takes precedence if both are set and the area name resolves |
| `gap`, `row-gap`, `column-gap` | `gap: <row>` or `gap: <row> <column>`, px or `%` - shared by grid and flex, same properties either way |
| `flex-direction` | `row` (default) or `column`; see §9's Flex subsection |
| `justify-content` | `flex-start` (default), `center`, `flex-end`, `space-between`, `space-around` |
| `align-items` | `stretch` (default), `flex-start`, `center`, `flex-end` |
| `flex-grow`, `flex-shrink`, `flex-basis`, `flex` | On a flex item. `flex-basis` is a px/`%` outer width, or `auto` (fall back to `width`). The `flex` shorthand follows the spec: `none` = `0 0 auto`, `auto` = `1 1 auto`, otherwise up to two numbers (grow, shrink) and a basis, with an omitted basis meaning `0` (so `flex: 1` shares the line equally regardless of `width`) |
| `flex-wrap`, `flex-flow` | `nowrap` (default), `wrap`, `wrap-reverse`; `flex-flow` sets direction and wrap together. Row direction only - see §9 |
| `font-size` | px, `em`, `rem`, `%` (relative to the inherited size); inherited by children |
| `font-family` | The first family in the list that's installed (enumerated once with `EnumFontFamiliesExW`), with generic names mapped: `sans-serif`/`system-ui`/`-apple-system` → Segoe UI (the default), `serif` → Times New Roman, `ui-serif` → Georgia, `monospace`/`ui-monospace` → Consolas, `cursive` → Comic Sans MS, `fantasy` → Impact. Web fonts (`@font-face`) aren't loaded, so a page's own font falls through to its next entry; a list naming nothing installed keeps the inherited face. `<code>`/`<pre>`/`<kbd>`/`<samp>`/`<tt>` default to monospace. Inherited; the chosen face is interned (`internFont`) so it's a pointer per word, not a string |
| `font-style` | `italic`/`oblique` vs `normal`. `<em>`/`<i>`/`<cite>`/`<var>`/`<dfn>`/`<address>` default to italic. Inherited |
| `font` | `[style] [variant] [weight] <size>[/<line-height>] <family list>`; whatever isn't given resets to normal, as the shorthand does |
| `text-align` | `left`/`start`/`justify` (justify isn't stretched), `center`, `right`/`end`. Moves each line of a block's inline content by its leftover width (`layoutInlineRun`). `<center>` and `<th>` default to center. Inherited. `-webkit-center` (what `<center>` and `align=center` on a `<div>` or table part map to) also centres block children narrower than the container - a table inside `<center>`, say (`TextPaint::centerBlocks`, applied in `layoutBlockChild`) |
| `vertical-align` | On a table cell, row or row group: `top` (`baseline`/`text-top` treated as top), `middle`, `bottom`. A cell without one takes its row's, then its row group's, else `middle`. Ignored elsewhere |
| `border-collapse`, `border-spacing` | On a table: `separate` (default) with `border-spacing: <x> [<y>]` px between cells and around them (default 2px), or `collapse` - see §9, Tables |
| `line-height` | `normal` (font size + 8, as before), a number (a multiple of each descendant's own font size), or px/em/% (fixed at the declaring element's font size, then inherited as pixels) - as in CSS. Sets the height of the text band of each line; each word is centred in it. Inherited |
| `text-decoration`, `text-decoration-line` | `underline`, `line-through` (`overline` is accepted and not drawn). Drawn in the text colour, continuous across the space to the next word when it's decorated the same way. Links are underlined by default, `<u>`/`<ins>` too, `<s>`/`<strike>`/`<del>` struck through. Treated as inherited (an ancestor's decoration runs through its descendants' text) - but unlike CSS, a descendant's `none` does remove it |
| `text-transform` | `uppercase`, `lowercase`, `capitalize`, `none` - applied to the words as they're split for layout (`appendWords`); the DOM text is unchanged. Inherited |
| `display` | `none`, `block`, `inline`, `inline-block` (treated as inline), `grid`, `flex`, `table` (`inline-table` treated as `table`), `table-row`, `table-cell`, `table-row-group`, `table-header-group`, `table-footer-group`, `table-caption`; `table-column`/`table-column-group` are treated as `none`. Table parts outside a table are laid out as plain blocks. `flow-root` is a block that contains its floats |
| `float`, `clear` | `float: left`/`right` (`inline-start`/`inline-end` treated as left/right) and `clear: left`/`right`/`both`; see §9, Floats. `<img align=left/right>` and `<table align=left/right>` float, and `<br clear=all/left/right>` clears |
| `opacity`, `visibility` | `opacity <= 0` or `visibility: hidden`/`collapse` (own or inherited from an ancestor) makes an element's boxes skip painting (`LayoutBox::visuallyHidden`, checked in `Engine::render`) - but *not* layout: it still occupies its normal space, matching real CSS, unlike `display:none`. Click hit-testing is unaffected (a simplification - real CSS only blocks it for `visibility:hidden`, not `opacity`) |
| `height`, `min-height`, `max-height` | px, `em`/`rem`, `vh`/`vw`/`vmin`/`vmax`, or `%` of the containing block's height when that is definite (an explicit height, or the viewport down through `html`/`body { height: 100% }` - `LayoutRoot::containingHeight_`), else auto, as in CSS. Content-box by default, border-box with `box-sizing`. A block's content height is its own height if set, else its children's, then clamped by min/max; content taller than a set height overflows - drawn anyway unless `overflow` clips it - but doesn't push later content down. Also sizes `<img>`/`<svg>` and form controls. In a flex row, only an item with `height: auto` is stretched by `align-items` |
| `overflow`, `overflow-x`, `overflow-y` | Anything but `visible` clips the element's descendants to its padding box along that axis (`LayoutBox::clipped`, set by `clipBoxes`; nested clips intersect; painted with the renderer's clip stack, `pushClip`/`popClip`). There's no scrolling inside an element: `auto`/`scroll` clip like `hidden`. The clip is a rectangle, so it doesn't follow `border-radius`. An absolute descendant whose containing block is outside the element isn't clipped by it, as in CSS |
| `position`, `top`/`right`/`bottom`/`left`, `inset` | See "Positioning" below. Offsets: px, `em`/`rem`, `vh`/`vw`, or `%` of the containing block (negative values work) |
| `z-index` | Integers or `auto`, on positioned elements: boxes are stably sorted by a paint key, 4 × z-index + 2 for a positioned element (0 for the flow, 1 for floats), so the same z paints above the flow and floats, and higher z later. An explicit z-index flattens its descendants into its own key (a simplified stacking context) |

**Presentational attributes** (`tableHints`): on table parts, the old HTML attributes become declarations placed *before* every author rule in the cascade, so CSS overrides them - `width`, `height`, `bgcolor`, `align` (on the table itself: centring it; elsewhere: `text-align`), `valign`, a table's `border` (that width, gray, plus a 1px border on every cell) and `cellspacing` (`border-spacing`), and on cells the enclosing table's `cellpadding`. A `<div>`'s `align` too.

Everything else (`letter-spacing`, `transform`, …) is parsed and ignored. `em`/`rem` aren't supported for `width`/`margin`/`padding`/`border-width`/grid tracks/`gap` (only `font-size`, heights and offsets resolve those) - use px, `%` or `vw`/`vh`.

### Positioning (`layoutBlockChild`, `layoutOutOfFlow`)
Boxes are one flat list in document coordinates, and a subtree's boxes are a contiguous range of it, so positioning works on ranges:
- **`relative`:** laid out in the flow, then its range is shifted by its offsets - the flow doesn't move.
- **`absolute`:** taken out of the flow where it's met (`deferOutOfFlow`, which records its static position and the ancestors its descendants' selectors need) and laid out once its **containing block** - the nearest positioned ancestor's padding box, or the viewport - has its final size (`layoutOutOfFlow`), so `right`/`bottom` work. Width: its own, or between `left` and `right`, or shrink-to-fit; height: its own, or between `top` and `bottom`; position from `left`/`top`, else `right`/`bottom`, else its static position. Like a flex item it's laid out at (0, 0) and moved into place; its boxes go after the flow's, painting over it.
- **Static position:** where the element would have been in the flow - for a block-level one, below the text before it (pending inline text is laid out first). It's recorded as an *anchor*: a zero-size `LayoutBox` with a non-zero `anchor` id, pushed where the element was met. Flex/grid items, table cells and floats are laid out at local `(0, 0)` and moved into place afterwards, and relative positioning shifts its subtree; the anchor is moved along with the boxes around it, so `layoutOutOfFlow` reads the real position from it (scanning the containing block's own boxes for the ids it needs). Anchors are removed at the end of `layout()` and never painted. An absolute element met inside inline content (`collectInline`) has no anchor and starts from its containing block's corner, as before.
- **`fixed`:** the same, against the viewport, and its boxes are flagged `fixed`: their `y` is from the top of the viewport.
- **`sticky`** (with `top`): in the flow; its boxes carry an index into `LayoutRoot::stickies`, and while scrolling they shift down to keep `top` from the viewport's top, until they meet the bottom of the parent's content box.
- **On screen:** `Engine::boxShift` gives a box's scroll-dependent offset (scrollY for fixed, the sticking distance for sticky); painting and every hit test (`elementAt`, `controlAt`, `linkAt`, via `boxContains`, which also respects clips) add it, so a fixed header is clickable where it's drawn. Fixed boxes don't count towards the page's height.
- **Relayout:** layout records whether it used the viewport's height (`usedViewportHeight`: `vh`, fixed/root-absolute boxes, `height: 100%` from the viewport); only then does a change of window height (or the console opening) relayout.

`CSS::parseSelector` and `CSS::matches` are also reused by JS `querySelector`.

## 9. Layout (`Layout.h`, `Layout.cpp`)

`LayoutRoot::layout()` starts at `<body>` at `(10, 10)` with width `viewportWidth − 20` and walks the DOM once, appending to `boxes`.

### `LayoutBox`
`x, y, width, height` (document space), plus optional `background`, `borderWidth`/`borderColor`, `text`, `href`, `imageSrc`, `fontSize`, `control` (`TextField | Button | Checkbox | Select`), `el` (source element) and `form` (enclosing `<form>`). One struct serves as a background/border rectangle, a single word of text, an image, or a form control.

### Default spacing and margin collapsing
**Defaults** (`computeStyle`, before the cascade, like a browser's built-in stylesheet): a block has no margin or padding unless its tag has one.
- **Vertical margins in `em`** of the element's final font size (resolved after the cascade, unless an author rule set the margin): `<p>`, `<dl>`, `<pre>`, `<blockquote>`, `<figure>` and lists 1em (a list inside a list none); `<h1>`-`<h6>` .67/.83/1/1.33/1.67/2.33em; `<hr>` .5em.
- **Indents:** lists 40px of left padding; `<dd>` 40px left margin; `<blockquote>` and `<figure>` 40px on both sides.
- **Headings** are 2/1.5/1.17/1/.83/.67em of the inherited font size, and bold.
- **`<hr>`** is a 1px gray border.
- **Table parts** have their own (see Tables).

**Margin collapsing** (`layoutBlockChild`, with `marginEndY_`/`pendingMargin_`/`openTops_`): adjoining vertical margins merge into one - the largest positive plus the most negative (`combineMargins`).
- **Siblings:** a block's bottom margin is left "pending" where it ends; a block starting exactly there (nothing laid out in between - text would have moved `y`) merges its top margin with it.
- **Parent and first child:** a block with no top border or padding (and no float context of its own) leaves its own top margin pending for its first child. If that child merges with it, the parent's box top moves down to the child's (and so on up, through ancestors that merged the same way).
- **Parent and last child:** with no bottom border, padding or set height, a margin pending at the end - the last child's bottom margin, or an empty block's own top margin - carries through and merges with the block's bottom margin.
- **Not merged:** float context roots (flex and grid items, table cells, floats, `overflow` other than visible - laid out by `layoutItemDetached`, which isolates them) keep their margins to themselves. Margins of form controls and images (`layoutControl`/`layoutImage`) don't merge.
- **The page root:** the 10px above `<body>`'s content stands in for its margin and merges with its first child's.

Not supported: clearance-specific rules beyond "a cleared block doesn't merge" (its `y` moved), and the logical `margin-block`/`margin-inline` properties.

### The box model (`ComputedStyle`, `layoutElement`'s block branch)
`ComputedStyle` carries the full box model for an ordinary block element: `marginTop/Right/Bottom/Left`, `paddingTop/Right/Bottom/Left`, `width` (`-1` = auto), `borderWidth`/`borderColor`, and `boxSizing` (`ContentBox` | `BorderBox`). `layoutElement`'s block branch turns these into two widths before laying out children:

- **`outerWidth`** - what actually gets painted (the border/background edge). With `width` unset, it's `containingWidth - marginLeft - marginRight`, same as always. With `width` set: under `content-box` (the default), `width` names the *content* box, so outer = `width + padding + 2×border`; under `border-box`, `width` already *is* the outer size.
- **`contentWidth`** - `outerWidth` minus padding and border, and what children are actually laid out into.

This box-model computation - and reserving the background/border box, sized to `outerWidth`, height patched in once the content's natural height is known - is factored into `layoutBlockChild(e, x, y, containingWidth, style)`, called once per block-level child from `layoutElement`'s loop and, for each grid/flex item, from `layoutGrid`/`layoutFlex` (below). Once the box model is resolved, `layoutBlockChild` recurses into `layoutElement(e, ...)` as always - unless `style.display` is `Grid` or `Flex`, in which case it calls `layoutGrid(e, ...)` or `layoutFlex(e, ...)` instead.

This still means **explicit CSS `height` isn't supported** - a box is always exactly as tall as its content, `overflow: visible`-style clipping/`height` isn't modeled. `width`/`border`/`box-sizing` currently apply to plain block and grid/flex-item elements only, not to `<input>`/`<button>`/`<select>` (`layoutControl`, unchanged) or `<img>` (`layoutImage`, unchanged) - those keep their own fixed/intrinsic sizing.

`Engine::render` paints a border as four thin rects forming a hollow frame (not one filled rect), so a border with no background still lets whatever's behind the box show through the middle, then paints the background inset by the border width.

### Grid (`layoutGrid`)
`display: grid` on a block-level element runs `layoutGrid` instead of the usual vertical flow, but only for *that element's own children* - everything above (the grid container's own margin/padding/border/background) is unchanged, since `layoutBlockChild` handles that identically for a block or a grid container.

1. **Columns**: `grid-template-columns` is parsed (`parseGridTemplateTracks` - also used for rows, below, since the track grammar is identical on both axes) into a list of `GridTrack { isFr, value, isAuto, cap }`:
   - **Track kinds:** a fixed width (px, `%`, `em`, `rem` against the 14px base, `vw`/`vh`); an `fr` share; or a content-sized track (`auto`, `min-content`, `max-content`, and `fit-content(<cap>)` with a cap).
   - **`minmax(<min>, <max>)`** is sized by its max: an `fr` share, the content, or - for a length - growing like `1fr` up to that length (`cap`). The min isn't enforced.
   - **`repeat(N, <track>)`** is expanded textually first (`"repeat(3, 1fr)"` → `"1fr 1fr 1fr"`, matching parentheses so `repeat(3, minmax(0, 1fr))` works), and tracks are split outside parentheses (`cssTokens`).
   - **Anything else** becomes `1fr`, so the *number* of columns an author wrote is always honored even where the sizing isn't.
   - **Padding and fallback:** if `grid-template-areas` names more columns than there are tracks for, the missing ones are padded with `1fr`; with no `grid-template-columns` (or `-areas`) at all, it falls back to one column spanning the full width (items stack, rather than the grid disappearing).
   - **Pixel widths** are worked out *after* placement (step 2), since content-sized columns depend on what's in them: fixed tracks first; then each content-sized track, as wide as the widest item placed in it alone (`shrinkToFitWidth`, or its own `width`), capped by what's left and by its cap; then the `fr` tracks share the rest, each up to its cap.
2. **Placement**: `el`'s direct element children (skipping the usual non-visual tags and any `display: none`) are read into an `ItemPlacement` list. An item with `grid-area` naming a cell that appears in `grid-template-areas` (`parseGridTemplateAreas` - one grid row per quoted string, whitespace-separated column names within it, `.` meaning no area) is placed at that name's bounding box across every cell it appears in - real CSS requires those cells to form a rectangle; this doesn't specially validate that, it just takes the bounding box regardless. Otherwise its `grid-column`/`grid-row` (`parseGridLinePlacement`, each side via `parseGridLine`: `auto`, a line number, or `span N`) resolve per axis into a 0-based cell range - a negative line counts back from the explicit grid's last line - or, if neither side gives a line, an *auto* axis with just a span size. Columns are clamped to the template's count (a line beyond it doesn't create an implicit column); rows grow as needed. Placement then follows CSS's sparse `grid-auto-flow: row` algorithm: items definite in both axes claim their cells first (tracked in a `std::set<std::pair<int,int>>`), then items with only a definite row take the first columns that fit in that row, then the rest go in source order behind a `(row, col)` cursor that only moves forward - a definite-column item drops the cursor a row if its column is already behind it, and a fully auto item takes the next spot where its whole span fits (a `span 3` that doesn't fit in what's left of a row wraps to the next). Simplification: a definite row that's already full overlaps instead of growing implicit columns, and there's no `dense` packing. **Not supported:** alignment properties, and a bare (non-element) text node directly inside a grid container is dropped rather than becoming an anonymous item.
3. **Row height**: a row's height depends on every item in it (more so now that an item can span several rows), which `layoutElement`'s single top-to-bottom `y` sweep can't express - so every item is laid out once, into a scratch `boxes` vector at local `(0, 0)` (`std::swap(boxes, scratch)` redirects every `boxes.push_back` anywhere in that call - `layoutControl`, `layoutImage`, or `layoutBlockChild` for a plain item, chosen the same way `layoutElement`'s loop would) to discover its natural height, *before* any row height is decided. An explicit `grid-template-rows` track wins for a given row if set; `fr` row tracks are treated as unset instead (see its `ComputedStyle` comment - `fr` needs a definite container height to distribute against, which this engine's always-auto-height pages don't have, the same wall `layoutFlex`'s column direction hits). Otherwise a row is as tall as the tallest *single-row* item placed in it; an item spanning multiple rows bumps the *last* row it spans if the rows it's already in (plus the row-gaps between them) aren't tall enough for it, rather than distributing the shortfall across all of them. `grid-template-areas` sets the row *count* too, even for a trailing row nothing ends up placed in (e.g. one made entirely of `.` cells). Once every row's height is settled, each item's saved boxes are translated by `(columnX[colStart], rowY[rowStart])` and appended to the real `boxes`. A grid item that would otherwise be `display: inline` (e.g. a bare `<span>`) is "blockified" first, matching real CSS - there's no flowing paragraph for it to join inside a cell.
4. `ancestorStack` gets `el` (the grid container) pushed for the whole function, so a descendant selector matching a grid item still sees the container as an ancestor, exactly as it would for a plain block's children.
5. **The `grid-template` shorthand** (`parseGridTemplateShorthand`) has two forms:
   - **Area strings:** alternating quoted area-rows (single or double quotes, as in `grid-template-areas`) and an optional row-size token right after each one's closing quote, then an optional `/ <column tracks>` suffix (e.g. `"header header" 40px "sidebar main" 1fr / 100px 1fr`). A row with no size token gets `GridTrack{isFr:true, value:0}` - a zero-share `fr` track, which the row-height logic above already treats as unset, so "no size given" rides that same rule instead of needing a new one.
   - **Plain:** `<rows> / <columns>` - two track lists.
   
   Wikipedia's page layout needs both of these: its `.mw-page-container-inner` grid uses the plain form for its tracks and single-quoted `grid-template-areas`. Before either was supported, its sidebar, article and footer were auto-placed into a 2×2 grid of equal columns.

### Flex (`layoutFlex`)
`display: flex` runs `layoutFlex` instead of the usual vertical flow, the same way `display: grid` runs `layoutGrid` - only for the container's own children, box model unchanged. Row and column direction are different enough (which axis is "main" swaps entirely) that they're really two algorithms sharing one function.

**Row direction** (`flex-direction: row`, the default):
1. **Hypothetical size.** Each item's starting outer width is its `flex-basis` if set, else its explicit `width`, else its content's width (never less than its own padding+border). The content width is a shrink-to-fit measurement (`shrinkToFitWidth`, below), standing in for CSS's max-content size; as in CSS, an item only grows past it if `flex-grow` says so.
2. **Lines.** With `flex-wrap: wrap`/`wrap-reverse`, items are packed greedily into lines (an item wider than the container gets a line to itself); `wrap-reverse` stacks the lines bottom-up. Otherwise everything is one line.
3. **Flexing, per line.** Positive free space is split by `flex-grow`; negative free space (overflow) is taken back by `flex-shrink` (default 1) weighted by each item's basis - the spec's "scaled shrink factor". Either way no item ends up below its **min-content width** (CSS's `min-width: auto` for flex items - `minContentWidth`, but never above the item's own `width` if it set one): an item that would is frozen there and the free space worked out again for the rest, round by round. The min-content width is only measured where it could matter - an item being shrunk, or one whose basis didn't come from its content (`flex: 1`'s basis of 0, say). The flexed width replaces the item's own `width` when it's laid out.

**Content-width measurement** (`contentWidth`, behind `shrinkToFitWidth` and `minContentWidth`): the item is laid out in measuring mode (`measuring_`, as table cells are measured) - at the available width for its max-content estimate, at width 0 for its min-content one - and how far its content reaches is measured. In measuring mode nothing that would make content reach further than it needs is applied:
- text isn't aligned or broken mid-word;
- flex rows hand out no free space (`flex-grow`, `justify-content`, auto margins);
- blocks aren't centred (`margin: auto`, `<center>`);
- `fr` grid columns (and an untemplated grid's one column) are content-sized;
- right floats go on the left, adding just their width;
- a percentage `width` counts as auto, as CSS treats it for intrinsic sizes.

Absolute elements met while measuring go to a throwaway containing block.
4. **Placement, per line.** Each item is laid out once into a scratch `boxes` vector at local `(0, 0)` (`layoutItemDetached`, the same `std::swap(boxes, scratch)` technique `layoutGrid` uses) to discover its height; the line is as tall as its tallest item. `justify-content` distributes whatever width is left after flexing, and `align-items` positions items within the line. `align-items: stretch` (the default) is approximated by extending each item's own background/border box to the line's height rather than re-flowing its content. Lines stack with `row-gap` between them.

**Column direction** (`flex-direction: column`) - no equivalent sizing problem, since the main axis is height, and height is exactly what ordinary block layout already produces from content:
- Items are normal block children stacked vertically, honoring `row-gap`/`gap`.
- `justify-content` has no effect: distributing leftover space along a container's height needs a *definite* height to distribute within, and this engine's pages are always "auto" height (they grow to fit content) - the same behavior real CSS flexbox shows for an auto-height flex column, not a shortcut unique to this engine.
- `align-items` does work, on the cross axis (horizontal, here): an item with an explicit `width` can be positioned via `flex-start`/`center`/`flex-end` within the container's width; one without always fills it (`stretch`, the default, and the only sensible behavior for something with no natural width to fall back to - same reasoning as row direction's fallback).

`flex-wrap` has no effect on a column: wrapping a column needs a definite height, and an auto-height flex column is always single-line in real CSS too.

**Not supported (either direction):** `align-content` (lines always pack to the top), `align-self`, `order`, and `row-reverse`/`column-reverse` (treated as `row`/`column`).

### Floats (`placeFloat`, `FloatContext`)
A floated element is taken out of the flow but stays in the inline run around it: `layoutElement`/`collectInline` add it as an `InlineItem` carrying a `FloatItem` (the element, its style, and the ancestors, link and form it's laid out with). `layoutInlineRun` places it when the run reaches it: at the current `y` if the line so far is empty, else just below that line (a simplification - CSS puts it on the current line if it fits). A run with only whitespace and floats still places the floats, but takes no space itself.

**Placing** (`placeFloat`):
- **Laying it out.** The float is laid out on its own (`layoutItemDetached`). A block with no `width` is shrink-to-fit (`shrinkToFitWidth`); an image, control or table is laid out in the whole containing block and sized by how far its boxes reach.
- **Height.** Its top is never above where the run reached it, an earlier float's top (`FloatContext::lastTop`), or its own `clear`.
- **Moving down.** If its margin box doesn't fit beside the floats already there (`spaceBeside`), it moves down to the bottom of the highest float in the way.
- **Recording.** Its boxes get paint key 1 (above in-flow backgrounds, below positioned elements - see `LayoutBox::paintKey`), and its margin box is added to the current `FloatContext`.

**Float contexts** (CSS block formatting contexts): floats are kept in the same coordinates as the boxes, so a float affects lines in nested blocks too, not just its siblings.
- **Where a new context starts.** At the root, in every detached layout (flex/grid items, table cells, absolute elements, floats - each laid out at local `(0, 0)` and moved later), and in `layoutBlockChild` for a block with `overflow` other than `visible`, `display: flow-root`, a table, a flex or grid container, or a float.
- **Containing floats.** Such a block contains its floats: its auto height reaches their bottom.
- **Beside floats, not under them.** It doesn't overlap the floats outside it either: it's narrowed to the space beside them (`besideFloats`). A table whose minimum width doesn't fit there moves down past them instead.

**Lines** (`openLine` in `layoutInlineRun`): when a line starts, its left edge and width are those `spaceBeside` leaves at its `y`, for roughly its first item's height; if that first word or image doesn't fit, the line moves down past the floats in the way. `text-align` aligns within the shortened line. A word wider than the whole block still breaks by characters, below any floats.

**Clearing:** `clear` on a block moves its border edge below the floats on that side (`clearance`); `<br clear=all>` does the same for the next line.

**Not supported:** a float placed on the line that's already partly filled, margin collapsing around floats (there's none anywhere here), `::after` clearfixes (pseudo-elements aren't supported - `overflow: hidden` or `display: flow-root` contain floats instead), `shape-outside`, and floats on flex/grid items or table cells (ignored, as in CSS for the first two). Line heights grow after a line is opened, so a tall image added later in a line can overlap a float below it.

### Tables (`buildTable`, `layoutTable`)
`display: table` (the default for `<table>`) is browsers' automatic table layout, simplified. It runs in two steps, because an auto-width table's own width depends on its contents.

1. **`buildTable`**, called by `layoutBlockChild` before the table's width is settled, reads the table into a `TableModel`:
   - **Structure.** Captions; `<col>`/`<colgroup>` widths (with `span`); row groups, with the first sections being `<thead>`s and the last `<tfoot>`s whatever their place in the source; rows; and cells. Cells with no `<tr>`, or anything else that isn't a row directly in a table or group, go into an anonymous row. A `<form>` between table parts (`<table><form><tr>…`) is looked through, its controls still belonging to it; hidden inputs and text between table parts are dropped.
   - **Grid placement.** Each cell takes the next column in its row that a `rowspan` from above hasn't filled. `colspan` is clamped to 1000. `rowspan` stops at the end of its section (a row group, or a run of rows directly in the table), and `rowspan=0` reaches that end.
   - **Measuring.** Each cell is laid out twice in *measuring mode* (`measuring_`: text isn't aligned or broken mid-word, and a line can be as narrow as one word). Once at width 0 gives its min-content width, from how far right anything reaches. Once at the table's available width gives its max-content width, from how far right text, images, controls and nested tables reach (other blocks just fill what they're given). Absolute elements found while measuring go to a throwaway containing block.
   - **Columns.** A column's min/max is the largest of its single-column cells'; then each spanning cell, narrowest span first, shares any shortfall among its columns in proportion to their max widths (`shareOut`). An explicit `width` on a cell or `<col>` replaces the max-content width and marks the column fixed. A `%` width has already been resolved against the available width by then, so it's treated the same way.
   - **Gaps.** `border-spacing` goes between cells and around them. `border-collapse: collapse` makes the gaps negative instead: neighbouring cells overlap by the widest cell border, and the outer cells overlap the table's border.
2. **`layoutBlockChild`** sizes the table: its `width` if set (a table's width always includes its border and padding), else the sum of the column maxima if that fits, else what's available - never less than the sum of the minima. A `<center>` or `align=center` parent centres it.
3. **`layoutTable`** places everything:
   - **Column widths** (`columnWidths`): every column gets its min; up to the sum of the maxima, each moves the same fraction of the way from min to max; beyond that, the extra goes to non-fixed columns in proportion to their max.
   - **Cells** are laid out (`layoutCell` → `layoutItemDetached`) at the width of the columns they span, with their margins zeroed, their width cleared, their `height` turned into a minimum, and their own ancestors and form restored.
   - **Rows** are as tall as their tallest cell, or the row's own `height` if larger. A spanning cell adds any shortfall to the last row it spans.
   - **Painting.** Row-group and row backgrounds are painted across all columns, under the cells. Each cell's own background box (the first box `layoutBlockChild` made for it) is stretched to the full height of its rows, and its content is shifted by `vertical-align`.

Captions are laid out above the table's border box, at its width.

**Quirks mode** (`LayoutRoot::quirks`, from the doctype - §7): a table resets `text-align`, bold/italic and font size instead of inheriting them. Pages without a doctype that wrap everything in `<center>` (Hacker News, say) rely on this.

**Cost.** Measuring is two extra layouts per cell. While measuring, `layoutCell` memoizes results per (element, width) for the rest of the pass (`cellCache_`), so nested tables don't multiply it level by level. Real layouts are never cached, so their side effects - absolute descendants registering with their containing block, sticky elements - always happen. On Wikipedia's Tiger article (~50 tables, ~220 cells, Release build) measuring adds ~120 ms to a ~330 ms layout.

**Not supported:** `table-layout: fixed` (always automatic), percentage columns as such, `caption-side: bottom`, `empty-cells`, row/column borders, and collapsed borders of different widths or colours (cells simply overlap). `visibility: collapse` on rows isn't special. Table parts outside a table get no anonymous table around them (consecutive `display: table-cell` divs stack like blocks), and `inline-table` is laid out as a block.

### Algorithm (`layoutElement`)
For each child:
- **Text node** → split into words and buffered in `pendingInline`.
- **Skipped entirely:** `head script style title meta link base`, and anything whose computed `display` is `none`.
- **`<br>`** → a forced-break item in the inline buffer.
- **`<input>`, `<button>`, `<select>`** → flush inline, then `layoutControl`.
- **`<img>`** → flush inline, then `layoutImage`.
- **Floated element** (`float: left`/`right`, any tag) → a float item in the inline buffer, placed by `layoutInlineRun` (Floats, above).
- **Inline element** (`a span b strong i em u small code sub sup mark label abbr cite q`, or `display:inline`) → `collectInline` flattens its text and nested inline children into the same run. `<a href>` sets `currentHref` for its words.
- **Block element** (or `display: grid`/`flex`/`table`) → flush inline, then `layoutBlockChild`: resolve the box model above, add `margin-top`, reserve a background/border box if it has either (height patched afterwards), add `border` + `padding-top`, recurse into `layoutElement` (or `layoutGrid`/`layoutFlex`/`layoutTable`, above, for `display: grid`/`flex`/`table`), add `padding-bottom` + `border` and `margin-bottom`. `<form>` sets `currentForm` for the subtree either way.

### Inline runs (`layoutInlineRun`)
Greedy word wrapping. Each word gets its own `LayoutBox` (so words on one line can differ in size, link, or owner). Text sits in a band as tall as the tallest `line-height` on the line (`lineBand`; font size + 8 for `normal`), each word centred in it. Words get a space before them only where the HTML had whitespace (`InlineItem::spaceBefore`, and `isSpace` markers for whitespace that ends a text node), so `<a>link</a>.` keeps its period attached. `text-align` then shifts each finished line by what's left of the container's real width (lines wrap at no less than 40 px, but a narrower container - a table column - still aligns to its own edge). A word wider than a line is split by characters. No gap follows a run - as in browsers, paragraphs are spaced by `<p>`'s margins (see Default spacing).

**A block inside inline content** (`<a><div>…</div></a>`, a `<div>` in a `<span>`) and a form control inside it (`<label>Name <input></label>`) become `InlineItem::blockItem`s (`collectInline`): the run ends its line there, lays the block or control out on lines of its own (`layoutInlineBlock`, in the context it was met in - ancestors, link, form), and carries on below it - CSS's anonymous block boxes around a block in an inline. Before, a block there was flattened into the text (so it never acted as a containing block for its absolute children) and a control vanished.

**Images in a line:** `<img>` and `<svg>` are inline by default (`isInlineTag`), as in CSS, so they join the run around them (`appendImage`, from `layoutElement` or from inside inline elements via `collectInline`). The item carries a ready-sized box (`makeImageBox`, shared with block images); it wraps like a word (never split), with its horizontal margins around it, and is placed in the same coordinates as text (4 px in). An image taller than the text band makes the line taller and puts the text at the line's bottom, roughly where a browser puts a baseline-aligned image; a shorter one is centred on the text band, the usual look of an icon beside a label. An image inside a link carries its `href`, and `linkAt` treats its whole box as the link. `display: block` on an image lays it out on its own (`layoutImage`).

### Sizing rules
- **Text field:** width from `size` attribute (default 20 chars × 8 + 16), height `max(28, font + 14)`.
- **Button:** text width + 24, height `max(30, font + 16)`. `<input type=submit|button|reset|image>` are buttons.
- **Checkbox:** `max(18, font + 4)` square. **Select:** 160 wide.
- `hidden`, `radio`, `file`, `range`, `color` inputs produce **no box** at all (hidden takes no space; the rest are unsupported).
- **Image:** explicit `width`/`height` attributes win per axis; otherwise natural size if already decoded; otherwise a 200×150 placeholder. Width is clamped to the container.

`ancestorStack` is maintained during the walk so `computeStyle` can evaluate descendant selectors.

Text is measured through a `std::function` set by `Engine` that calls `Renderer::measureText`, so wrapping uses real Segoe UI metrics. Each measurement is a GDI call, so `textWidth` caches widths by font (face, size, bold, italic) and text, across layouts too (`textCache_`, capped at 200,000 entries). The widths depend on the page zoom, so `Engine::doLayout` passes it in as `measureScale` and the cache starts afresh when it changes (or when no renderer is set yet, -1). On the Tiger article this took measuring from ~25 ms to ~3.5 ms per layout.

**Layout cost, Tiger article, Release:** ~180 ms per layout before the ancestor filter, the text cache and `Prop` ids; ~80 ms after. What's left is spread out: rule matching (~20 ms), applying declarations (~12 ms), inline runs (~25 ms, mostly making boxes), and table measuring. `:hover` still relayouts only under 50 ms (§8), so it remains off on that page.

> `<noscript>` is deliberately not in the skip-list: its content is laid out like a normal container, even though scripts now run. The engine doesn't distinguish "JS available" from "JS not available".

## 10. Engine (`Engine.h`, `Engine.cpp`)

`Engine` owns the current page and ties everything together.

**State:** `document`, `layoutRoot`, `scrollY`, `topInset`, `documentHeight`, the per-page JS realm (`jsEngine` + `domState`), and the form/focus state.

### `loadHTML(html, baseUrl)`
1. `parseAndBuild` — clears focus/submission/dropdown state, parses, collects linked stylesheets and starts fetching them in the background (below), points `layoutRoot` at the new body and rules.
2. `beginScripts` — see §11. Also starts external script fetches, and runs whatever prefix of them is already ready (inline scripts, synchronously - see below).
3. `doLayout` — first paint, before any external resource is necessarily ready. `Engine::pollResources` (called every frame from `render`) fills the rest in progressively; see §5's "Progressive resource loading".

### Linked stylesheets (`parseAndBuild`)
`<style>` blocks are the only CSS source `HTMLParser` itself understands (§7). Right after parsing, `parseAndBuild` walks the parsed tree (`document->root` if set, else `document->body`) for every `<link rel="stylesheet" href="...">`, in document order, resolves each href with `resolveUrl` (so a relative href only works against an `http(s)` page - same rule `<img src>` and `<script src>` already follow), and hands the resolved URLs to `styleLoader_` (a `ResourceLoader`, §5), which fetches them all concurrently in the background. `pollResources` parses each one's response text with `CSS::parseStylesheet` - straight text parsing, so nothing needs to know or care that this particular response happens to be CSS rather than HTML - and appends the resulting rules to `document->styles`, after whatever `<style>` blocks already produced, as each fetch completes.

Each stylesheet's own rules come back from `parseStylesheet` numbered from 0 (it has no idea it's one of several sources), so before appending, every rule's `order` is shifted up by a precomputed *band* - `kStyleOrderBand * (this stylesheet's 1-based position among the page's `<link>`s)` - computed from the link's position in the *document*, not from `document->styles.size()` at append time. That distinction matters now that stylesheets apply in whatever order their background fetches happen to complete, rather than always in document order: a size-based offset would only have been correct if appending still happened serially in document order.

**Simplification (unchanged from before backgrounding):** every linked stylesheet's rules end up ordered after every inline `<style>` block's rules, regardless of their true relative position in the markup. Correct for the overwhelmingly common case (stylesheet links in `<head>`, any inline overrides after them); wrong only if a page deliberately puts an overriding `<style>` block *before* its `<link rel=stylesheet>` and relies on that ordering to win a specificity tie.

### `doLayout()`
Clears any open dropdown, runs `layoutRoot.layout()`, recomputes `documentHeight` (max box bottom), and clamps `scrollY`.

**When a re-layout happens:** page load, window resize, `setRenderer`, `setTopInset`, when a background image finishes loading (`imageGeneration` changed), and when JS mutated the DOM (`domDirty`).

### `render(renderer, time)` — per frame
1. If `renderer.imageGeneration()` changed → re-layout (so image-sized boxes get their real size).
2. `fireDueTimers` (JS `setTimeout`/`setInterval`).
3. If `domDirty` → clear it and re-layout.
4. For each box: convert to screen space (`screenY = b.y − scrollY + topInset`), cull if off-screen, then
   - controls → `drawControl`,
   - otherwise background rect → image (`drawImage`, then `continue`) → text (`LayoutBox::color`, black when empty, and `LayoutBox::bold`; drawn at `+4,+4` inside the box).
5. `drawOpenSelect` overlay.

### Coordinates
There are two spaces. **Page (document) space** is what layout produces (`b.x`, `b.y`), in page pixels. **Window space** is what mouse events use. Every hit test converts through one function, `Engine::toPage`: `pageX = x / zoom`, `pageY = (y − topInset) / zoom + scrollY` (false above the page). Drawing goes the other way through the renderer's page transform (§14): `render()` draws boxes at `b.y − scrollY` and `Renderer::setPageTransform(topInset, zoom)` moves and scales that into place, so drawing code never adds `topInset` itself. Browser UI - the address bar, scrollbar, find bar and console - is drawn in window space, unzoomed.

### Hit-testing
All hit-tests scan `boxes` in **reverse** (last painted = topmost):
- `linkAt` — words with an `href`, tested against the actual text width.
- `controlAt` — form controls, by bounding box.
- `dispatchClick` — any box with an `el`, by bounding box.
- All of them take window coordinates and convert with `toPage` (see Coordinates), so they work at any zoom.

### Scrolling, zoom and find
- **Keyboard scrolling** (nothing focused): arrows a line (40 px), PageUp/PageDown/Space a screenful minus one line of overlap, Home/End the very top/bottom.
- **Scrollbar**: an overlay along the page's right edge (12 px, window space; no layout space reserved), drawn only when the page is taller than the viewport. The thumb's size and position come from `viewHeight/documentHeight` and `scrollY/maxScroll` (`scrollbarGeometry`, min 30 px). Dragging keeps the grab point under the mouse (`beginScrollbarDrag`/`dragScrollbar`, followed each frame by `main.cpp` until mouse-up); a click on the track pages towards it. It darkens on hover, and the page under it gets no hover or cursor.
- **Zoom** (`setZoom`, 0.25–5): layout runs at `width / zoom`, so text reflows, and `viewHeight()` is in page pixels. Text is measured for layout at the size it'll be drawn (`measurePageText`: rasterized at `round(size × zoom)`, divided back) - otherwise zoomed words come out slightly wider than plain scaling and crowd into each other's spaces.
- **Find in page** (`findText`/`findNext`/`clearFind`; the UI is `FindBar`): the visible text boxes are joined with single spaces into one lower-cased stream, with a map back to each character's box, so matches are case-insensitive and can span words and lines (a whitespace run in the query matches one space). Each match is a list of per-box character ranges; `render()` draws them behind the text - yellow, the current one orange. A new search starts at the first match at or below the top of the view (wrapping), and the current match is scrolled a third of the way down if it isn't visible. `doLayout()` re-runs the search against the new boxes, so it survives relayouts - including a new page's - keeping the current index without scrolling. Capped at 1000 matches.
- All of them take window coordinates and convert with `toPage` (see Coordinates), so they work at any zoom.



## 11. JavaScript (`JSEngine.cpp`, `JSBinding.cpp`)

### Per-page realm
Every `loadHTML` creates a **fresh** `JSEngine` (quickjs runtime + context) and resets `DOMBindingState`. Nothing survives navigation. `JSEngine::eval` runs a global script and returns the result string, or `"Error: message\nstack"` on exception. `console.log/warn/error` all print `[console] …` to stdout and the debugger output.

**Teardown order matters.** `Engine::beginScripts` resets `domState` first, then destroys the old `JSEngine`, then creates the new one. `domState` owns `JSValue`s (listeners, timers, node wrappers) that must be freed against a runtime that still exists; quickjs asserts (Debug) or corrupts memory (Release) if a runtime is destroyed while any value is alive. Keep that order if you touch it. The same rule is why `domState` is declared after `jsEngine` in `Engine.h` (members destruct in reverse).

### Promises and microtasks
quickjs never runs promise jobs on its own; the host must pump them. `runPendingJobs(ctx)` (`JSEngine.cpp`) drains the job queue, like a browser's microtask checkpoint. It runs after every script's `eval`, after each event listener the engine calls, and after each timer callback, so `Promise.then` and `async`/`await` continuations behave in the expected order (`sync code → promise callbacks → timers`). A job that throws is printed as `[promise] Error: …` and the rest still run. **Any new place that calls into JS (`JS_Call`, `JS_Eval`) should call `runPendingJobs` afterwards.** A promise still rejected and unhandled once the queue is empty is reported as `Uncaught (in promise) …`. `queueMicrotask(fn)` (`kBootstrapJS`) queues `fn` as a promise job; an exception it throws is reported as `microtask` rather than becoming a rejection.

### Script execution (`Engine::beginScripts` / `advanceScripts`)
- Runs **after the entire DOM is built**, in document order across the whole tree - `<head>` scripts first, then `<body>`'s (`collectScripts` starts at `document->root`). So no `document.write`, and scripts can see the whole page, even one in `<head>`.
- `beginScripts` handles inline scripts and `src=` scripts, but doesn't fetch or run anything itself beyond building `scriptTasks_` (one per `<script>`, either the inline code or an index into `scriptLoader_`) and starting every external script's fetch concurrently (`ResourceLoader`, §5). `advanceScripts` does the actual running: it walks `scriptTasks_` from a cursor, executing each task the moment it's ready (inline: immediately; external: once its fetch completes) and **stopping at the first one that isn't ready yet**, even if a later task's fetch already finished - preserving document order despite concurrent, out-of-order fetch completion. `beginScripts` calls it once synchronously right after starting the fetches (so a page with only inline scripts runs them all immediately, same as before backgrounding existed), and `pollResources` calls it again every frame to pick up whatever's newly ready.
- Only "classic" scripts run: no `type`, or `text/javascript`, `application/javascript`, `application/ecmascript`. `type="module"`, JSON-LD, templates etc. are skipped.
- An exception is printed as `[script] Error: …` and execution continues with the next script.

### Script execution watchdog
JS runs synchronously on the UI thread (there's no worker/off-thread execution model, and quickjs contexts aren't meant to be shared across threads), so a script stuck in an infinite loop used to freeze the whole window indefinitely - the same freeze class as the pre-fix `fetchPage()`/`LayoutRoot::layout()` issues above, just triggered by page JS instead of networking or CSS matching.

Fixed with quickjs-ng's `JS_SetInterruptHandler` (`JSEngine.cpp`): installed once per `JSEngine`/`JSRuntime`, it's polled from inside the bytecode interpreter every ~10000 ops. `ArmScriptWatchdog(ctx)` resets a `steady_clock` deadline (`kScriptTimeout` = 2000ms, `JSEngine.h`) to `now() + kScriptTimeout`, and is called immediately before **every** real entry point into JS - `JSEngine::eval` (script tags), `runPendingJobs` (promise jobs, per job drained), and `JSBinding.cpp`'s `fireDueTimers` and event dispatch (per timer/listener call; not for `dispatchEvent()` called by a script that's already running, which stays under that script's deadline) - so the deadline always measures "how long has this one script/callback run," not wall clock since page load. Exceeding it throws quickjs's own uncatchable `InternalError: interrupted` (`JS_ThrowInterrupted`/`JS_SetUncatchableError` in quickjs.c) - uncatchable specifically so a script's own `try { while(true){} } catch(e){}` can't defeat it. The four call sites already had a call-then-check-exception pattern (§ above, and the Events/timers section below), so no new control flow was needed beyond arming the deadline and, for timers, distinguishing a watchdog kill from an ordinary throw (`JS_IsUncatchableError`) to decide whether to keep the timer around.

Verified against three cases (a scratchpad test page + `CloseMainWindow`/stdout capture, since `wprintf` is fully-buffered once redirected and only flushes on normal process exit): a `<script>while(true){}</script>` page recovers and finishes rendering the rest of the DOM instead of hanging (confirmed both via `Process.Responding` staying `True` throughout and via `[script] Error: InternalError: interrupted` in the log); a `setInterval(() => { while(true){} }, 0)` is killed once and then *not* rescheduled (CPU time measured over the following 6s: +0.84s, not the ~6s repeated-kill churn it'd be if the timer kept re-firing); and a genuine `throw new Error(...)` still surfaces as `Error: <message>` unchanged, confirming no regression to the normal error path.

### Developer console (`DevConsole.h`/`.cpp`)
Everything a page's JS says or throws goes into `consoleLog()` - one process-wide `ConsoleLog`, UI-thread only (all JS runs there), cleared at the start of every page's scripts (`beginScripts`), capped at 1000 entries. Each `LogEntry` has a level (log / warn / error, plus `Input`/`Result` for the console's own input line) and a source; `add()` also mirrors it to stdout and the debugger's Output window as `[source] text`.

**What gets captured** - including errors that used to vanish silently:

| Source | From |
|---|---|
| `console` | `console.log/info/debug/warn/error` - arguments formatted by `__wtInspect` (strings as-is; objects/arrays expanded two levels; elements as `<p#id.class>`; errors with their stack) |
| `script` | An uncaught error in a `<script>`. Scripts are evaluated with a filename - the script's URL, or `<inline script N>` - so the stack names the script and line |
| `click listener` (`<type> listener`), `inline handler`, `timer` | An exception in an event listener or a `setTimeout`/`setInterval` callback (previously swallowed), or an inline `on*="..."` attribute that doesn't compile |
| `promise` | A promise rejected with no handler by the end of a microtask checkpoint: `JS_SetHostPromiseRejectionTracker` records each rejection and forgets it if a handler is attached later; `runPendingJobs` reports what's left as `Uncaught (in promise) ...`, as browsers do. This is how a failed `fetch()` without `.catch` shows up |
| `network` | An external script or stylesheet that failed to download (previously skipped silently) |
| `input` | Errors from the console's own input line |

A watchdog kill is reported as "Script stopped: it ran longer than 2000 ms...". `describeException` (JSEngine.cpp) formats any thrown value, using the bootstrap's `__wtInspect` when it exists.

**The panel** (`DevConsolePanel`, F12 or the red error badge in the address bar): docked at the bottom, about 40% of the window height. `Engine::setBottomInset` shrinks the page's viewport to fit above it (scroll is re-clamped, no relayout). It shows entries newest-at-bottom with a per-level icon and color, `[source]` tags (omitted for plain console output), long lines and stacks wrapped to the panel width (re-wrapped only when the log or width changes), a header with error/warning counts plus Clear and Close, and mouse-wheel scrolling that follows new output while at the bottom.

**The input line** evaluates JS against the page (`Engine::consoleEval` → `evaluateInConsole`, as a global script named `<console>`), echoing the line and its value formatted like a REPL (strings quoted), then running promise jobs and marking the DOM dirty so any change it made is laid out. Up/Down browse the input history; Ctrl+V/C/A work; Esc returns focus to the page. The address bar, a page field and the console input are mutually exclusive keyboard targets.

**The error badge**: the address bar's right end shows a red "✖ N" whenever the log holds errors (`AddressBar::setErrorCount`, set each frame); clicking it opens the console.

### What JS can see
Everything is installed by `installDOMBindings`.

**Globals:** `document` (a wrapper around `<body>`), `window` (an alias of the global object), `self`/`parent`/`top` (all also aliases of the global object - WTEngine has no `<iframe>`/frame support, so every page legitimately *is* its own top-level, un-framed window, exactly like `self === parent === top === window` for a real un-framed page), `location`, `console`, `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval`, `requestAnimationFrame`/`cancelAnimationFrame`, `queueMicrotask`, `postMessage`, `MessageChannel`/`MessagePort`, `performance` (`now()`, `timeOrigin`), `fetch`, `Headers`, `XMLHttpRequest` (with `XMLHttpRequestUpload` and `ProgressEvent`), `EventTarget`, `addEventListener`/`removeEventListener`/`dispatchEvent` (on `window`), `Event`/`UIEvent`/`MouseEvent`/`KeyboardEvent`/`InputEvent`/`FocusEvent`/`CustomEvent`/`SubmitEvent`/`MessageEvent`, `DOMException`, `localStorage`, `sessionStorage`.

Some of the API is written in JavaScript rather than C++: `kBootstrapJS` in `JSBinding.cpp`, evaluated at the end of `installDOMBindings`, defines `fetch()`'s option handling, `Headers`, the `Response` object, `XMLHttpRequest`, `EventTarget`, `DOMException`, the event classes, the `Storage` objects, and `element.style`. It builds on the native bindings (`__wtFetch`, `__wtStorage*`, `getAttribute`/`setAttribute`) rather than adding new C++ for things that are simpler in JS.

### `localStorage` / `sessionStorage` (`WebStorage.h`/`.cpp`)
Both are a `Proxy` over a `Storage` object, so `getItem`/`setItem`/`removeItem`/`clear`/`key`/`length` work, and so do `localStorage.foo`, `localStorage['foo'] = 'x'`, `delete localStorage.foo`, `'foo' in localStorage`, `Object.keys(localStorage)` and `JSON.stringify(localStorage)`. Values are converted to strings.
- **Partitioned by origin** (`storageOrigin`): `scheme://host[:port]` for http(s) pages (default ports dropped), one shared `file://` origin for every page loaded from disk, and a memory-only area for anything else (the built-in start page).
- **`localStorage`** is loaded from `%LOCALAPPDATA%\WTEngine\Local Storage\<origin>.txt` the first time an origin uses it, and written back after every change (to a temporary file, then swapped in). One item per line, `key<TAB>value`, with `\\`, `\t`, `\n`, `\r` escaped.
- **`sessionStorage`** is memory-only and lasts for the whole run: there is only one tab, so "the session" is the program's lifetime.
- **Quota:** 5,000,000 UTF-16 code units of keys plus values per origin, as browsers allow roughly 5 MB; going over throws a `QuotaExceededError` `DOMException` and changes nothing.
- No `storage` event (there's only one tab to tell).

### `fetch()`
`fetch(url, { method, headers, body })` returns a Promise, as in a browser:
- **Request:** any method; `headers` as a plain object, an array of pairs, or a `Headers`; `body` as a string (anything else is `String()`-ed - no `FormData`/`Blob`/`URLSearchParams`). The URL is resolved like a link on an http(s) page. On a page loaded from disk, a relative URL is resolved against the page file's folder, so a local test page can fetch a sibling file - a real browser would block that.
- **Plumbing:** `__wtFetch` hands the request to `fetchHttpAsync` (§13), which runs it on Fetcher's single network thread with async I/O - no thread per request. Its completion callback (on that network thread) only stores the result into a `FetchSlot` (a mutex-guarded result - it never touches a `JSValue`, since quickjs isn't thread-safe). The promise's resolve/reject functions wait in `FetchStorage`. `Engine::render` calls `pollFetches` every frame, right after `fireDueTimers`; it settles every finished request and runs the resulting promise jobs. A navigation resets `DOMBindingState`, freeing the pending functions; a request still in flight completes into a slot nothing reads, the same abandon-in-place pattern as `PageLoader`.
- **Response:** `ok`, `status`, `statusText`, `url`, `headers` (every response header but `Set-Cookie`, names lowercased), `bodyUsed`, `text()`, `json()`, `clone()`. The body is decoded as UTF-8; there's no `arrayBuffer()`/`blob()`. An HTTP error status still *resolves* (with `ok: false`); only a network failure rejects, with a real `TypeError`.
- **Not supported:** `AbortController`, streaming, CORS (every request is allowed), cookies. Requests share the engine's connection pool and 6-per-host limit (§13), so many `fetch()`es to one host run in waves of 6, as in a browser.

### `XMLHttpRequest`
Written in JS in `kBootstrapJS` on top of the same `__wtFetch` native as `fetch()`, so it shares its plumbing, connection pool and URL resolution. It extends `EventTarget` (a JS class, also used by `MessagePort`): handlers can be set as `onload`/`onreadystatechange`/... or with `addEventListener` (with `{ once }`); the `on...` handler runs first.
- **Supported:** `open(method, url)`, `setRequestHeader` (forbidden headers like `Host`, `Cookie`, `Proxy-*`, `Sec-*` are silently dropped, as browsers do), `send(body)` (a string body on a non-GET/HEAD defaults to `text/plain;charset=UTF-8`), `abort()`, `timeout` (fires `timeout`), `readyState` with `readystatechange` through 1-4, `status`, `statusText`, `responseURL`, `getResponseHeader`, `getAllResponseHeaders`, `responseText`, `response` for `responseType` `""`/`"text"`/`"json"`. Events: `loadstart`, `progress` (one, with the whole body), `load`/`error`/`abort`/`timeout`, then `loadend`, as `ProgressEvent`s. An HTTP error status is a `load`, a network failure an `error`, as in browsers.
- **Not supported:** synchronous requests (`open(..., false)` throws `InvalidAccessError` - nothing in the engine may block on the network), `responseXML` (always `null`), `arraybuffer`/`blob`/`document` response types (`response` is `null`), upload progress events (`upload` exists but never fires), `withCredentials`/cookies, `overrideMimeType` (accepted, ignored). The body arrives whole, so readyState 3 and 4 come back to back.

### `location` and JS-driven navigation
`window.location` (and the bare global `location`, since `window` aliases the global object) is a plain object, rebuilt fresh on every read - none of its members use their own opaque state, they all go through the current page's `DOMBindingState` (`bindingState(ctx)`), so nothing needs to survive between one read of `location` and the next:

| Member | Behaviour |
|---|---|
| `.href` (get/set) | Reads the current page's URL; setting it queues a navigation |
| `.replace(url)` | Queues a navigation that **overwrites** the current history entry (no Back-button stop) |
| `.assign(url)` | Queues a normal navigation (a new Back-button stop), same as `.href =` |
| `.reload()` | Re-navigates to the current URL, `.replace()`-style |
| `.toString()` | Same as reading `.href` |
| `window.location = "..."` / bare `location = "..."` | Same as `.href =` |

A relative URL is resolved against the page's own URL with `resolveUrl` - same rule `<img src>`/`<script src>` follow, so it only works on an `http(s)` page - and an unresolvable one (a fragment, `javascript:`, ...) is silently dropped, same as a plain `<a href>` click in that situation.

**Plumbing:** setting `location` doesn't navigate immediately - it sets `DOMBindingState::navigationPending`/`navigationUrl`/`navigationReplace` (`queueNavigation` in `JSBinding.cpp`), the same "set a flag, let the main loop act on it next frame" pattern already used for a queued form submission. `Engine::takeNavigation` (polled once per frame in `main.cpp`, alongside `takeSubmission`) retrieves and clears it; `main.cpp`'s `navigate()` takes a `replace` parameter that selects `PageHistory::replaceCurrent` (overwrite in place) over the normal `visit` (push) - this is what makes `location.replace()` behave like the real thing instead of just being `.assign()` under a different name.

**Why this exists:** a page whose script does `location.href = url` (or the equivalent `.replace()`/`.assign()`) to navigate - extremely common for redirect trampolines, auth callbacks, and click-through/tracking links - would previously throw (`location` didn't exist at all) and go nowhere. DuckDuckGo's own search-result links are exactly this: clicking one lands on `duckduckgo.com/l/?uddg=<target>`, a near-blank page whose entire content is `window.parent.location.replace(target)` (`window.parent` needed fixing too, for the same reason - a real page's own `.parent` is itself, not `undefined`).

**One `Node` class** covers both elements and text nodes:

| Member | Notes |
|---|---|
| `textContent` (get/set) | |
| `innerHTML` (set only) | Parses the fragment with `HTMLParser` |
| `tagName`, `id`, `className` | |
| `title` (get/set) | On `document`: the page's `<title>` text; setting it rewrites (or creates, in `<head>`) the `<title>` element, and the window title follows next frame. On any other element: its `title=""` attribute |
| `value` (get/set) | `<input>`/`<button>`: the `value` attribute (`"on"` for a checkbox without one). `<textarea>`: its text. `<select>`: the selected `<option>`'s value (the first option if none is marked); setting it selects the first matching option. `<option>`: `value`, else its text. If the focused text field's value is changed, `Engine::render` reloads its editor from it |
| `checked` (get/set) | Presence of the `checked` attribute - the same state EngineForms toggles on a click |
| `style` | A `Proxy` over the `style=""` attribute: `el.style.backgroundColor = 'red'` (camelCase → kebab-case), `cssText`, `getPropertyValue`, `setProperty`, `removeProperty`, `length`. Setting `''`/`null` removes a property. Each write goes through `setAttribute`, so it triggers the usual relayout. Only inline declarations are visible - no computed style |
| `getAttribute`, `setAttribute`, `hasAttribute`, `removeAttribute` | |
| `createElement`, `createTextNode` | Available on any node, including `document` |
| `appendChild`, `insertBefore`, `replaceChild` | Inserting a node that's already somewhere - in the page or in another detached subtree - **moves** it, as in browsers. Inserting a node into itself or its own descendant throws a `HierarchyRequestError` `DOMException`; a reference/old child that isn't a child throws `NotFoundError` |
| `removeChild`, `remove()` | Detach (not destroy) the node; see Memory model |
| `parentNode`, `parentElement` | The parent element, or `null` |
| `getElementById`, `getElementsByTagName` | Search the subtree of the node they're called on |
| `querySelector`, `querySelectorAll` | Same selector grammar as CSS (§8), including combinators, attribute selectors and structural pseudo-classes; results are plain arrays. Combinators only look at ancestors inside the searched subtree, but sibling combinators and structural pseudo-classes see the element's real siblings |
| `addEventListener`, `removeEventListener`, `dispatchEvent` | Any event type; see Events below |
| `onclick`, `oninput`, `onchange`, `onsubmit`, `onkeydown`, ... | `on*` handler properties for the types in `kHandlerTypes` |
| `body`, `head`, `documentElement`, `readyState` | On `document` only (it wraps `<body>`, so `document.body` is `document` itself) |
| `cookie` (get/set) | On `document` only, for an http(s) page: the page's cookies from the same jar requests use, minus `HttpOnly` ones; assigning one stores it as if the server had sent it. A script can't create, overwrite or shadow (same name, any path) an `HttpOnly` cookie (§13) |

### Memory model
- Each node has **one** JS wrapper object for its lifetime (`NodeWrappers`, cached by `wrapNode`), so `e.target === el` holds and expando properties (`el.myData = …`) stick. Wrappers hold a raw `Node*` (no finalizer). The cache also holds a `shared_ptr` to each wrapped node, so a node JS has seen stays alive until the page is left, even after a script frees it from the tree (`innerHTML =`): JS can never touch freed memory through a wrapper.
- Nodes made by `createElement`/`createTextNode`, or removed by `removeChild`/`remove()`, are kept alive in `DOMBindingState::detachedNodes` until the page is left. Inserting moves the `shared_ptr` that owns the node - its parent's child slot or its `detachedNodes` entry (`takeNode`) - so the `Node` object itself never changes and every raw pointer to it (layout boxes, focus, listeners) stays valid.
- Listeners, timers and wrappers hold `JSValue`s in `ListenerStorage` / `TimerStorage` / `NodeWrappers` (defined only in `JSBinding.cpp` so `quickjs.h` stays out of `Engine.h`). They are freed when the state is reset.
- **Elements the engine holds** (`focusedEl`, `openSelect`, the hovered element, the element a just-dispatched event targeted) may be freed by any listener. Code that keeps an `Element*` across an event dispatch checks `Engine::stillInPage(el)` before using it again, and `render` drops any that are gone before it clears `domDirty`.

### Events and timers
- **Dispatch** (`dispatch` in `JSBinding.cpp`): the path is fixed first - the target, its ancestors via `parent`, then `window` - and its elements are held alive for the whole dispatch. Then the three phases: capture (listeners registered with `capture: true`, from `window` down), target (all of the target's listeners, in registration order), and bubble (non-capture listeners back up to `window`, if the event bubbles). `stopPropagation()` ends it after the current node and `stopImmediatePropagation()` after the current listener; listeners added during dispatch don't run, and removed ones are skipped. An exception is reported to the developer console (`<type> listener`, with its stack) and dispatch continues. A watchdog kill is included; a hung listener isn't auto-disabled, since unlike an interval an event doesn't repeat on its own.
- **Listeners** (`ListenerStorage`, keyed by element, `nullptr` = `window`): `addEventListener(type, fn | {handleEvent}, capture | {capture, once})` (`passive`/`signal` are accepted and ignored; adding the same function twice registers it once), `removeEventListener` by strict equality of the function and capture flag. An `on*` handler - `el.onclick = fn`, or an inline `onclick="..."` attribute compiled on first use as `function (event) { … }` - is a listener flagged `handler`, so it runs in the order it was first set; returning `false` cancels the event. `window.onload = fn` and other `window.on*` are ordinary global properties, read at the `window` step; `<body onload="...">` sets `window.onload`.
- **Event objects** are plain JS (`kBootstrapJS`): `type`, `target`, `currentTarget`, `eventPhase`, `bubbles`, `cancelable`, `defaultPrevented`, `isTrusted` (true for engine-fired events), `timeStamp`, `preventDefault()` (only if cancelable), `stopPropagation()`, `stopImmediatePropagation()`, `composedPath()`, plus the subclass fields: `MouseEvent` (`clientX/Y`, `pageX/Y`, `button`, `relatedTarget`, modifier keys), `KeyboardEvent` (`key`, `code`, `keyCode`/`which`, `location`, `repeat`, modifier keys), `InputEvent` (`inputType`, `data`), `SubmitEvent` (`submitter`), `CustomEvent` (`detail`). A page can construct any of them and send it with `dispatchEvent`.
- **What the engine fires** (`fire*` in `JSBinding.h`):

| Event | Target | When |
|---|---|---|
| `readystatechange`, `DOMContentLoaded` | `document` | Once every `<script>` has run (`Engine::fireReadyEvents`); `document.readyState` goes `loading` → `interactive` |
| `readystatechange`, `load` | `document`, `window` | Once every stylesheet has also arrived (images aren't waited for); `readyState` → `complete` |
| `click` | Element under the mouse | `onClick` (controls) / `dispatchClick`. `preventDefault()` cancels following a link or the control's action |
| `mouseover`, `mouseout`, `mouseenter`, `mouseleave` | Element under the mouse | `updateHover`, when that element changes; `relatedTarget` is the other one |
| `keydown`, `keyup` | Focused field, else `document` | `main.cpp`'s `onKey` → `Engine::onKeyEvent`. `key` is layout-aware (`ToUnicodeEx` on the scancode); `keyCode` is the Windows virtual-key code, as browsers on Windows report |
| `focus`, `focusin`, `blur`, `focusout` | Text field | `focusInput` / `blurInput` |
| `input` | Text field, checkbox, `<select>` | After each edit that changes a field's text (`afterEdit`), a checkbox toggle, or picking a different option |
| `change` | Same | On blur if the user changed the field's value; right after a checkbox toggle or a new option |
| `submit` | `<form>` | `queueSubmit`, before collecting fields. `preventDefault()` cancels the submission (the usual "send it with `fetch` instead" pattern) |

- Every engine-fired event runs synchronously, each listener with its own watchdog deadline and a microtask checkpoint after it. Any DOM change it makes sets `domDirty` as usual.
- Timer callbacks' exceptions are reported to the console as `timer`.
- **The event loop** is `Engine::render`'s once-per-frame sequence, in this order: `fireDueTimers`, `runQueuedTasks`, `pollFetches`, `pollResources` (scripts and stylesheets that have arrived), `runAnimationFrames`, then the relayout if the DOM changed. Input events come from `glfwPollEvents` in `main.cpp`, between frames. Every callback the engine makes gets its own watchdog deadline and a microtask checkpoint after it.
- **Timers:** stored with an absolute due time on the page's own clock (`TimerStorage::seconds()`, seconds since the page's bindings were installed - the clock `performance.now()` reads too), so a timer set while the page loads counts from when it was set. `fireDueTimers` runs once per frame, so timer resolution is one frame (~16 ms). Callbacks are looked up by id right before being called, so a timer can safely clear itself or others. New timers scheduled inside a callback wait until the next frame. Intervals resync to "now" instead of catching up on missed ticks. A callback killed by the script watchdog (above) is removed instead of being rescheduled - otherwise a broken `setInterval(fn, 0)` would get killed and immediately re-armed every frame forever, trading the old "window frozen" failure for a new "permanent 100% CPU" one.
- **`requestAnimationFrame`** (`runAnimationFrames`): callbacks run once per frame, last before the relayout check, so what they change is painted that frame. A frame's batch is the callbacks requested before it started (ones requested meanwhile wait for the next), and they all get the same `performance.now()` timestamp. `cancelAnimationFrame` also cancels one still waiting in the running batch. Exceptions are reported as `animation frame`.
- **Tasks** (`runQueuedTasks`): `postMessage` deliveries go through the native `__wtQueueTask` queue, not `setTimeout`, so a page that yields with them (React's scheduler, via `MessageChannel`) isn't held to one task per frame. Tasks run in order, each followed by its microtasks; tasks they queue run in the same frame until 8 ms of it is spent, and the rest wait for the next. `window.postMessage(data)` fires a `MessageEvent` at `window` (`origin` from the page URL, `null` for a file; `source` is `window`). `MessageChannel` (`kBootstrapJS`) gives two entangled `MessagePort`s: a port delivers once started - by `start()` or by setting `onmessage`, as in browsers - and queues until then. Simplified: data isn't structured-cloned (the receiver gets the same object), ports can't be transferred, and `targetOrigin` isn't checked.
- **Dirty flag:** any mutating binding sets `domDirty`; `Engine::render` re-lays-out at the top of the next frame.

## 12. Form controls (`EngineForms.cpp`)

Part of `Engine`, split out for size.

- **Styling:** CSS `width`/`height` size a control (not a checkbox), border-box style; the label is centred vertically in it. `background`, `border-radius`, and the label's `color` and font (weight, style, face) apply too - but only when set on the control itself (`ComputedStyle::colorSet`/`fontSet`, `inherit` included): like browsers, controls don't pick up the page's text colour and font. A text field's own text keeps the default face, since the caret is placed by measuring it. In a flex row, a control with a CSS width takes its flexed width; one without keeps its natural size.
- **Text fields:** one `TextEditor` instance (`editor`) holds the text/caret/selection for whichever field is focused. After every edit `syncValue()` writes it back to the element's `value` attribute. Password fields are masked with bullets and can't be copied or word-selected. The field scrolls horizontally to keep the caret visible; the caret blinks (0.6 s on, 0.4 s off).
- **Clicking:** double-click (within 0.4 s and 4 px) selects a word.
- **Checkbox:** toggles the `checked` attribute.
- **Select:** click opens a dropdown overlay (`openSelect`, plus a snapshot of the closed box). The overlay is **not** part of `boxes` — it is drawn last and hit-tested first. Any click, inside or outside, closes it; a click on a row chooses that option (sets `selected`). Any re-layout also closes it.
- **Buttons:** if inside a form and a submit type (`<button>` defaults to submit; `<input>` only for `submit`/`image`), the form is queued.
- **Submission** (`queueSubmit` → `takeSubmission` → `submitForm` in `main.cpp`): `collectFields` walks the form's subtree in document order, encoding `name=value` pairs. Included: text-like inputs, checked checkboxes (`on` if no value), the selected option of each `<select>`, and the clicked submit button. Excluded: unnamed controls, `button/reset/file` inputs, unclicked submit buttons. **Method:** `post` → POST body (`application/x-www-form-urlencoded`); anything else → GET, fields appended as the query string. The action is resolved against the current URL; only `http(s)` targets are sent.
- **Click vs. JS:** when a click lands on a control, `onClick` first runs the JS click listeners for that control (bubbling up through its ancestors, so a listener on a wrapping `<div>` or the `<form>` also fires). If none called `event.preventDefault()`, the control's own action follows: a checkbox toggles, a submit button queues its form, a select opens. `preventDefault()` cancels that action, which is how the common `button.addEventListener('click', e => { e.preventDefault(); … })` pattern works. Focusing a text field is not cancelled, matching browsers. Because `onClick` already dispatched the click, `main.cpp` must not call `dispatchClick` again for a control hit. If a listener rewrites the page and removes the control (e.g. `innerHTML =`), the action is skipped. Clicks that land on an open `<select>` dropdown don't reach listeners (the dropdown consumes them).
- **Form events:** focusing a field fires `focus`/`focusin`, leaving it `change` (only if the user edited it to a different value - a script setting `.value` doesn't count), `blur` and `focusout`. Moving focus straight to another field blurs the old one first. Each edit that changes the text fires `input` (`insertText`, `insertFromPaste`, `deleteContentBackward`/`Forward`). A checkbox toggle and picking a different `<select>` option fire `input` then `change`. Submitting (a submit button, or Enter in a field) fires `submit` at the form first; `preventDefault()` cancels it. See §11's event table.

## 13. Fetching (`Fetcher.cpp`)

HTTP(S) I/O goes through **Boost.Beast/Asio + OpenSSL** (from vcpkg, `x64-windows-static-md` triplet - see §2), not WinINet. URL parsing/combining (`InternetCrackUrlW`/`InternetCombineUrlW`) is pure string manipulation with no networking involved, so it's kept as-is from the original WinINet-based implementation rather than writing a new URL parser - `wininet.lib` is still linked for exactly that, and for nothing else.

- **Connecting:** `async_connect` gets a short deadline (3 s, via `beast::tcp_stream::expires_after`) covering its attempts at each of DNS's resolved addresses, instead of waiting out the OS's own ~20-30 s connect timeout on an unreachable one - the practical benefit of RFC 8305 Happy Eyeballs without racing connections in parallel. The DNS lookup (`async_resolve`) has no deadline of its own.
- **TLS:** one shared, thread-safe `ssl::context` (a function-local `static`, the standard safe-to-share-across-threads OpenSSL pattern) per process. It loads the live Windows "ROOT" certificate store (`CertOpenSystemStoreW` → `d2i_X509` → `X509_STORE_add_cert`) into OpenSSL's trust store, since OpenSSL has no native notion of Windows' store. Hostname verification (`ssl::host_name_verification`) is required in addition to chain-of-trust verification - the latter alone would accept any validly-CA-signed certificate for any host. SNI is set via `SSL_set_tlsext_host_name`.
- **Decompression (`decompressBody`, via zlib - from vcpkg, same triplet as Boost/OpenSSL):** every request sends `Accept-Encoding: gzip, deflate`, and a response with a matching `Content-Encoding` is inflated (`inflateInit2` with `windowBits = 15+32`, zlib's own documented trick for auto-detecting either a gzip or a zlib/deflate header, so one code path handles both) before anything else touches the body. This isn't optional best-effort handling: confirmed against a real site, some CDNs (S3/CloudFront serving statically pre-compressed objects, observed here) send `Content-Encoding: gzip` *unconditionally*, regardless of whether the request even included an `Accept-Encoding` header asking for it - so **any** HTTP client talking to a server like that receives compressed bytes whether it wants to or not. Before this existed, `fetchPage`/`fetchBytes` fed those raw compressed bytes straight to the CSS parser, the JS engine, or the image decoder - which quietly parsed little-to-nothing out of what looked like binary garbage, with no error surfaced anywhere (a page that "looks unstyled" or "doesn't run its scripts" for no apparent reason on a real site is a likely symptom of exactly this, prior to this fix). On decompression failure (corrupt/truncated data, or a `Content-Encoding` claimed but not actually used), `decompressBody` returns the original bytes unchanged rather than an empty result. Brotli (`br`) isn't supported - would need a separate library, and this file never advertises `br` support, so a well-behaved server shouldn't send it unasked.
- **Connection pooling (`ConnectionPool`):** a request doesn't always pay for its own TCP+TLS handshake. `NetworkThread::sendOne` first tries an idle kept-alive connection for its `scheme://host:port`; only if none is available does it open a fresh one (`openConnection`). Measured need (see §17's Engine notes): on a real page, most requests land on the same host (its own origin, or a shared CDN), and a handshake costs roughly 200ms - dominated by the TLS handshake alone - that a second request to the same host shouldn't have to pay again.
  - The pool lives on the network thread and is only ever touched from it, so it needs no lock. (The earlier blocking version needed a mutex, plus a private `io_context` per pooled connection, since any of several background threads could reuse it.)
  - A connection is only pooled if the response didn't say `Connection: close` (`responseWantsClose`); requests send `Connection: keep-alive`. Its advertised lifetime comes from the response's `Keep-Alive: timeout=N` header if present (`parseKeepAliveTimeout`), else a conservative 4-second default (`kDefaultPoolTimeout`) - safely under most servers' real defaults (commonly 5-15s). `Keep-Alive: max=N` (the request-count limit) isn't tracked separately; running into it is just another way a pooled connection turns out to be unusable, already covered by the next point.
  - **Stale-connection handling:** a pooled connection can still have been silently closed by the server between uses - unavoidable with pooling, only recoverable from. Reusing one is wrapped in its own `try`/`catch`: if the exchange throws, that exception is swallowed and a fresh connection is opened instead, transparently - only a *fresh* connection's failure is reported to the caller. The exchange deadline is set before reusing a pooled connection too, so one that accepted the request but never responds fails via timeout rather than hanging.
  - Bounded to `kMaxPooledPerHost` (6, matching the per-host connection limit below) idle connections per host - a `give` past that cap just lets the connection close.
- **`WIN32_LEAN_AND_MEAN`** must be defined before `<windows.h>` in this file - otherwise `windows.h` pulls in the legacy `winsock.h`, which conflicts with Asio's `winsock2.h` (`error C1189: WinSock.h has already been included`).

| Function | Purpose |
|---|---|
| `fetchPageAsync(url, postBody?, options, onDone)` | Text, for pages (`FetchPriority::Page`, `Accept: text/html`) and scripts/stylesheets (`Blocking`, `Accept: */*`). `GET`, or `POST` with a form body; anything non-http is read as a local file path. Status ≥ 400 → error. Follows up to 10 redirects (`followRedirect`: as GET after a 301/302/303; a 307/308 keeps the method and body). Decodes UTF-8 (BOM stripped). Reports the final URL after redirects |
| `fetchHttpAsync(HttpRequest, onDone)` | For JS `fetch()` (§11), at `FetchPriority::Fetch`. Any method, extra headers (overriding the defaults), a body. Delivers status and reason phrase, raw body bytes (decompressed), `Content-Type`, every response header but `Set-Cookie` (names lowercased) and the final URL. An HTTP error status is still a completed response (`ok` = a response arrived). A non-http URL is read as a local file (status 200) |
| `fetchBytesAsync(url, options, onDone)` | Raw bytes, for images (`FetchPriority::Image`): http(s), local file, or `data:…;base64,…` URI |
| `resolveUrl(base, href)` | Makes an absolute URL via `InternetCombineUrl`. Returns `""` for fragments (`#…`) and non-http schemes (`javascript:`, `mailto:`, …). **From a local page** (a Windows path), resolves against the page file's folder (`resolveLocal`): relative (`sub/a.html`, `../img/x.png`), root-relative (`/x` → the drive or share root), absolute paths and `file:` URLs; `%20`-style escapes are decoded (as UTF-8), `..` never climbs above the root, and a `?query`/`#fragment` is dropped (a file has nowhere to send them). So links, images, stylesheets, scripts and `fetch()` all work on a multi-file site opened from disk. A web page can't link to local files |
| `urlEncodeForm(text)` | UTF-8 percent-encoding, space → `+` |
| `withQuery(url, query)` | Replaces the query string and fragment |

**Request headers** (`buildRequest`): what every browser sends, matched to what the request is for (`FetchOptions::dest`: document, script, style, image, or `fetch()`): `Accept`, `Sec-Fetch-Dest`/`-Mode`/`-Site` (and `Sec-Fetch-User` + `Upgrade-Insecure-Requests` for a page), `Accept-Language` from the Windows display language, and a `Referer` (`FetchOptions::referrer`: the page for its scripts, stylesheets and images, which the renderer learns via `Renderer::setPageUrl`; the current page for a link click, form submit or JS navigation; none for a typed URL, Reload or Back/Forward) trimmed by Chrome's default policy - the full URL same-origin, the origin cross-origin, nothing from https to http. Many sites answer requests without these with 403: measured, Best Buy, Medium and Quora load with them and didn't before. The user agent stays `WTEngine/0.1` on purpose - a Chrome User-Agent was tried and was worse: firewalls that check it against the TLS handshake (which isn't Chrome's) served Amazon's JS challenge instead of the page, and it unblocked nothing the headers hadn't. Sites behind Cloudflare/DataDome bot checks (Etsy, Stack Overflow) still refuse - they need a browser's TLS fingerprint and JS. There is no blocking fetch API at all: nothing in the engine can wait on the network, on any thread.

**Cookies** (`CookieJar.h`/`.cpp`): one process-wide jar, shared by the network thread and `document.cookie` (so every method locks). Every `Set-Cookie` header of a response (`RawResponse::setCookies`, collected in `fillRawResponse`) is stored **per hop, before a redirect is followed** - logins and bot checks typically set their cookie on the redirect response itself and expect it back on the very next request - and `buildRequest` sends the matching ones as `Cookie`. Matching follows RFC 6265: host-only vs `Domain=` cookies, path prefixes (the default path is the request path's directory), `Secure` only over https (and only settable from https), `Max-Age` over `Expires` (an `Expires` at or before 1970 deletes, which is how sites delete cookies), longer paths first, and the `__Secure-`/`__Host-` prefix rules. Persistent cookies are saved to `%LOCALAPPDATA%\WTEngine\Cookies.txt` (tab-separated, one per line; written at most every few seconds and on exit, via a temporary file) and loaded at start; session cookies last until the program exits. **Simplifications:** no public-suffix list (a `Domain=` is only refused when it has no dot, like `com`, or doesn't cover the host), and `SameSite` isn't enforced - cookies go with cross-site requests too.

### The network thread
All network I/O - pages (`PageLoader`, §5), scripts and stylesheets (`ResourceLoader`, §5), images (§14) and JS `fetch()` (§11) - runs on **one thread** (`NetworkThread`, created on first use) driving one `net::io_context`:

- **Async I/O:** every request is a C++20 coroutine (`net::co_spawn` + `co_await ... net::use_awaitable`): resolve, connect, TLS handshake, write, read. Each `co_await` suspends that request and frees the thread for the others; on Windows, Asio waits on all their sockets at once through an **I/O completion port (IOCP)** and resumes whichever one has data. The number of requests in flight isn't tied to a number of threads, and a slow server holds up nothing but its own request. This replaced a thread per navigation, two pools of 4 blocking threads for scripts/stylesheets, and 4 for image downloads - with those, a 5th image waited for one of the first 4 to finish.
- **Per-host limit and priorities (`HostLimiter`):** at most 6 requests in flight per `scheme://host:port`, the same limit browsers use for HTTP/1.1 - otherwise a page with 100 images on one CDN would open 100 connections at once (which servers throttle or refuse). Requests over the limit wait, and are admitted by `FetchPriority` (page, then scripts/stylesheets, then `fetch()`, then images), first-come-first-served within a priority - so a script discovered after 50 images still goes next. The slot is held per redirect hop, since a redirect can lead to a different host. A waiter sleeps on a never-expiring `steady_timer` that `release()` cancels to wake it, handing over the slot directly.
- **Cancellation:** `FetchOptions::stillWanted` is checked right after a request gets its slot, before anything is sent; `PageLoader`/`ResourceLoader` pass a `weak_ptr::expired` check, so a superseded navigation or script batch costs nothing if it hadn't started.
- **No locks:** the connection pool and the limiter are only ever touched from the network thread. Callers communicate only through `onDone`, which runs on the network thread and must just hand the result off (a mutex-guarded slot the UI thread polls: `PageLoader::poll`, `ResourceLoader::ready`, `pollFetches`, the image decode queue).
- **Deadlines:** 3 s to connect, 10 s for the TLS handshake, and for the request/response exchange 10 s (30 s for JS `fetch()`, since an API call may legitimately be slow). `PageLoader` separately gives up on a navigation after 8 s.
- **Measured:** thread count stayed flat while loading 30 remote images (18 threads, vs 22 before on a page loading nothing). 20 simultaneous `fetch()`es to one host with a 3-second response ran 6 at a time and all completed (in waves, ~15 s total) with no failures. Wikipedia's Tiger article renders the same as with the old blocking code.
- **Blocking work on the network thread:** gzip decompression, `data:` URI decoding and local-file reads happen there, briefly delaying other requests' progress (never the UI). Image *decoding* does not - that's CPU work for the renderer's decode pool (§14).
- **Shutdown:** `NetworkThread` is a function-local static; at process exit its destructor stops the loop, joins the thread and closes pooled connections, dropping any request still in flight without calling its callback. (Member order matters there - see its comments.)
- **Build:** the coroutine templates overflow MSVC's default per-object section limit, so `Fetcher.cpp` alone is compiled with `/bigobj` (set in both `CMakeLists.txt` and `WTEngine.vcxproj`).

## 14. Rendering (`Renderer.h`, `OpenGLRenderer.cpp`)

`Renderer` is an abstract interface (`drawRect`, `drawText`, `measureText`, `drawImage`, `preloadImage`, `imageGeneration`, `setClip`, `clearClip`); `OpenGLRenderer` is the only implementation. `parseColor`/`tryParseColor` (declared in `Renderer.h`, defined in `Engine.cpp`) turn any CSS color from §8 into a `Color`; `parseColor` falls back to light gray, `tryParseColor` returns false so callers can ignore invalid values. `drawText`/`measureText` take an optional `bold` flag.

- **Projection:** `glOrtho(0, w, h, 0)` — top-left origin, y down. `scrollY` is not passed to the renderer; `Engine` subtracts it before drawing.
- **Page transform** (`setPageTransform(offsetY, scale)` / `resetTransform`, reset by `beginFrame`): a modelview translate+scale for the page area - below the address bar, at the zoom. `drawText` rasterizes at `round(size × scale)` and draws the quad at `1/scale` that size, snapped to whole screen pixels, so zoomed text is sharp; `setClip` applies the transform by hand (scissor rects ignore the modelview matrix). Textures use `GL_CLAMP_TO_EDGE`, or scaled words/images would show a faint line where linear filtering wraps one edge into the other.
- **Rects:** immediate-mode `GL_QUADS`.
- **Rounded shapes** (`drawRoundedRect`, `drawRoundedFrame`, and `drawImage` with radii): triangles around a contour that follows each corner's arc (`roundedContour`; 2-16 segments per corner, by radius). OpenGL's polygon edges are aliased, so every shape also gets a 1px fringe strip whose alpha fades to zero, which reads as a smooth edge; a rounded image uses the same geometry with texture coordinates. A rounded box paints its background inset by the border width with each radius reduced by it, then the border ring on top.
- **Gradients** (`drawGradient`): a linear gradient is baked once into a 256×1 texture of its colours along the gradient line; the box's texture coordinates run along that line (an affine `TexMap`), so every angle and box size shares one texture - a repeating one tiles a texture of one period with `GL_REPEAT`. A radial one is baked into a 256×256 texture over the box (its key includes the centre and shape). Textures are cached by everything that shapes them (`gradientCache_`, emptied past 48). **Shadows** (`drawShadow`): the same rounded shape as a box, with a fading edge as wide as the blur - a linear ramp standing in for the Gaussian blur browsers use.
- **Text:** rendered once with GDI (white on black into a DIB, anti-aliased, in the box's font - Segoe UI unless `font-family` chose another installed face; italic gets a little extra width so its last glyph isn't clipped), converted so brightness becomes alpha, and uploaded as an RGBA texture. The colour is applied at draw time via `glColor4f`, so one texture serves any colour. Cached in `textCache` keyed by `text@size` (plus a `b` suffix for bold). Each entry records the frame it was last drawn in; once the cache's textures pass `kTextCacheBudget` (64 MB), `beginFrame` deletes the least recently drawn ones until it's back to ¾ of that. Text drawn in the previous frame is never evicted.
- **`measureText`:** GDI `GetTextExtentPoint32W` with a cached `HFONT` per size, so measuring doesn't create textures.
- **Clipping:** `glScissor`, with y flipped to OpenGL's bottom-left origin. Page content clips through `Renderer::pushClip`/`popClip`, a stack that intersects each new rectangle with the current one - so a control's own clip stays inside an `overflow: hidden` ancestor's. Page content clips through `Renderer::pushClip`/`popClip`, a stack that intersects each new rectangle with the current one - so a control's own clip stays inside an `overflow: hidden` ancestor's.

### Image loading pipeline
```
layout/draw asks for URL ──► getOrCreateImageTexture ──► reserve slot (Loading), fetchBytesAsync
                                                               │
        network thread (§13): download at FetchPriority::Image  →  decodeQueue (mutex, weak_ptr)
                                                               │
        4 decode threads (COM initialised each): WIC decode, or Direct2D for SVG → RGBA
                                                               │  pendingUploads (mutex)
        main thread, start of next beginFrame(): drainPendingImageUploads
            → glTexImage2D (Ready) or Failed;  imageGen++
                                                               │
        Engine::render sees imageGeneration changed → doLayout()
```
Only the main thread makes GL calls. A `Failed` image is not retried. While an image loads, `drawImage` draws nothing and layout uses the 200×150 placeholder (or the `width`/`height` attributes); when it finishes, the page re-lays-out to the real size. Any WIC format works (PNG, JPEG, GIF first frame, BMP, TIFF, ICO, and WebP/HEIC/AVIF/JPEG XL where their Windows extensions are installed). Textures get mipmaps when OpenGL is 1.4+ (`supportsAutoMipmaps`), so images drawn smaller than their pixels stay smooth. There is no `srcset`. (CSS `background-image` is drawn by `Engine::paintBackgroundLayers` - §8.)

### SVG (`Svg.h`/`.cpp`)
- **Files** (`<img src="x.svg">`, or an SVG `data:` URI): the decode thread sniffs the bytes (`looksLikeSvg`) and draws them with Direct2D's SVG renderer (`ID2D1DeviceContext5`, Windows 10 1703+) into a WIC bitmap (`rasterizeSvg`). The intrinsic size comes from the root's `width`/`height`/`viewBox` (`svgIntrinsicSize`); it's rasterized at 2× (4× for icons up to 64px, capped at 4096px a side) so it stays sharp when shown larger or zoomed, and `ImageTexture` keeps the intrinsic size for layout. Pixels are converted from premultiplied BGRA to straight RGBA, and colour is bled into fully transparent pixels so filtering doesn't darken edges.
- **Inline `<svg>`** is laid out like an `<img>` (`layoutImage`): its subtree is serialized back to an SVG document and handed to the same pipeline as a `data:` URI (`svgDataUri`). The serializer restores the camelCase names the HTML parser lowercased (`viewBox`, `linearGradient`, …), adds the SVG namespace, writes `href` as `xlink:href`, turns `style=""` declarations into presentation attributes, replaces `currentColor` with the element's inherited text colour, and rewrites colours to `#rrggbb` (+ `*-opacity`), since Direct2D doesn't read `hsl()` and the like.
- **Sizing** (both `<img>` and `<svg>`): CSS `width`/`height`, then the `width`/`height` attributes, then the natural size; a dimension given alone keeps the natural aspect ratio.
- **Not supported:** SVG `<text>` (Direct2D's renderer doesn't draw it), filters, masks, animation, and the page's CSS styling the inside of an inline `<svg>` (`.icon path { fill: red }`). A change of `currentColor` (e.g. on `:hover`) makes a new image, which is blank for the frame or two it takes to rasterize.

## 15. Shared helpers

- **`TextEditor`** — text + caret + selection for one line. Used by both the address bar and page inputs, and knows nothing about drawing. Handles typing (with UTF-16 surrogate pairs), paste, Backspace/Delete/←/→/Home/End, click-to-caret, double-click word select, and masking. There is no Shift-selection with the keyboard.
- **`AddressBar`** — wraps a `TextEditor`; adds focus state, Back/Forward/Reload buttons, drawing, and URL normalisation.
- **`PageHistory`** — two stacks around `current_`. `visit()` clears forward history; back stack is capped at 50 entries. Stores each page's HTML and scroll offset.

## 16. Life of a click on a link

1. GLFW → `onMouseButton` (`main.cpp`); position converted to framebuffer pixels.
2. Not in the bar → `Engine::onClick`: not a control → `false`. (On a control, `onClick` would run the listeners and the control's action itself, and the click would end here; see §12.)
3. `Engine::linkAt` returns the `href`.
4. `Engine::dispatchClick` finds the topmost box → element, bubbles JS listeners; returns whether `preventDefault` was called.
5. No prevent → `resolveUrl(currentUrl, href)` → `app->pendingUrl`.
6. Next frame: `navigate` → `PageLoader::start` → (network thread) `fetchPageAsync` → `applyFinishedNavigation` → `visitPage` → `showEntry` → `Engine::loadHTML` → parse → run scripts → layout → render.

## 17. Known limitations and rough edges

**HTML / CSS**
- Sloppy HTML only partly recovers: end tags close open ancestors and table parts/list items end implicitly (§7), but the rest of HTML's tree-building rules (an unclosed `<p>` before a block, misnested formatting tags, content inside a table moved out of it) aren't implemented.
- Tables use automatic layout only, with the simplifications in §9 (Tables) - no `table-layout: fixed`, no real percentage columns, collapsed borders just overlap, and table parts outside a table aren't wrapped in an anonymous one.
- Colours: `color-mix()`, `oklch()` and other newer colour functions, and `currentcolor` as a background, are not understood (an invalid `color` is ignored; an invalid background falls back to light gray). Only regular and bold weights exist, and web fonts (`@font-face`) aren't loaded.
- External stylesheets (`<link rel=stylesheet>`) are fetched and applied, but always cascade after every inline `<style>` block regardless of true document order (§10). `@media` is evaluated for a bare type and/or `min-width`/`max-width` (§8, resize-reactive since layout already fully re-runs on resize); everything else (`@supports`, other features, comma query lists) is not. Selectors cover combinators, attribute selectors and most pseudo-classes, but not pseudo-elements, `:is()`/`:where()`/`:has()`, or a working `:focus`/`:active`/`:visited` (§8); `:hover` is ignored on pages whose layout takes over 50 ms. **Confirmed fixed end-to-end on a real page:** Wikipedia's collapsible "Main menu" panel was hidden by `.vector-dropdown-content{opacity:0;height:0;visibility:hidden;...}`, nested inside a real ~36KB `@media screen{...}` block - fixed by `opacity`/`visibility` support, bare-media-type `@media`, and `height:0` collapse landing together (all §8). Verified by reloading the real page: the dropdown's menu items no longer render inline at the top, replaced by just the real "Main menu"/"Search" labels - the header box is still taller than a real browser's, most likely from other hidden dropdowns in the same header or generic box-model padding differences, not investigated further.
- **Partially confirmed on the same real page:** Wikipedia's responsive sidebar-beside-content layout (`grid-template-areas` on `.mw-page-container-inner`/`.mw-body`) is gated behind `@media screen and (min-width:1120px)`, now evaluated (§8). At a wide window, the page visibly changes - header items spread out differently and the article text now renders in a constrained, centered column instead of full window width, confirming the width-conditioned rules are reaching real elements - but no left-hand navigation sidebar appears beside the content in that view. Not narrowed down further: real candidates are JS-toggled "pinned" state classes (`vector-feature-main-menu-pinned-disabled` and similar - already noted elsewhere as a likely gap given this engine's limited JS/DOM surface) or another still-unevaluated rule overriding the sidebar's own hidden state specifically at wide viewports. Treat as an open, layered investigation, not a confirmed fix.
- Only the properties in §8 are honoured. Styles on `<html>` and `<body>` reach the page through inheritance (colour, fonts, line-height, alignment, custom properties) and `<body>`'s `font-size` applies - but neither's own background is painted, and `<html>`'s `font-size` doesn't change the 14px base. `width`/`border`/`box-sizing` work for plain block and grid/flex-item elements (`width`/`height` also size `<img>`/`<svg>` and form controls - see §12). Floats are simplified (§9, Floats). A `height` attribute on an `<img>` isn't overridden by CSS `height: auto`, so `max-width` won't rescale such an image's height. Positioning (§8) is simplified: no scrolling inside an element (`overflow: auto`/`scroll` clip like `hidden`), overflow clips are rectangles that ignore `border-radius`, `sticky` only sticks to the top, an absolute element met inside a line of text starts from its containing block's corner if it gives no offsets, and stacking contexts are flattened (an explicit `z-index` puts its whole subtree on one level). Border is always solid-colored; `border-style` isn't read, and `border-radius` is always circular (no elliptical corners). `opacity`/`visibility` are supported as a "keep the space, skip the paint" hide mechanism (§8) - not a general implementation of either property (no partial-opacity blending, no `visibility:collapse`'s table-specific behavior).
- Grid supports column and row tracks (fixed, `fr`, content-sized and `minmax()` columns - see §9; `fr` and content-sized rows fall back to auto - no definite container height to distribute against), `gap`, row-major auto-placement, named-area placement (`grid-template-areas`/`grid-area`, and both forms of the `grid-template` shorthand), and line-based placement (`grid-column`/`grid-row` with line numbers, negative lines and `span N`, either axis alone - see §9). No `grid-auto-flow: dense`, named lines, or implicit columns; a named area's cells aren't checked for forming a proper rectangle (its bounding box is used regardless). Not supported: `justify-*`/`align-*` and subgrid. A bare text node directly inside a grid container is dropped rather than becoming an anonymous item.
- Flex supports `flex-direction`, `flex-wrap`/`flex-flow` (row direction), `justify-content`, `align-items`, `flex-grow`/`flex-shrink`/`flex-basis` and the full `flex` shorthand, and `gap` - see §9. Items are content-sized and never shrink below their min-content width, but both are measured by laying the item out (`contentWidth`), an approximation of CSS's intrinsic sizes. Not supported: `align-content`, `align-self`, `order`, reversed directions.
- Radio buttons, file inputs, `<textarea>` (content dropped), and multi-line inputs are not supported. A `<select>` shows only direct `<option>` children (no `<optgroup>`).

**JavaScript**
- `window.onerror`/`unhandledrejection` events don't exist - errors reach the developer console (§11), not page code.
- No computed style (`getComputedStyle`; `element.style` only sees inline declarations), or `innerHTML` getter. (`location`, `fetch()`, `XMLHttpRequest`, events, `localStorage`, `element.style`/`.value`/`.checked` and `document.title` are supported - §11.)
- Events the engine doesn't fire yet, though listeners and `on*` handlers for them can be registered: `mousedown`/`mouseup`/`mousemove`/`dblclick`/`contextmenu`/`wheel`, `keypress`, `scroll`/`resize`, `<img>`/`<script>` `load`/`error`, and `focus`/`blur` on anything but a text field. `load` doesn't wait for images. `el.click()`, `form.submit()` and `form.requestSubmit()` don't exist.
- Mouse events only find elements that have a box: text, controls, images, and blocks with a background or border. Moving or clicking over the empty part of a plain `<div>` reaches its nearest painted ancestor instead (often none).
- `document` is the `<body>` element's wrapper rather than a separate Document node, so `document.body === document`, and a listener on `document` sits between `<body>` and `<html>` in the event path instead of above `<html>`.
- ES modules (`type="module"`) are skipped.

**Engine**
- Page navigation itself no longer blocks the UI thread (`PageLoader`, §5) and gives up after 8s if a fetch is stuck. The networking backend was also replaced (WinINet → Boost.Beast/Asio + OpenSSL, §13) to give connection attempts a short per-address timeout instead of waiting out the OS's own. External script/stylesheet fetches are backgrounded too, and concurrently rather than serially (`ResourceLoader`, §5). All network I/O now runs on one async network thread (§13), and no blocking fetch API exists any more, so nothing can wait on the network.
- **Confirmed real bug, now fixed:** `Fetcher.cpp` had no response decompression at all until this session (§13's `decompressBody`) - found by loading a real site (a corporate site whose stylesheet visibly wasn't applying) and discovering its CDN sends `Content-Encoding: gzip` *unconditionally*, even to a request with no `Accept-Encoding` header. Every response like that was silently fed to the CSS parser/JS engine/image decoder as raw compressed bytes, parsing to near-nothing with no visible error - not a rare edge case, since pre-compressing static assets regardless of client negotiation is a common real-world CDN configuration, not a misconfiguration specific to that one site. Verified end-to-end against the real page: the HTML, both JS bundles, the CSS (866KB decompressed from a 132KB gzip payload), and two SVGs all now decode to valid, readable content.
- **`LayoutRoot::layout()` was the next dominant freeze cause after fetching was fixed - largely fixed itself now.** Diagnostic timing against a real page (Wikipedia's Tiger article, ~1.3MB of HTML, ~20,000 laid-out boxes) showed `doLayout()` taking 5.2s with 135 CSS rules, then 14.4s on the next call with 623 rules applied - both entirely on the UI thread, since layout has to finish before there's anything to paint. Root cause: `CSS::matches` is checked per element against every rule with no selector index (by tag/class/id) - expected to cost roughly elements × rules - but the actual dominant cost turned out to be `classesOf()` (in `CSS.cpp`, used by any class selector) re-splitting the same element's `class=""` attribute string into a fresh `std::vector` from scratch on *every single call*, rather than once per element. Fixed by caching each element's parsed class list for the duration of one layout pass (`CSS::ClassCache`, threaded through `matches`/`compoundMatches` as an optional parameter so `querySelector`'s one-off matching - JSBinding.cpp - is unaffected; `LayoutRoot::classCache`, cleared at the top of every `layout()` since it's only valid within one pass - the DOM doesn't mutate mid-layout). Measured result on the same page: 5.2s → 1.1s and 14.4s → 2.2s, roughly a 5-6x speedup, with no change in visual output (verified against a page exercising every selector form: multi-class AND, tag+class compounds, id, and descendant combinators). A rule index (§8) since cut the brute-force elements × rules matching down to the rules that could match each element: with the wider selector support parsing 1456 rules on that page, layout went from 5.1 s unindexed to 1.7 s (Debug build).
- **Confirmed real bug, now fixed:** JS ran synchronously on the UI thread with no timeout - a `<script>`, timer callback, or click listener stuck in an infinite loop froze the whole window indefinitely, the same freeze class as the fetch/layout issues above but caused by page JS instead. Fixed with a `JS_SetInterruptHandler`-based watchdog (§11's "Script execution watchdog") that kills anything running past 2s with an uncatchable error; verified against a genuine infinite-loop script, a self-rescheduling infinite `setInterval`, a legitimate ~500ms computation (not falsely killed), and a normal thrown error (unaffected).
- Layout is a full re-layout on every change; no incremental layout.
- Dropdown lists are not clipped to the window and don't scroll.

**Project**
- The source file list lives in two places (`WTEngine.vcxproj` and `CMakeLists.txt`); keep them in sync (§2).
- The window is titled "WTEngine" until the first page sets it.
- Build artefacts (`x64/`, `build/`, `.vs/`, `*.obj`) are present in the working tree; check `.gitignore` before committing.

## 18. Where to make common changes

| I want to… | Start here |
|---|---|
| Support a new CSS property | `LayoutRoot::computeStyle` → `applyDecl` (`Layout.cpp`), add a field to `ComputedStyle` |
| Support a new CSS selector form | `parseCompound` / `compoundMatches` / `matches` (`CSS.cpp`) |
| Add a new inline/block default | `isInlineTag` (`Layout.cpp`) |
| Support a new HTML tag or control | `HTMLParser` void/raw lists; `layoutControl` + `drawControl` + `collectFields` |
| Add a JS DOM method or property | Add a function and a list entry in `js_node_proto_funcs` (`JSBinding.cpp`) |
| Add a JS global | `js_global_funcs` (`JSBinding.cpp`) or `JSEngine::JSEngine` (console-style) |
| Fire another kind of event | A `fire*` function in `JSBinding.cpp` (or plain `fireEvent`), called from where `Engine` sees the input; add its `on*` property to `kHandlerTypes` + `js_node_proto_funcs` (§19 Example D) |
| Change drawing | `Renderer` interface, then `OpenGLRenderer` |
| Add a keyboard shortcut | `onKey` in `main.cpp` |
| Change history behaviour | `PageHistory.h`, `visitPage` / `goHistory` in `main.cpp` |

## 19. Worked examples (how a feature touches the code)

These are sketches of the *shape* of a change, so you know which files and functions are involved. Those marked "(implemented)" describe how an existing feature was added.

### Example A — a simple new JS method: `element.remove()` (implemented)
Only `JSBinding.cpp` changes.
1. Write `static JSValue js_remove(JSContext*, JSValueConst this_val, int, JSValueConst*)`. Get the node with `unwrapNode(this_val)`.
2. Detach it with `detachNode`, which `removeChild` shares: `takeNode` takes the `shared_ptr` out of its parent's `children` (clearing `parent`), and it goes into `state->detachedNodes`.
3. `detachNode` calls `markDirty(ctx)` so the next frame re-lays-out.
4. Register it: `JS_CFUNC_DEF("remove", 0, js_remove)` in `js_node_proto_funcs`.

Rules of thumb for any binding: convert strings with `argStr` / `jsStr`; return nodes with `wrapNode` (never a fresh `JS_NewObjectClass`), so each node keeps one wrapper; never store a raw `Node*` you don't know is still in the tree; call `markDirty` after any DOM mutation.

### Example B — a new tag's appearance: `<hr>`
`hr` is already parsed (it's a void tag) but it has no children and no background, so layout produces nothing. To draw a line:
1. In `LayoutRoot::layoutElement` (`Layout.cpp`), add a branch next to the `<img>` one: `if (e->tag == L"hr") { flushInline(); … }`.
2. Push a `LayoutBox` with `background = L"#999999"`, `height = 2`, `width = containingWidth`, at the current `y`; advance `y` by the height plus margins.
3. Nothing else is needed — `Engine::render` already paints any box that has a background.

### Example C — an inherited CSS property: `color` / `font-weight` (implemented)
Read this as a map of how `color` and `font-weight` were added; a new inherited text property follows the same path. It crosses several layers, because both are **inherited** (like `font-size`):
1. `Layout.h`: the value lives in `LayoutRoot::TextPaint` (`color`, `bold`), held by `ComputedStyle::paint` and `InlineItem::paint`, and copied onto `LayoutBox::color`/`bold`.
2. `Layout.cpp`, `computeStyle`: `sv.paint` starts from the `inheritedPaint` parameter (threaded through `layoutElement`, `collectInline`, `computeStyle` and the grid/flex item calls, exactly as `inheritedFontSize` is), then the tag's default (`<a href>` blue, `<b>`/`<strong>`/`<th>`/headings bold), then `applyDecl`'s `color`/`font-weight` branches.
3. `appendWords` / `layoutInlineRun`: copy `paint` from `InlineItem` into the emitted `LayoutBox`. Anything that changes glyph width (like bold) must also reach `textWidth`, or wrapping will be measured with the wrong font.
4. `Engine::render` (`Engine.cpp`): text is drawn with `parseColor(b.color)` (black when empty) and `b.bold`.

### Example D — a new event type (e.g. `mousedown`)
`addEventListener` already accepts any type, so only firing it is new.
1. Catch the native input (`onMouseButton` in `main.cpp`) and forward it into `Engine` (like `dispatchClick`).
2. In `Engine`, fire it with the matching `fire*` from `JSBinding.h` (`fireMouseEvent(ctx, target, L"mousedown", mouseInfoAt(x, y))`), or add one if the event needs new fields: `makeEvent` builds the event object from a `kBootstrapJS` class, and you set its fields before `fireAndFree`.
3. Treat every `Element*` held across the call as possibly freed: check `stillInPage` before using it again. Any DOM change a listener makes is picked up by the existing `domDirty` relayout.
4. Its `onmousedown` property already exists; for a type that isn't in `kHandlerTypes`, add it there and a `HANDLER_DEF` line in `js_node_proto_funcs`.

### Debugging tips
- `console.log` from a page script prints to stdout and the debugger Output window; script exceptions print as `[script] Error: … <stack>`.
- To see layout, temporarily give every box a background in `Engine::render` (e.g. draw an outline with four thin `drawRect`s per `LayoutBox`); everything is a flat list, so this shows exactly what layout produced.
- Test pages: pass a local `.html` file as the command-line argument (or type its path in the address bar) for fast iteration without the network.
- Most "nothing shows up" bugs are one of: the tag is in the layout skip-list, `display:none` matched, the selector isn't supported (silently skipped by `parseStylesheet`), or the HTML nested wrongly (§7).
