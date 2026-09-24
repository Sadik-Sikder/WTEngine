# WTEngine — How It Works

A working reference for the current codebase. It describes what the code does today, not what is planned. File and function names are given so you can jump straight to the source.

_Snapshot: latest commit `5cfc6d7` ("Add a watchdog timeout to JS execution"), plus this commit's documentation of it._

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
- `console.log(...)` from page scripts appears in the console window (stdout) and the debugger's Output window.

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
  ResourceLoader.cpp         ResourceLoader.h — background script/stylesheet fetch, §5
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
2. `runJSEngineSmokeTest()` prints `1 + 2 = 3` to stdout/debugger (leftover Phase-0 check).
3. Create a 900×600 GLFW window, enable alpha blending.
4. Create `Engine`, `OpenGLRenderer`; call `engine.setRenderer()` (so layout can measure text) and `engine.setTopInset(AddressBar::kHeight)` (the page starts 40 px down).
5. Register the GLFW callbacks. They all reach the `App` struct through `glfwGetWindowUserPointer`.
6. Load the command-line URL, or the default page.

### The frame loop
Each iteration (capped at ~60 fps with `Sleep`):

1. **Apply pending actions** set by input callbacks: history back/forward (`pendingHistory`), navigation (`pendingUrl`), and a queued form submission (`engine.takeSubmission`). Callbacks never navigate directly; they set a flag and the loop does it. That keeps fetching out of the callback stack.
2. `applyFinishedNavigation` — shows a background page fetch's result the moment it's ready (below).
3. Clear, set the viewport, `engine.onResize()`.
4. `renderer.beginFrame()` then `engine.render()` — draws the page.
5. `bar.draw()` — drawn **after** the page so it covers content scrolled under it.
6. Choose the cursor (I-beam over text fields/address bar, hand over links/buttons/nav buttons).
7. Swap buffers, poll events, sleep the remainder of the 16.6 ms budget.

### Navigation (`PageLoader`)
Page fetching is **backgrounded**: `navigate(url, postBody?)` doesn't call `fetchPage()` itself, it calls `app.pageLoader.start(url, postBody, replace)`, which launches a detached `std::thread` to do the actual `fetchPage()` call and returns immediately - so the frame loop, and the window's message pump with it, keeps running while a fetch is in flight, however long it takes. `applyFinishedNavigation` polls `pageLoader.poll(...)` once per frame; the moment a result is ready, it shows the page (`visitPage`) or a generated red error page, exactly as if the fetch had been synchronous.

- **Why:** a single blocking `fetchPage()` call used to freeze the *entire application* - unresponsive, un-closeable except by killing the process - for as long as the underlying fetch took. The original theory (still true, just not the dominant cause - see below) was WinINet's dual-stack connect behavior: on a host whose IPv6 route is black-holed, WinINet waits through the OS's full TCP connect timeout (~20-30s) before falling back to the working IPv4 address.
- **Superseding:** starting a new fetch (or `goHistory`'s Back/Forward, which calls `pageLoader.cancel()`) abandons whatever was previously in flight. Its thread keeps running `fetchPage()` to completion regardless - cheaper than trying to interrupt a blocking network call - but its result is just never read once nothing points at it anymore. See `PageLoader.h`'s comments for how this is made thread-safe (the tricky part: not destroying the result slot's mutex while a `lock_guard` still holds it - a real bug caught during development via a Debug-CRT "unlock of unowned mutex" assertion).
- **Timeout:** `poll()` also gives up on a fetch that's been running longer than 8 seconds, reporting it as failed ("Timed out") from the application's side - independent of whatever the OS is still doing with the underlying connection.
- **The networking layer was later replaced** (WinINet → Boost.Beast/Asio + OpenSSL, §13) specifically to fix the above: `beast::tcp_stream::connect()` applies a short *per-address* deadline (`expires_after`, a few seconds) instead of waiting out the OS's own connect timeout, so a black-holed address is abandoned quickly.
- **This did not fully fix the freeze.** Diagnostic timing (wall-clock around `resolve()`/`connect()`) showed the top-level fetch itself completing in ~1 second even against the site that used to hang for 20-30s - yet the application still went unresponsive for tens of seconds afterward. The actual dominant cause at that point was `Engine::runScripts()` and `loadLinkedStylesheets()` each calling `fetchPage()` synchronously on the main UI thread, once per `<script src>`/`<link rel=stylesheet>` tag - entirely bypassing `PageLoader`. That's now fixed too - see `ResourceLoader` below - but fixing it surfaced a third, larger bottleneck that's still open; see §13's `ResourceLoader` note and the Engine limitation at the end of this doc.
- `visitPage` saves the scroll offset of the page being left, pushes a new `HistoryEntry`, then `showEntry`.
- `showEntry` calls `engine.loadHTML(html, url)`, restores the scroll position, and updates the window title and address bar. (The window title is the URL — `<title>` content is discarded by the parser.)
- `goHistory(±1)` re-displays a stored entry **from its saved HTML** — no network request, and a POSTed result is not re-sent.

### Progressive resource loading (`ResourceLoader`)
A page's own `<script src>` and `<link rel="stylesheet">` fetches are **also backgrounded**, and concurrently rather than serially - fixing the bypass `PageLoader` couldn't cover (above). `Engine::parseAndBuild` and `beginScripts` don't fetch anything themselves: they collect every external stylesheet/script URL in document order and hand the list to a `ResourceLoader` (one per resource kind: `styleLoader_`, `scriptLoader_`).

`ResourceLoader` runs on a **small, fixed pool** of background threads (`kWorkerThreads = 4`, matching `OpenGLRenderer::kImageLoaderThreads` - §14) pulling from a shared queue, not one OS thread per URL. A page referencing hundreds of resources would otherwise spawn hundreds of threads at once on every navigation - real, unbounded overhead a fixed pool avoids entirely, the same reasoning the image loader already applied. `start()` queues the whole batch and wakes every idle worker (`notify_all`, since several may have work waiting, not just one); each worker loops pulling one `{url, slot}` item at a time. Superseding a batch (a new `loadHTML()`, i.e. a new `start()`) doesn't touch the queue - a stale entry left over from the old batch is instead skipped cheaply the moment a worker reaches it: popping the entry leaves its `Slot` referenced only by that worker's local copy *unless* `slots_` (replaced by the new `start()`) still points at it, so `slot.use_count() == 1` means "nothing can ever read this fetch's result" and the worker skips the network I/O entirely rather than spend a pool slot on wasted work. This is best-effort, not a hard guarantee (a fetch that's already in flight, or one that slips past the check a moment before being superseded, just finishes normally and is discarded) - the same abandon-in-place thread-safety pattern `PageLoader` already established, generalized from one in-flight result to a queued batch of them (a `shared_ptr<Slot>` per URL instead of per navigation).

The page paints once immediately after `parseAndBuild`/`beginScripts` return - with only inline `<style>` rules and whatever prefix of inline `<script>`s could run synchronously (see below) - then fills in progressively:

- **Stylesheets** apply independently, in *whatever order their fetches complete* - safe because each one's CSS rules get a precomputed "order band" (`kStyleOrderBand * (its 1-based position among `<link>`s)`) added to their specificity tie-break `order` field at *collection* time, not at *append* time. The old synchronous code relied on append order matching document order, which relied on fetching happening serially; that assumption no longer holds, so tie-breaking had to move from "wherever this landed in `doc->styles`" to "this stylesheet's true position in the document," computed up front.
- **Scripts** must still run in strict document order (side effects, shared globals) despite fetching concurrently and completing in arbitrary order. `Engine::advanceScripts()` walks `scriptTasks_` from a cursor, running each task the instant it's ready (inline is always ready; external once its `ResourceLoader` slot is `done`) and **stopping at the first one that isn't** - even if a later task already finished fetching. It's called once synchronously right after starting the fetches (so a page with no external scripts behaves exactly as it did when this all ran synchronously - no added delay), then again every frame from `pollResources()`.
- `Engine::pollResources()` (called from `render()`, once per frame) is the only place newly-arrived resources get applied. It reuses `domState.domDirty` - the same per-frame "did something change, so re-layout" flag already used for JS timers/DOM mutations (§4's frame loop) - rather than inventing a second signal; applying a stylesheet or running a script just sets it.
- Cancellation follows `PageLoader`'s pattern: a new `loadHTML()` call replaces `styleTasks_`/`scriptTasks_`/both loaders' slots outright. Old in-flight or already-skipped work from an abandoned navigation never touches the new page's state.

### Input handling

| Input | Behaviour |
|---|---|
| Left click in the top 40 px | Back/Forward buttons, or focus/place the caret in the address bar |
| Left click on the page | 1. `Engine::onClick` (form controls): if it hit a control it runs that control's JS click listeners, then performs the control's action unless a listener called `preventDefault()`; the click is then finished. 2. Otherwise find a link with `linkAt`. 3. `Engine::dispatchClick` runs JS click listeners. 4. If a listener did **not** call `preventDefault()` and there was a link, navigate |
| Mouse 4/5, Alt+←/→ | Back / forward |
| Ctrl+L, F6 | Focus the address bar |
| Keys / chars | Go to the address bar if focused, otherwise to the focused page input |
| Tab / Shift+Tab | Next/previous text field |
| Ctrl+V / C / A | Paste / copy / select all (clipboard via GLFW). Copy is blocked for password fields |
| Enter | Address bar: `normalizeInput` then navigate. Page field: submit its form |
| Esc | Blur, and restore the address bar text |
| Scroll wheel | `Engine::scroll(-yoffset * 40)` |

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
- **Raw-text tags** — `script style title textarea`: content is read verbatim up to the matching end tag. `script` and `style` content is kept as one `TextNode` (no entity decoding, so `&&` in JS survives). `title` and `textarea` content is **dropped**.
- **Entities:** `&amp; &lt; &gt; &quot; &apos; &nbsp; &copy; &mdash; &ndash; &hellip;` plus numeric `&#N;` / `&#xH;` (BMP only). Decoded in text nodes and attribute values.
- **Text nodes** are whitespace-trimmed at both ends; empty ones are discarded.
- **Body discovery:** `findBody` looks anywhere in the tree for `<body>`. If there is none, a synthetic `<body>` wraps all top-level nodes. This is what makes `innerHTML` work: the fragment is parsed as a whole "document" and its synthetic body's children are reused.
- **Stylesheets:** all `<style>` text is concatenated and parsed once into `Document::styles`. `<link rel="stylesheet" href="...">` isn't touched here at all (`<link>` is just a void tag to the parser) - see `Engine::parseAndBuild` in §10 for where those get fetched.

**Important simplification:** an end tag closes the *current* element regardless of its name (`endName` is read but never compared). There are no implied end tags either (an unclosed `<p>`, `<li>` etc. swallows following siblings until some end tag appears). Well-formed pages work; sloppy ones will nest oddly.

## 8. CSS (`CSS.h`, `CSS.cpp`)

**Parsing** (`parseStylesheet`): strips `/* */` comments, drops every `@`-rule (so `@media`, `@import`, `@font-face` never apply), splits comma groups, and produces one `Rule` per selector with `chain`, `declarations`, `specificity` and source `order`.

**Selectors supported:** `tag`, `.class`, `#id`, `*`, compounds like `div.card#x`, and the **descendant** combinator (`a b`, any depth). **Not supported** (the whole selector is skipped): `>`, `+`, `~`, `[attr]`, `:pseudo`.

**Matching** (`CSS::matches`): the last compound must match the element; earlier compounds must match *some* ancestor in order, right to left. No selector index - every rule is checked against every element (`LayoutRoot::computeStyle`, §10), so cost is roughly elements × rules; the optional `ClassCache*` parameter only avoids re-parsing the same element's `class=""` repeatedly across those checks (§10's Engine limitations), it doesn't reduce how many checks happen.

**Cascade** (in `LayoutRoot::computeStyle`): matched rules are stable-sorted by specificity `(ids, classes, tags)` then source order and applied in that order; then the inline `style=""` attribute is applied last (always wins). `!important` is stripped but gives no extra priority.

**Properties the engine actually honours:**

| Property | Handling |
|---|---|
| `background`, `background-color` | Kept as a string; only `#rrggbb` is understood |
| `margin`, `padding` | Shorthand: 1/2/3/4 space-separated values, expanded per the usual CSS rule (`T`, `T/R`, `T/R-L/B`, `T/R/B/L`) |
| `margin-top/right/bottom/left`, `padding-top/right/bottom/left` | Individually, px or `%` (of the containing block's width - CSS's rule for percentage margin/padding on every side, top/bottom included) |
| `width` | px or `%` of the containing block's width; unset (or `auto`) fills the container, as always |
| `border`, `border-width`, `border-color` | `border: 1px solid #rrggbb`-style shorthand, or the longhands. The style keyword (`solid`/`dashed`/…) is accepted but ignored - every border draws the same way |
| `box-sizing` | `content-box` (default) or `border-box`; see §9 |
| `grid-template-columns`, `grid-template-rows` | Space-separated px/`%`/`fr` tracks, and `repeat(N, <track>)`; see §9's Grid subsection. `fr` rows are treated as unset (no definite container height to distribute against) |
| `grid-template-areas` | One or more quoted strings, each a grid row, each whitespace-separated token a column's area name (`.` = no area); see §9 |
| `grid-template` | Shorthand - only the area-string form (e.g. `"a a" 40px "b c" 1fr / 100px 1fr`), not the plain `<rows> / <columns>` form; see §9 |
| `grid-area` | On a grid item: the area name to place into (must match a name in the container's `grid-template-areas`) |
| `grid-column`, `grid-row` | On a grid item: `"2"` (start, span 1), `"2 / 4"` (start/end line numbers), or `"2 / span 3"`. Also the `-start`/`-end` longhands (a bare integer each). Both axes must be set for an item to be explicitly placed; one alone is treated as fully automatic. `grid-area` takes precedence if both are set and the area name resolves |
| `gap`, `row-gap`, `column-gap` | `gap: <row>` or `gap: <row> <column>`, px or `%` - shared by grid and flex, same properties either way |
| `flex-direction` | `row` (default) or `column`; see §9's Flex subsection |
| `justify-content` | `flex-start` (default), `center`, `flex-end`, `space-between`, `space-around` |
| `align-items` | `stretch` (default), `flex-start`, `center`, `flex-end` |
| `flex-grow`, `flex` | A bare number, on a flex item. `flex: N` is simplified to set `flex-grow` only (real CSS's shorthand also sets `flex-shrink`/`flex-basis`, neither modeled) |
| `font-size` | px, `em`, `rem`, `%` (relative to the inherited size); inherited by children |
| `display` | `none`, `block`, `inline`, `inline-block` (treated as inline), `grid`, `flex` |

Everything else (`color`, `height`, `float`, `position`, `font-weight`, …) is parsed and ignored. Text is always black; links are always blue. `em`/`rem` aren't supported for `width`/`margin`/`padding`/`border-width`/grid tracks/`gap` (only `font-size` resolves those) - use px or `%`.

`CSS::parseSelector` and `CSS::matches` are also reused by JS `querySelector`.

## 9. Layout (`Layout.h`, `Layout.cpp`)

`LayoutRoot::layout()` starts at `<body>` at `(10, 10)` with width `viewportWidth − 20` and walks the DOM once, appending to `boxes`.

### `LayoutBox`
`x, y, width, height` (document space), plus optional `background`, `borderWidth`/`borderColor`, `text`, `href`, `imageSrc`, `fontSize`, `control` (`TextField | Button | Checkbox | Select`), `el` (source element) and `form` (enclosing `<form>`). One struct serves as a background/border rectangle, a single word of text, an image, or a form control.

### The box model (`ComputedStyle`, `layoutElement`'s block branch)
`ComputedStyle` carries the full box model for an ordinary block element: `marginTop/Right/Bottom/Left`, `paddingTop/Right/Bottom/Left`, `width` (`-1` = auto), `borderWidth`/`borderColor`, and `boxSizing` (`ContentBox` | `BorderBox`). `layoutElement`'s block branch turns these into two widths before laying out children:

- **`outerWidth`** - what actually gets painted (the border/background edge). With `width` unset, it's `containingWidth - marginLeft - marginRight`, same as always. With `width` set: under `content-box` (the default), `width` names the *content* box, so outer = `width + padding + 2×border`; under `border-box`, `width` already *is* the outer size.
- **`contentWidth`** - `outerWidth` minus padding and border, and what children are actually laid out into.

This box-model computation - and reserving the background/border box, sized to `outerWidth`, height patched in once the content's natural height is known - is factored into `layoutBlockChild(e, x, y, containingWidth, style)`, called once per block-level child from `layoutElement`'s loop and, for each grid/flex item, from `layoutGrid`/`layoutFlex` (below). Once the box model is resolved, `layoutBlockChild` recurses into `layoutElement(e, ...)` as always - unless `style.display` is `Grid` or `Flex`, in which case it calls `layoutGrid(e, ...)` or `layoutFlex(e, ...)` instead.

This still means **explicit CSS `height` isn't supported** - a box is always exactly as tall as its content, `overflow: visible`-style clipping/`height` isn't modeled. `width`/`border`/`box-sizing` currently apply to plain block and grid/flex-item elements only, not to `<input>`/`<button>`/`<select>` (`layoutControl`, unchanged) or `<img>` (`layoutImage`, unchanged) - those keep their own fixed/intrinsic sizing.

`Engine::render` paints a border as four thin rects forming a hollow frame (not one filled rect), so a border with no background still lets whatever's behind the box show through the middle, then paints the background inset by the border width.

### Grid (`layoutGrid`)
`display: grid` on a block-level element runs `layoutGrid` instead of the usual vertical flow, but only for *that element's own children* - everything above (the grid container's own margin/padding/border/background) is unchanged, since `layoutBlockChild` handles that identically for a block or a grid container.

1. **Columns**: `grid-template-columns` is parsed (`parseGridTemplateTracks` - also used for rows, below, since the track grammar is identical on both axes) into a list of `GridTrack { isFr, value }` - a fixed px/`%` width, or an `fr` share. `repeat(N, <track>)` is expanded textually first (`"repeat(3, 1fr)"` → `"1fr 1fr 1fr"`); a track keyword this doesn't understand (`auto`, `minmax(...)`, `fit-content(...)`) becomes `1fr`, so the *number* of columns an author wrote is always honored even where the sizing isn't. If `grid-template-areas` names more columns than there are tracks for, the missing ones are padded with `1fr`; with no `grid-template-columns` (or `-areas`) at all, it falls back to one column spanning the full width (items stack, rather than the grid disappearing). Column pixel widths: sum the fixed tracks and the gaps, split what's left among the `fr` tracks proportionally.
2. **Placement**: `el`'s direct element children (skipping the usual non-visual tags and any `display: none`) are read into an `ItemPlacement` list. An item with `grid-area` naming a cell that appears in `grid-template-areas` (`parseGridTemplateAreas` - one grid row per quoted string, whitespace-separated column names within it, `.` meaning no area) is placed at that name's bounding box across every cell it appears in - real CSS requires those cells to form a rectangle; this doesn't specially validate that, it just takes the bounding box regardless. Otherwise, an item with **both** `grid-column` and `grid-row` set (`parseGridLinePlacement` - `"2"`, `"2 / 4"`, or `"2 / span 3"`, into 1-based start/end line numbers) is placed into that exact cell range directly, clamped to the template's column count (a line beyond it doesn't create an implicit column). One axis set without the other, or a `grid-area` naming nothing in the template, is treated as fully automatic, not partially placed - a deliberate simplification. Every remaining item then auto-places row-major (`grid-auto-flow: row`, the CSS default), walking a `(row, col)` cursor forward and skipping any cell an explicit item already claimed (tracked in a `std::set<std::pair<int,int>>`) - simpler than real CSS's own auto-placement (which packs more tightly around explicit items) and can leave a gap a "dense" packing algorithm would have filled instead. **Not supported:** alignment properties, and a bare (non-element) text node directly inside a grid container is dropped rather than becoming an anonymous item.
3. **Row height**: a row's height depends on every item in it (more so now that an item can span several rows), which `layoutElement`'s single top-to-bottom `y` sweep can't express - so every item is laid out once, into a scratch `boxes` vector at local `(0, 0)` (`std::swap(boxes, scratch)` redirects every `boxes.push_back` anywhere in that call - `layoutControl`, `layoutImage`, or `layoutBlockChild` for a plain item, chosen the same way `layoutElement`'s loop would) to discover its natural height, *before* any row height is decided. An explicit `grid-template-rows` track wins for a given row if set; `fr` row tracks are treated as unset instead (see its `ComputedStyle` comment - `fr` needs a definite container height to distribute against, which this engine's always-auto-height pages don't have, the same wall `layoutFlex`'s column direction hits). Otherwise a row is as tall as the tallest *single-row* item placed in it; an item spanning multiple rows bumps the *last* row it spans if the rows it's already in (plus the row-gaps between them) aren't tall enough for it, rather than distributing the shortfall across all of them. `grid-template-areas` sets the row *count* too, even for a trailing row nothing ends up placed in (e.g. one made entirely of `.` cells). Once every row's height is settled, each item's saved boxes are translated by `(columnX[colStart], rowY[rowStart])` and appended to the real `boxes`. A grid item that would otherwise be `display: inline` (e.g. a bare `<span>`) is "blockified" first, matching real CSS - there's no flowing paragraph for it to join inside a cell.
4. `ancestorStack` gets `el` (the grid container) pushed for the whole function, so a descendant selector matching a grid item still sees the container as an ancestor, exactly as it would for a plain block's children.
5. **The `grid-template` shorthand** (`parseGridTemplateShorthand`) supports only its area-string form - alternating quoted area-rows and an optional row-size token right after each one's closing quote, then an optional `/ <column tracks>` suffix (e.g. `"header header" 40px "sidebar main" 1fr / 100px 1fr`). A row with no size token gets `GridTrack{isFr:true, value:0}` - a zero-share `fr` track, which the row-height logic above already treats as unset, so "no size given" rides that same rule instead of needing a new one. The plainer `<rows> / <columns>` form (no area strings) isn't supported - use the `grid-template-rows`/`-columns` longhands for that instead.

### Flex (`layoutFlex`)
`display: flex` runs `layoutFlex` instead of the usual vertical flow, the same way `display: grid` runs `layoutGrid` - only for the container's own children, box model unchanged. Row and column direction are different enough (which axis is "main" swaps entirely) that they're really two algorithms sharing one function.

**Row direction** (`flex-direction: row`, the default) - the genuinely hard part, and the reason this isn't spec-accurate:
- An item's width is its explicit CSS `width` if set. Otherwise it gets a *share* of whatever width is left after every explicit-width item and every gap is subtracted - 1 share by default, or its own `flex-grow` if explicitly set and positive.
- Real CSS flexbox sizes an unflexed item by its *content* instead (min/max-content sizing) - this engine has no equivalent of that anywhere (text wrapping already needs a width handed to it, it doesn't derive one), so an item with neither `width` nor `flex-grow` would otherwise collapse to zero. The equal-share fallback is the same tradeoff `layoutGrid` already makes for an untemplated grid ("no template → one full-width column" there; here, "no width/flex-grow → an equal share"), and gives common patterns (nav bars, equal-width card rows, button groups) a reasonable result instead of disappearing.
- `justify-content` only has a visible effect when every item has an explicit width and their sum is still less than the container - any item using a share consumes 100% of the leftover space by construction, leaving none for `justify-content` to distribute. This matches real flexbox's own behavior in that case, not a shortcut.
- A row's height, like a grid row's, isn't known until every item in it has been laid out - so each item is laid out once into a scratch `boxes` vector at local `(0, 0)` (same `std::swap(boxes, scratch)` technique `layoutGrid` uses) to discover its natural height, then translated into place once the row's height (the tallest item) is known.
- `align-items: stretch` (the default) is approximated by extending each item's own background/border box (if it made one - see `layoutBlockChild`) to the row's height, rather than by re-flowing its content into the extra space; an item with no background/border has nothing visible to stretch anyway.

**Column direction** (`flex-direction: column`) - no equivalent sizing problem, since the main axis is height, and height is exactly what ordinary block layout already produces from content:
- Items are normal block children stacked vertically, honoring `row-gap`/`gap`.
- `justify-content` has no effect: distributing leftover space along a container's height needs a *definite* height to distribute within, and this engine's pages are always "auto" height (they grow to fit content) - the same behavior real CSS flexbox shows for an auto-height flex column, not a shortcut unique to this engine.
- `align-items` does work, on the cross axis (horizontal, here): an item with an explicit `width` can be positioned via `flex-start`/`center`/`flex-end` within the container's width; one without always fills it (`stretch`, the default, and the only sensible behavior for something with no natural width to fall back to - same reasoning as row direction's fallback).

**Not supported (either direction):** `flex-wrap` (always single-line/single-column), `flex-shrink`, `flex-basis` as a value distinct from `width`, `align-content`, `align-self`, and `order`.

### Algorithm (`layoutElement`)
For each child:
- **Text node** → split into words and buffered in `pendingInline`.
- **Skipped entirely:** `head script style title meta link base`, and anything whose computed `display` is `none`.
- **`<br>`** → a forced-break item in the inline buffer.
- **`<input>`, `<button>`, `<select>`** → flush inline, then `layoutControl`.
- **`<img>`** → flush inline, then `layoutImage`.
- **Inline element** (`a span b strong i em u small code sub sup mark label abbr cite q`, or `display:inline`) → `collectInline` flattens its text and nested inline children into the same run. `<a href>` sets `currentHref` for its words.
- **Block element** (or `display: grid`/`flex`) → flush inline, then `layoutBlockChild`: resolve the box model above, add `margin-top`, reserve a background/border box if it has either (height patched afterwards), add `border` + `padding-top`, recurse into `layoutElement` (or `layoutGrid`/`layoutFlex`, above, for `display: grid`/`flex`), add `padding-bottom` + `border` and `margin-bottom`. `<form>` sets `currentForm` for the subtree either way.

### Inline runs (`layoutInlineRun`)
Greedy word wrapping. Each word gets its own `LayoutBox` (so words on one line can differ in size, link, or owner). Line height = tallest font on the line + 8. A word wider than a line is split by characters. A 6 px gap follows each run.

### Sizing rules
- **Text field:** width from `size` attribute (default 20 chars × 8 + 16), height `max(28, font + 14)`.
- **Button:** text width + 24, height `max(30, font + 16)`. `<input type=submit|button|reset|image>` are buttons.
- **Checkbox:** `max(18, font + 4)` square. **Select:** 160 wide.
- `hidden`, `radio`, `file`, `range`, `color` inputs produce **no box** at all (hidden takes no space; the rest are unsupported).
- **Image:** explicit `width`/`height` attributes win per axis; otherwise natural size if already decoded; otherwise a 200×150 placeholder. Width is clamped to the container.

`ancestorStack` is maintained during the walk so `computeStyle` can evaluate descendant selectors.

Text is measured through a `std::function` set by `Engine` that calls `Renderer::measureText`, so wrapping uses real Segoe UI metrics.

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
   - otherwise background rect → image (`drawImage`, then `continue`) → text (blue if it has an `href`, else black; drawn at `+4,+4` inside the box).
5. `drawOpenSelect` overlay.

### Coordinates
There are two spaces. **Document space** is what layout produces (`b.x`, `b.y`). **Window space** is what mouse events use. Convert with `docY = y − topInset + scrollY`. X needs no conversion.

### Hit-testing
All hit-tests scan `boxes` in **reverse** (last painted = topmost):
- `linkAt` — words with an `href`, tested against the actual text width.
- `controlAt` — form controls, by bounding box.
- `dispatchClick` — any box with an `el`, by bounding box.

## 11. JavaScript (`JSEngine.cpp`, `JSBinding.cpp`)

### Per-page realm
Every `loadHTML` creates a **fresh** `JSEngine` (quickjs runtime + context) and resets `DOMBindingState`. Nothing survives navigation. `JSEngine::eval` runs a global script and returns the result string, or `"Error: message\nstack"` on exception. `console.log/warn/error` all print `[console] …` to stdout and the debugger output.

**Teardown order matters.** `Engine::beginScripts` resets `domState` first, then destroys the old `JSEngine`, then creates the new one. `domState` owns `JSValue`s (listeners, timers) that must be freed against a runtime that still exists; quickjs asserts (Debug) or corrupts memory (Release) if a runtime is destroyed while any value is alive. Keep that order if you touch it. The same rule is why `domState` is declared after `jsEngine` in `Engine.h` (members destruct in reverse).

### Promises and microtasks
quickjs never runs promise jobs on its own; the host must pump them. `runPendingJobs(ctx)` (`JSEngine.cpp`) drains the job queue, like a browser's microtask checkpoint. It runs after every script's `eval`, after each click listener, and after each timer callback, so `Promise.then` and `async`/`await` continuations behave in the expected order (`sync code → promise callbacks → timers`). A job that throws is printed as `[promise] Error: …` and the rest still run. **Any new place that calls into JS (`JS_Call`, `JS_Eval`) should call `runPendingJobs` afterwards.** Unhandled promise rejections are not reported (no rejection tracker is installed).

### Script execution (`Engine::beginScripts` / `advanceScripts`)
- Runs **after the entire DOM is built**, in document order. So no `document.write`, and scripts can see the whole page.
- `beginScripts` handles inline scripts and `src=` scripts, but doesn't fetch or run anything itself beyond building `scriptTasks_` (one per `<script>`, either the inline code or an index into `scriptLoader_`) and starting every external script's fetch concurrently (`ResourceLoader`, §5). `advanceScripts` does the actual running: it walks `scriptTasks_` from a cursor, executing each task the moment it's ready (inline: immediately; external: once its fetch completes) and **stopping at the first one that isn't ready yet**, even if a later task's fetch already finished - preserving document order despite concurrent, out-of-order fetch completion. `beginScripts` calls it once synchronously right after starting the fetches (so a page with only inline scripts runs them all immediately, same as before backgrounding existed), and `pollResources` calls it again every frame to pick up whatever's newly ready.
- Only "classic" scripts run: no `type`, or `text/javascript`, `application/javascript`, `application/ecmascript`. `type="module"`, JSON-LD, templates etc. are skipped.
- An exception is printed as `[script] Error: …` and execution continues with the next script.

### Script execution watchdog
JS runs synchronously on the UI thread (there's no worker/off-thread execution model, and quickjs contexts aren't meant to be shared across threads), so a script stuck in an infinite loop used to freeze the whole window indefinitely - the same freeze class as the pre-fix `fetchPage()`/`LayoutRoot::layout()` issues above, just triggered by page JS instead of networking or CSS matching.

Fixed with quickjs-ng's `JS_SetInterruptHandler` (`JSEngine.cpp`): installed once per `JSEngine`/`JSRuntime`, it's polled from inside the bytecode interpreter every ~10000 ops. `ArmScriptWatchdog(ctx)` resets a `steady_clock` deadline (`kScriptTimeout` = 2000ms, `JSEngine.h`) to `now() + kScriptTimeout`, and is called immediately before **every** real entry point into JS - `JSEngine::eval` (script tags), `runPendingJobs` (promise jobs, per job drained), and `JSBinding.cpp`'s `fireDueTimers`/`dispatchClick` (per timer/listener call) - so the deadline always measures "how long has this one script/callback run," not wall clock since page load. Exceeding it throws quickjs's own uncatchable `InternalError: interrupted` (`JS_ThrowInterrupted`/`JS_SetUncatchableError` in quickjs.c) - uncatchable specifically so a script's own `try { while(true){} } catch(e){}` can't defeat it. The four call sites already had a call-then-check-exception pattern (§ above, and the Events/timers section below), so no new control flow was needed beyond arming the deadline and, for timers, distinguishing a watchdog kill from an ordinary throw (`JS_IsUncatchableError`) to decide whether to keep the timer around.

Verified against three cases (a scratchpad test page + `CloseMainWindow`/stdout capture, since `wprintf` is fully-buffered once redirected and only flushes on normal process exit): a `<script>while(true){}</script>` page recovers and finishes rendering the rest of the DOM instead of hanging (confirmed both via `Process.Responding` staying `True` throughout and via `[script] Error: InternalError: interrupted` in the log); a `setInterval(() => { while(true){} }, 0)` is killed once and then *not* rescheduled (CPU time measured over the following 6s: +0.84s, not the ~6s repeated-kill churn it'd be if the timer kept re-firing); and a genuine `throw new Error(...)` still surfaces as `Error: <message>` unchanged, confirming no regression to the normal error path.

### What JS can see
Everything is installed by `installDOMBindings`.

**Globals:** `document` (a wrapper around `<body>`), `window` (an alias of the global object), `self`/`parent`/`top` (all also aliases of the global object - WTEngine has no `<iframe>`/frame support, so every page legitimately *is* its own top-level, un-framed window, exactly like `self === parent === top === window` for a real un-framed page), `location`, `console`, `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval`.

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
| `getAttribute`, `setAttribute`, `hasAttribute`, `removeAttribute` | |
| `createElement`, `createTextNode` | Available on any node, including `document` |
| `appendChild`, `removeChild` | |
| `getElementById`, `getElementsByTagName` | Search the subtree of the node they're called on |
| `querySelector`, `querySelectorAll` | Same selector grammar as CSS (§8); results are plain arrays |
| `addEventListener('click', fn)` | Only `click` is supported |

**Event object:** `event.target`, `event.preventDefault()`.

### Memory model
- JS wrapper objects hold a raw `Node*` (no finalizer, non-owning).
- Nodes made by `createElement`/`createTextNode`, or removed by `removeChild`, are kept alive in `DOMBindingState::detachedNodes` until the page is left. `appendChild` moves the `shared_ptr` from there into the parent. Because of this, **only freshly created (detached) nodes can be appended; re-parenting an existing node silently does nothing.**
- Listeners and timers hold `JSValue`s in `ListenerStorage` / `TimerStorage` (defined only in `JSBinding.cpp` so `quickjs.h` stays out of `Engine.h`). They are freed when the state is reset.

### Events and timers
- **Click:** `dispatchClick` walks from the hit element up through `parent`, calling every registered listener at each level (bubbling). `preventDefault()` suppresses link navigation, and for form controls it cancels the control's action (checkbox toggle, form submit, dropdown open). `stopPropagation` does not exist; exceptions in listeners are swallowed (including a watchdog kill - see above; unlike timers, a hung click listener isn't auto-disabled, since a click doesn't repeat on its own the way an interval does).
- **Timers:** stored with an absolute due time on the same clock as `render()`'s `timeSeconds` (`glfwGetTime`). `fireDueTimers` runs once per frame, so timer resolution is one frame (~16 ms). Callbacks are looked up by id right before being called, so a timer can safely clear itself or others. New timers scheduled inside a callback wait until the next frame. Intervals resync to "now" instead of catching up on missed ticks. A callback killed by the script watchdog (above) is removed instead of being rescheduled - otherwise a broken `setInterval(fn, 0)` would get killed and immediately re-armed every frame forever, trading the old "window frozen" failure for a new "permanent 100% CPU" one.
- **Dirty flag:** any mutating binding sets `domDirty`; `Engine::render` re-lays-out at the top of the next frame.

## 12. Form controls (`EngineForms.cpp`)

Part of `Engine`, split out for size.

- **Text fields:** one `TextEditor` instance (`editor`) holds the text/caret/selection for whichever field is focused. After every edit `syncValue()` writes it back to the element's `value` attribute. Password fields are masked with bullets and can't be copied or word-selected. The field scrolls horizontally to keep the caret visible; the caret blinks (0.6 s on, 0.4 s off).
- **Clicking:** double-click (within 0.4 s and 4 px) selects a word.
- **Checkbox:** toggles the `checked` attribute.
- **Select:** click opens a dropdown overlay (`openSelect`, plus a snapshot of the closed box). The overlay is **not** part of `boxes` — it is drawn last and hit-tested first. Any click, inside or outside, closes it; a click on a row chooses that option (sets `selected`). Any re-layout also closes it.
- **Buttons:** if inside a form and a submit type (`<button>` defaults to submit; `<input>` only for `submit`/`image`), the form is queued.
- **Submission** (`queueSubmit` → `takeSubmission` → `submitForm` in `main.cpp`): `collectFields` walks the form's subtree in document order, encoding `name=value` pairs. Included: text-like inputs, checked checkboxes (`on` if no value), the selected option of each `<select>`, and the clicked submit button. Excluded: unnamed controls, `button/reset/file` inputs, unclicked submit buttons. **Method:** `post` → POST body (`application/x-www-form-urlencoded`); anything else → GET, fields appended as the query string. The action is resolved against the current URL; only `http(s)` targets are sent.
- **Click vs. JS:** when a click lands on a control, `onClick` first runs the JS click listeners for that control (bubbling up through its ancestors, so a listener on a wrapping `<div>` or the `<form>` also fires). If none called `event.preventDefault()`, the control's own action follows: a checkbox toggles, a submit button queues its form, a select opens. `preventDefault()` cancels that action, which is how the common `button.addEventListener('click', e => { e.preventDefault(); … })` pattern works. Focusing a text field is not cancelled, matching browsers. Because `onClick` already dispatched the click, `main.cpp` must not call `dispatchClick` again for a control hit. If a listener rewrites the page and removes the control (e.g. `innerHTML =`), the action is skipped. Not supported: a `submit` event on the form itself (only clicks on the submit button and Enter in a text field submit), and clicks that land on an open `<select>` dropdown don't reach listeners (the dropdown consumes them).

## 13. Fetching (`Fetcher.cpp`)

HTTP(S) I/O goes through **Boost.Beast/Asio + OpenSSL** (from vcpkg, `x64-windows-static-md` triplet - see §2), not WinINet. URL parsing/combining (`InternetCrackUrlW`/`InternetCombineUrlW`) is pure string manipulation with no networking involved, so it's kept as-is from the original WinINet-based implementation rather than writing a new URL parser - `wininet.lib` is still linked for exactly that, and for nothing else.

- **Connecting:** `beast::tcp_stream::connect()` is given a short per-address deadline (`expires_after`, a few seconds) before trying each of DNS's resolved addresses - the practical effect of RFC 8305 Happy Eyeballs (fast abandonment of an unreachable address) without actually racing connections in parallel. `tcp::resolver::resolve()` itself (the DNS lookup) has no such deadline - Boost.Asio's synchronous resolver API doesn't expose one.
- **TLS:** one shared, thread-safe `ssl::context` (a function-local `static`, the standard safe-to-share-across-threads OpenSSL pattern) per process. It loads the live Windows "ROOT" certificate store (`CertOpenSystemStoreW` → `d2i_X509` → `X509_STORE_add_cert`) into OpenSSL's trust store, since OpenSSL has no native notion of Windows' store. Hostname verification (`ssl::host_name_verification`) is required in addition to chain-of-trust verification - the latter alone would accept any validly-CA-signed certificate for any host. SNI is set via `SSL_set_tlsext_host_name`.
- **Decompression (`decompressBody`, via zlib - from vcpkg, same triplet as Boost/OpenSSL):** every request sends `Accept-Encoding: gzip, deflate`, and a response with a matching `Content-Encoding` is inflated (`inflateInit2` with `windowBits = 15+32`, zlib's own documented trick for auto-detecting either a gzip or a zlib/deflate header, so one code path handles both) before anything else touches the body. This isn't optional best-effort handling: confirmed against a real site, some CDNs (S3/CloudFront serving statically pre-compressed objects, observed here) send `Content-Encoding: gzip` *unconditionally*, regardless of whether the request even included an `Accept-Encoding` header asking for it - so **any** HTTP client talking to a server like that receives compressed bytes whether it wants to or not. Before this existed, `fetchPage`/`fetchBytes` fed those raw compressed bytes straight to the CSS parser, the JS engine, or the image decoder - which quietly parsed little-to-nothing out of what looked like binary garbage, with no error surfaced anywhere (a page that "looks unstyled" or "doesn't run its scripts" for no apparent reason on a real site is a likely symptom of exactly this, prior to this fix). On decompression failure (corrupt/truncated data, or a `Content-Encoding` claimed but not actually used), `decompressBody` returns the original bytes unchanged rather than an empty result. Brotli (`br`) isn't supported - would need a separate library, and this file never advertises `br` support, so a well-behaved server shouldn't send it unasked.
- **Connection pooling (`ConnectionPool`):** a request no longer always pays for its own TCP+TLS handshake. `sendOneRequest` first tries a pooled connection for `host:port` (via `sharedConnectionPool()`, a process-wide, mutex-guarded, function-local-`static` pool - same lifetime pattern as `sharedSslContext()`); only if none is available does it open a fresh one (`sendFreshHttps`/`sendFreshPlain`). Measured need (see §5's Engine limitations note): on a real page, most requests land on the same host (its own origin, or a shared CDN), and a handshake costs roughly 200ms - dominated by the TLS handshake alone - that a second request to the same host shouldn't have to pay again.
  - Each pooled connection owns its own `net::io_context` alongside the stream (`PooledPlainConnection`/`PooledSslConnection`) - an Asio/Beast stream is permanently bound to the `io_context` it was constructed with, so reusing a stream later, potentially from a different background thread (`PageLoader`/`ResourceLoader`/the image loader all call into this file concurrently), means keeping that `io_context` alive with it, not just the socket.
  - A connection is only pooled if the response didn't say `Connection: close` (checked via `responseWantsClose`) - now the exception rather than the rule, since requests send `Connection: keep-alive` (this file used to always send `close`, i.e. one request per connection, before pooling existed). Its advertised lifetime comes from the response's `Keep-Alive: timeout=N` header if present (`parseKeepAliveTimeout`), else a conservative 4-second default (`kDefaultPoolTimeout`) - safely under most servers' real defaults (commonly 5-15s). `Keep-Alive: max=N` (the request-count limit) isn't tracked separately; running into it is just another way a pooled connection turns out to be unusable, already covered by the next point.
  - **Stale-connection handling:** a pooled connection can still have been silently closed by the server between uses - unavoidable with pooling, only recoverable from. Reusing one is wrapped in its own `try`/`catch`: if `write`/`read` throws, that exception is swallowed and a fresh connection is opened instead, transparently - only a *fresh* connection's failure is reported to the caller. `expires_after(10s)` is set before reusing a pooled connection (same as a fresh one), so a connection that accepted the write but never responds fails via timeout rather than hanging.
  - Bounded to `kMaxPooledPerHost` (4) idle connections per host - a `give` past that cap just lets the connection close instead of growing the pool without limit.
- **`WIN32_LEAN_AND_MEAN`** must be defined before `<windows.h>` in this file - otherwise `windows.h` pulls in the legacy `winsock.h`, which conflicts with Asio's `winsock2.h` (`error C1189: WinSock.h has already been included`).

| Function | Purpose |
|---|---|
| `fetchPage(url, postBody?)` | HTML text. http(s) via Beast/Asio (`GET` with `Accept: text/html`, or `POST`); anything else is treated as a local file path. Status ≥ 400 → error. Follows up to 10 redirects (always as GET after the first hop). Decodes UTF-8 (BOM stripped). Reports the final URL after redirects |
| `fetchBytes(url, out)` | Raw bytes for images: http(s), local file, or `data:…;base64,…` URI |
| `resolveUrl(base, href)` | Makes an absolute URL via `InternetCombineUrl`. Returns `""` for fragments (`#…`), non-http schemes (`javascript:`, `mailto:`, …), and relative links from a local file |
| `urlEncodeForm(text)` | UTF-8 percent-encoding, space → `+` |
| `withQuery(url, query)` | Replaces the query string and fragment |

The user agent is `WTEngine/0.1`. Every caller of `fetchPage`/`fetchBytes` now runs it off the main thread: the top-level page fetch via `PageLoader` (§5), external scripts/stylesheets via `ResourceLoader` (§5's "Progressive resource loading"), and images via their own loader thread pool (§14). Nothing in the engine calls `fetchPage`/`fetchBytes` directly from the UI thread anymore - confirmed by grepping every call site.

## 14. Rendering (`Renderer.h`, `OpenGLRenderer.cpp`)

`Renderer` is an abstract interface (`drawRect`, `drawText`, `measureText`, `drawImage`, `preloadImage`, `imageGeneration`, `setClip`, `clearClip`); `OpenGLRenderer` is the only implementation. `parseColor` (declared in `Renderer.h`, defined in `Engine.cpp`) turns `#rrggbb` into a `Color`.

- **Projection:** `glOrtho(0, w, h, 0)` — top-left origin, y down. `scrollY` is not passed to the renderer; `Engine` subtracts it before drawing.
- **Rects:** immediate-mode `GL_QUADS`.
- **Text:** rendered once with GDI (white on black into a DIB, Segoe UI, anti-aliased), converted so brightness becomes alpha, and uploaded as an RGBA texture. The colour is applied at draw time via `glColor4f`, so one texture serves any colour. Cached in `textCache` keyed by `text@size`. **The cache never evicts.**
- **`measureText`:** GDI `GetTextExtentPoint32W` with a cached `HFONT` per size, so measuring doesn't create textures.
- **Clipping:** `glScissor`, with y flipped to OpenGL's bottom-left origin.

### Image loading pipeline
```
layout/draw asks for URL ──► getOrCreateImageTexture ──► reserve slot (Loading), queue URL
                                                               │
        4 worker threads (COM initialised each): fetchBytes + WIC decode → RGBA
                                                               │  pendingUploads (mutex)
        main thread, start of next beginFrame(): drainPendingImageUploads
            → glTexImage2D (Ready) or Failed;  imageGen++
                                                               │
        Engine::render sees imageGeneration changed → doLayout()
```
Only the main thread makes GL calls. A `Failed` image is not retried. While an image loads, `drawImage` draws nothing and layout uses the 200×150 placeholder (or the `width`/`height` attributes); when it finishes, the page re-lays-out to the real size. Any WIC format works (PNG, JPEG, GIF first frame, BMP, …). There is no CSS `background-image` and no `srcset`.

## 15. Shared helpers

- **`TextEditor`** — text + caret + selection for one line. Used by both the address bar and page inputs, and knows nothing about drawing. Handles typing (with UTF-16 surrogate pairs), paste, Backspace/Delete/←/→/Home/End, click-to-caret, double-click word select, and masking. There is no Shift-selection with the keyboard.
- **`AddressBar`** — wraps a `TextEditor`; adds focus state, Back/Forward buttons, drawing, and URL normalisation.
- **`PageHistory`** — two stacks around `current_`. `visit()` clears forward history; back stack is capped at 50 entries. Stores each page's HTML and scroll offset.

## 16. Life of a click on a link

1. GLFW → `onMouseButton` (`main.cpp`); position converted to framebuffer pixels.
2. Not in the bar → `Engine::onClick`: not a control → `false`. (On a control, `onClick` would run the listeners and the control's action itself, and the click would end here; see §12.)
3. `Engine::linkAt` returns the `href`.
4. `Engine::dispatchClick` finds the topmost box → element, bubbles JS listeners; returns whether `preventDefault` was called.
5. No prevent → `resolveUrl(currentUrl, href)` → `app->pendingUrl`.
6. Next frame: `navigate` → `fetchPage` → `visitPage` → `showEntry` → `Engine::loadHTML` → parse → run scripts → layout → render.

## 17. Known limitations and rough edges

**HTML / CSS**
- Sloppy HTML nests wrongly (any end tag closes the current element; no implied end tags).
- `<title>` is discarded, so the window title is the URL.
- Colours: only `#rrggbb`. Named colours (`red`), `#rgb`, `rgb(...)` and malformed values (e.g. `#gggggg`) all fall back to light gray. (`parseColor` validates the hex digits, so bad input can't crash the engine.)
- External stylesheets (`<link rel=stylesheet>`) are fetched and applied, but always cascade after every inline `<style>` block regardless of true document order (§10). No `@media`, no pseudo-classes, attribute selectors or child/sibling combinators.
- Only the properties in §8 are honoured. `width`/`border`/`box-sizing` work for plain block and grid/flex-item elements (not for form controls or `<img>`); `height` is not supported at all (boxes are always exactly as tall as their content). No floats, positioning, or text colour/weight. Border is always solid-colored; `border-radius`/`border-style` aren't read.
- Grid supports column and row tracks (px/%/fr for columns; `fr` rows fall back to auto - no definite container height to distribute against), `gap`, row-major auto-placement, named-area placement (`grid-template-areas`/`grid-area`, and the `grid-template` shorthand's area-string form), and explicit line-based placement (`grid-column`/`grid-row`, both axes required together - see §9). Auto-placed items fill gaps left by explicit ones less tightly than real CSS's own algorithm does; a named area's cells aren't checked for forming a proper rectangle (its bounding box is used regardless). Not supported: `justify-*`/`align-*` and subgrid. A bare text node directly inside a grid container is dropped rather than becoming an anonymous item.
- Flex supports `flex-direction` (row/column), `justify-content`, `align-items`, `flex-grow`, and `gap` - see §9's Flex subsection for exactly what each does and doesn't do on each axis. Not spec-accurate for row-direction sizing: an item with neither an explicit `width` nor `flex-grow` gets an equal share of leftover space rather than being sized by its content, since this engine has no min/max-content sizing anywhere to size it by. Not supported at all: `flex-wrap`, `flex-shrink`, `flex-basis`, `align-content`, `align-self`, `order`.
- Radio buttons, file inputs, `<textarea>` (content dropped), and multi-line inputs are not supported. A `<select>` shows only direct `<option>` children (no `<optgroup>`).

**JavaScript**
- Unhandled promise rejections are silent (no rejection tracker), and errors thrown inside `.then` callbacks become rejections, so they are only visible if you add a `.catch`.
- No `fetch`/`XMLHttpRequest`, `localStorage`, `DOMContentLoaded`/`load` events (`window.onload = fn` is accepted but never fired), `element.style`, `element.value`, `innerHTML` getter, `removeEventListener`, `stopPropagation`, or events other than `click`. (`location` itself is supported - §11.)
- ES modules (`type="module"`) are skipped.
- Re-parenting an already-attached node (`appendChild` of an existing element) silently does nothing.
- No `submit` event on forms (§12).

**Engine**
- Page navigation itself no longer blocks the UI thread (`PageLoader`, §5) and gives up after 8s if a fetch is stuck. The networking backend was also replaced (WinINet → Boost.Beast/Asio + OpenSSL, §13) to give connection attempts a short per-address timeout instead of waiting out the OS's own. External script/stylesheet fetches are backgrounded too, and concurrently rather than serially (`ResourceLoader`, §5) - fetching is no longer on the UI thread's critical path anywhere in the engine (confirmed by grepping every `fetchPage`/`fetchBytes` call site).
- **Confirmed real bug, now fixed:** `Fetcher.cpp` had no response decompression at all until this session (§13's `decompressBody`) - found by loading a real site (a corporate site whose stylesheet visibly wasn't applying) and discovering its CDN sends `Content-Encoding: gzip` *unconditionally*, even to a request with no `Accept-Encoding` header. Every response like that was silently fed to the CSS parser/JS engine/image decoder as raw compressed bytes, parsing to near-nothing with no visible error - not a rare edge case, since pre-compressing static assets regardless of client negotiation is a common real-world CDN configuration, not a misconfiguration specific to that one site. Verified end-to-end against the real page: the HTML, both JS bundles, the CSS (866KB decompressed from a 132KB gzip payload), and two SVGs all now decode to valid, readable content.
- **`LayoutRoot::layout()` was the next dominant freeze cause after fetching was fixed - largely fixed itself now.** Diagnostic timing against a real page (Wikipedia's Tiger article, ~1.3MB of HTML, ~20,000 laid-out boxes) showed `doLayout()` taking 5.2s with 135 CSS rules, then 14.4s on the next call with 623 rules applied - both entirely on the UI thread, since layout has to finish before there's anything to paint. Root cause: `CSS::matches` is checked per element against every rule with no selector index (by tag/class/id) - expected to cost roughly elements × rules - but the actual dominant cost turned out to be `classesOf()` (in `CSS.cpp`, used by any class selector) re-splitting the same element's `class=""` attribute string into a fresh `std::vector` from scratch on *every single call*, rather than once per element. Fixed by caching each element's parsed class list for the duration of one layout pass (`CSS::ClassCache`, threaded through `matches`/`compoundMatches` as an optional parameter so `querySelector`'s one-off matching - JSBinding.cpp - is unaffected; `LayoutRoot::classCache`, cleared at the top of every `layout()` since it's only valid within one pass - the DOM doesn't mutate mid-layout). Measured result on the same page: 5.2s → 1.1s and 14.4s → 2.2s, roughly a 5-6x speedup, with no change in visual output (verified against a page exercising every selector form: multi-class AND, tag+class compounds, id, and descendant combinators). The remaining ~1-3s is the *expected* cost of brute-force O(elements × rules) matching with no rule index at all - not yet addressed, and the next thing to look at if page-load time on large real pages still matters.
- **Confirmed real bug, now fixed:** JS ran synchronously on the UI thread with no timeout - a `<script>`, timer callback, or click listener stuck in an infinite loop froze the whole window indefinitely, the same freeze class as the fetch/layout issues above but caused by page JS instead. Fixed with a `JS_SetInterruptHandler`-based watchdog (§11's "Script execution watchdog") that kills anything running past 2s with an uncatchable error; verified against a genuine infinite-loop script, a self-rescheduling infinite `setInterval`, a legitimate ~500ms computation (not falsely killed), and a normal thrown error (unaffected).
- Layout is a full re-layout on every change; no incremental layout.
- The text-texture cache grows without bound.
- Dropdown lists are not clipped to the window and don't scroll.

**Project**
- The source file list lives in two places (`WTEngine.vcxproj` and `CMakeLists.txt`); keep them in sync (§2).
- `runJSEngineSmokeTest()` still runs at every startup.
- The window is created with the title "ToyEngine OpenGL" until the first page sets it.
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
| Fire another kind of event | Model on `dispatchClick` + `js_addEventListener` (currently `click` only) |
| Change drawing | `Renderer` interface, then `OpenGLRenderer` |
| Add a keyboard shortcut | `onKey` in `main.cpp` |
| Change history behaviour | `PageHistory.h`, `visitPage` / `goHistory` in `main.cpp` |

## 19. Worked examples (how a feature touches the code)

These are sketches of the *shape* of a change, so you know which files and functions are involved. They aren't implemented.

### Example A — a simple new JS method: `element.remove()`
Only `JSBinding.cpp` changes.
1. Write `static JSValue js_remove(JSContext*, JSValueConst this_val, int, JSValueConst*)`. Get the element with `unwrapElement(this_val)`, then use its `parent` pointer to find it in `parent->children`.
2. Do what `js_removeChild` does: move the `shared_ptr` into `state->detachedNodes`, set `parent = nullptr`, erase from `children`.
3. Call `markDirty(ctx)` so the next frame re-lays-out.
4. Register it: `JS_CFUNC_DEF("remove", 0, js_remove)` in `js_node_proto_funcs`.

Rules of thumb for any binding: convert strings with `argStr` / `jsStr`; never store a raw `Node*` you don't know is still in the tree; call `markDirty` after any DOM mutation.

### Example B — a new tag's appearance: `<hr>`
`hr` is already parsed (it's a void tag) but it has no children and no background, so layout produces nothing. To draw a line:
1. In `LayoutRoot::layoutElement` (`Layout.cpp`), add a branch next to the `<img>` one: `if (e->tag == L"hr") { flushInline(); … }`.
2. Push a `LayoutBox` with `background = L"#999999"`, `height = 2`, `width = containingWidth`, at the current `y`; advance `y` by the height plus margins.
3. Nothing else is needed — `Engine::render` already paints any box that has a background.

### Example C — a new CSS property: `color`
This one crosses several layers, because `color` is **inherited** (like `font-size`):
1. `Layout.h`: add `std::wstring color` to `ComputedStyle` and `LayoutBox`, and to `InlineItem`.
2. `Layout.cpp`, `computeStyle`: start `sv.color` from an inherited value passed in (thread an `inheritedColor` parameter through `layoutElement`, `collectInline` and `computeStyle`, exactly as `inheritedFontSize` is), and handle `k == L"color"` in `applyDecl`.
3. `appendWords` / `layoutInlineRun`: copy the colour from `InlineItem` into the emitted `LayoutBox`.
4. `Engine::render` (`Engine.cpp`): where text is drawn, use `parseColor(b.color)` when set, otherwise the current black/blue-link default.

Start by making it non-inherited (apply only to an element's own direct text) if you want a quick first version, then add inheritance.

### Example D — a new event type (e.g. `mouseover`, `keydown`)
1. Decide where the native event is caught (`main.cpp` callback) and forward it into `Engine` (like `dispatchClick`).
2. In `JSBinding.cpp`, extend `ListenerStorage` beyond the single `click` map, accept the new name in `js_addEventListener` (it currently rejects everything except `"click"`), and write a dispatcher modelled on `dispatchClick`.
3. Remember JS callbacks must run on the UI thread and can mutate the DOM, so the existing `domDirty` mechanism will handle the re-layout.

### Debugging tips
- `console.log` from a page script prints to stdout and the debugger Output window; script exceptions print as `[script] Error: … <stack>`.
- To see layout, temporarily give every box a background in `Engine::render` (e.g. draw an outline with four thin `drawRect`s per `LayoutBox`); everything is a flat list, so this shows exactly what layout produced.
- Test pages: pass a local `.html` file as the command-line argument (or type its path in the address bar) for fast iteration without the network.
- Most "nothing shows up" bugs are one of: the tag is in the layout skip-list, `display:none` matched, the selector isn't supported (silently skipped by `parseStylesheet`), or the HTML nested wrongly (§7).
