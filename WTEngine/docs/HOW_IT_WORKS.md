# WTEngine — How It Works

A working reference for the current codebase. It describes what the code does today, not what is planned. File and function names are given so you can jump straight to the source.

_Snapshot: latest commit `83e145f` ("Add inline layout, JS timers, window global, and `<select>` dropdowns")._

---

## 1. What it is

WTEngine is a small, from-scratch web browser engine for Windows. It:

1. Loads a page from an `http(s)://` URL or a local file (WinINet).
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
| Networking | WinINet (`InternetOpenUrl`, `HttpSendRequest`) |
| JavaScript | quickjs-ng, vendored in `third_party/quickjs-ng` |
| Platform | Windows, x64 only |

## 2. Building and running

There are two equivalent ways to build. Both produce the same program (x64 only, C++20).

**A. Visual Studio (`WTEngine.sln` → `WTEngine.vcxproj`).** Open the solution, pick **x64** (Debug or Release), build. The vendored quickjs-ng is compiled directly into the project from four C files (`dtoa.c`, `libregexp.c`, `libunicode.c`, `quickjs.c`) as C11 with `/experimental:c11atomics`. **Any new `.cpp` file must be added to the project** (VS: right-click → Add → Existing Item; it lands in the `ClCompile` list in `WTEngine.vcxproj`).

**B. CMake.** `CMakeLists.txt` builds quickjs-ng as the static library `qjs` (its own `CMakeLists.txt`, unmodified) and links `glfw3`, `opengl32`, `wininet` and `qjs`. **Any new `.cpp` file must be added to the `add_executable(WTEngine …)` list.**

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
                     Fetcher (WinINet)                    Engine::render()
                            │ html                               │
                            ▼                                    ▼
                   Engine::loadHTML(html, baseUrl)         Renderer (OpenGL)
                            │                              drawRect / drawText /
        ┌───────────────────┼───────────────────┐          drawImage
        ▼                   ▼                   ▼
   HTMLParser          runScripts()         doLayout()
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
2. Clear, set the viewport, `engine.onResize()`.
3. `renderer.beginFrame()` then `engine.render()` — draws the page.
4. `bar.draw()` — drawn **after** the page so it covers content scrolled under it.
5. Choose the cursor (I-beam over text fields/address bar, hand over links/buttons/nav buttons).
6. Swap buffers, poll events, sleep the remainder of the 16.6 ms budget.

### Navigation
- `navigate(url, postBody?)` → `fetchPage()`; on success `visitPage(finalUrl, html)`, on failure it shows a generated red error page.
- `visitPage` saves the scroll offset of the page being left, pushes a new `HistoryEntry`, then `showEntry`.
- `showEntry` calls `engine.loadHTML(html, url)`, restores the scroll position, and updates the window title and address bar. (The window title is the URL — `<title>` content is discarded by the parser.)
- `goHistory(±1)` re-displays a stored entry **from its saved HTML** — no network request, and a POSTed result is not re-sent.

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
- **Stylesheets:** all `<style>` text is concatenated and parsed once into `Document::styles`.

**Important simplification:** an end tag closes the *current* element regardless of its name (`endName` is read but never compared). There are no implied end tags either (an unclosed `<p>`, `<li>` etc. swallows following siblings until some end tag appears). Well-formed pages work; sloppy ones will nest oddly.

## 8. CSS (`CSS.h`, `CSS.cpp`)

**Parsing** (`parseStylesheet`): strips `/* */` comments, drops every `@`-rule (so `@media`, `@import`, `@font-face` never apply), splits comma groups, and produces one `Rule` per selector with `chain`, `declarations`, `specificity` and source `order`.

**Selectors supported:** `tag`, `.class`, `#id`, `*`, compounds like `div.card#x`, and the **descendant** combinator (`a b`, any depth). **Not supported** (the whole selector is skipped): `>`, `+`, `~`, `[attr]`, `:pseudo`.

**Matching** (`CSS::matches`): the last compound must match the element; earlier compounds must match *some* ancestor in order, right to left.

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
| `font-size` | px, `em`, `rem`, `%` (relative to the inherited size); inherited by children |
| `display` | `none`, `block`, `inline`, `inline-block` (treated as inline) |

Everything else (`color`, `height`, `float`, `position`, flex/grid, `font-weight`, …) is parsed and ignored. Text is always black; links are always blue. `em`/`rem` aren't supported for `width`/`margin`/`padding`/`border-width` (only `font-size` resolves those) - use px or `%`.

`CSS::parseSelector` and `CSS::matches` are also reused by JS `querySelector`.

## 9. Layout (`Layout.h`, `Layout.cpp`)

`LayoutRoot::layout()` starts at `<body>` at `(10, 10)` with width `viewportWidth − 20` and walks the DOM once, appending to `boxes`.

### `LayoutBox`
`x, y, width, height` (document space), plus optional `background`, `borderWidth`/`borderColor`, `text`, `href`, `imageSrc`, `fontSize`, `control` (`TextField | Button | Checkbox | Select`), `el` (source element) and `form` (enclosing `<form>`). One struct serves as a background/border rectangle, a single word of text, an image, or a form control.

### The box model (`ComputedStyle`, `layoutElement`'s block branch)
`ComputedStyle` carries the full box model for an ordinary block element: `marginTop/Right/Bottom/Left`, `paddingTop/Right/Bottom/Left`, `width` (`-1` = auto), `borderWidth`/`borderColor`, and `boxSizing` (`ContentBox` | `BorderBox`). `layoutElement`'s block branch turns these into two widths before laying out children:

- **`outerWidth`** - what actually gets painted (the border/background edge). With `width` unset, it's `containingWidth - marginLeft - marginRight`, same as always. With `width` set: under `content-box` (the default), `width` names the *content* box, so outer = `width + padding + 2×border`; under `border-box`, `width` already *is* the outer size.
- **`contentWidth`** - `outerWidth` minus padding and border, and what children are actually laid out into (`layoutElement(e, boxX + border + paddingLeft, y, contentWidth, ...)`).

A background/border box is reserved (as today) before recursing into children, sized to `outerWidth`, with its height patched in afterwards once the children's natural height is known. This still means **explicit CSS `height` isn't supported** - a box is always exactly as tall as its content, `overflow: visible`-style clipping/`height` isn't modeled. `width`/`border`/`box-sizing` currently apply to plain block elements only, not to `<input>`/`<button>`/`<select>` (§`layoutControl`, unchanged) or `<img>` (§`layoutImage`, unchanged) - those keep their own fixed/intrinsic sizing.

`Engine::render` paints a border as four thin rects forming a hollow frame (not one filled rect), so a border with no background still lets whatever's behind the box show through the middle, then paints the background inset by the border width.

### Algorithm (`layoutElement`)
For each child:
- **Text node** → split into words and buffered in `pendingInline`.
- **Skipped entirely:** `head script style title meta link base`, and anything whose computed `display` is `none`.
- **`<br>`** → a forced-break item in the inline buffer.
- **`<input>`, `<button>`, `<select>`** → flush inline, then `layoutControl`.
- **`<img>`** → flush inline, then `layoutImage`.
- **Inline element** (`a span b strong i em u small code sub sup mark label abbr cite q`, or `display:inline`) → `collectInline` flattens its text and nested inline children into the same run. `<a href>` sets `currentHref` for its words.
- **Block element** → flush inline, resolve the box model above, add `margin-top`, reserve a background/border box if it has either (height patched afterwards), add `border` + `padding-top`, recurse, add `padding-bottom` + `border` and `margin-bottom`. `<form>` sets `currentForm` for the subtree.

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
1. `parseAndBuild` — clears focus/submission/dropdown state, parses, points `layoutRoot` at the new body and rules.
2. `runScripts` — see §11.
3. `doLayout`.

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

**Teardown order matters.** `Engine::runScripts` resets `domState` first, then destroys the old `JSEngine`, then creates the new one. `domState` owns `JSValue`s (listeners, timers) that must be freed against a runtime that still exists; quickjs asserts (Debug) or corrupts memory (Release) if a runtime is destroyed while any value is alive. Keep that order if you touch it. The same rule is why `domState` is declared after `jsEngine` in `Engine.h` (members destruct in reverse).

### Promises and microtasks
quickjs never runs promise jobs on its own; the host must pump them. `runPendingJobs(ctx)` (`JSEngine.cpp`) drains the job queue, like a browser's microtask checkpoint. It runs after every script's `eval`, after each click listener, and after each timer callback, so `Promise.then` and `async`/`await` continuations behave in the expected order (`sync code → promise callbacks → timers`). A job that throws is printed as `[promise] Error: …` and the rest still run. **Any new place that calls into JS (`JS_Call`, `JS_Eval`) should call `runPendingJobs` afterwards.** Unhandled promise rejections are not reported (no rejection tracker is installed).

### Script execution (`Engine::runScripts`)
- Runs **after the entire DOM is built**, in document order. So no `document.write`, and scripts can see the whole page.
- Handles inline scripts and `src=` scripts. External scripts are fetched **synchronously** through `fetchPage`, blocking the UI.
- Only "classic" scripts run: no `type`, or `text/javascript`, `application/javascript`, `application/ecmascript`. `type="module"`, JSON-LD, templates etc. are skipped.
- An exception is printed as `[script] Error: …` and execution continues with the next script.

### What JS can see
Everything is installed by `installDOMBindings`.

**Globals:** `document` (a wrapper around `<body>`), `window` (an alias of the global object), `console`, `setTimeout`, `setInterval`, `clearTimeout`, `clearInterval`.

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
- **Click:** `dispatchClick` walks from the hit element up through `parent`, calling every registered listener at each level (bubbling). `preventDefault()` suppresses link navigation, and for form controls it cancels the control's action (checkbox toggle, form submit, dropdown open). `stopPropagation` does not exist; exceptions in listeners are swallowed.
- **Timers:** stored with an absolute due time on the same clock as `render()`'s `timeSeconds` (`glfwGetTime`). `fireDueTimers` runs once per frame, so timer resolution is one frame (~16 ms). Callbacks are looked up by id right before being called, so a timer can safely clear itself or others. New timers scheduled inside a callback wait until the next frame. Intervals resync to "now" instead of catching up on missed ticks.
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

| Function | Purpose |
|---|---|
| `fetchPage(url, postBody?)` | HTML text. http(s) via WinINet (`GET` with `Accept: text/html`, or `POST`); anything else is treated as a local file path. Status ≥ 400 → error. Decodes UTF-8 (BOM stripped). Reports the final URL after redirects |
| `fetchBytes(url, out)` | Raw bytes for images: http(s), local file, or `data:…;base64,…` URI |
| `resolveUrl(base, href)` | Makes an absolute URL via `InternetCombineUrl`. Returns `""` for fragments (`#…`), non-http schemes (`javascript:`, `mailto:`, …), and relative links from a local file |
| `urlEncodeForm(text)` | UTF-8 percent-encoding, space → `+` |
| `withQuery(url, query)` | Replaces the query string and fragment |

The user agent is `WTEngine/0.1`. **All page and script fetches are synchronous on the UI thread**; only images are asynchronous.

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
- No external stylesheets (`<link rel=stylesheet>` is ignored), no `@media`, no pseudo-classes, attribute selectors or child/sibling combinators.
- Only the properties in §8 are honoured. `width`/`border`/`box-sizing` work for plain block elements (not for form controls or `<img>`); `height` is not supported at all (boxes are always exactly as tall as their content). No floats, positioning, flex/grid, or text colour/weight. Border is always solid-colored; `border-radius`/`border-style` aren't read.
- Radio buttons, file inputs, `<textarea>` (content dropped), and multi-line inputs are not supported. A `<select>` shows only direct `<option>` children (no `<optgroup>`).

**JavaScript**
- Unhandled promise rejections are silent (no rejection tracker), and errors thrown inside `.then` callbacks become rejections, so they are only visible if you add a `.catch`.
- No `fetch`/`XMLHttpRequest`, `localStorage`, `location`, `DOMContentLoaded`/`load` events (`window.onload = fn` is accepted but never fired), `element.style`, `element.value`, `innerHTML` getter, `removeEventListener`, `stopPropagation`, or events other than `click`.
- ES modules (`type="module"`) are skipped.
- Re-parenting an already-attached node (`appendChild` of an existing element) silently does nothing.
- No `submit` event on forms (§12).

**Engine**
- Page navigation and external script fetches block the UI thread.
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
