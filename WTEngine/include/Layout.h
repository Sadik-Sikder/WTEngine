// Layout.h
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include "DOM.h"
#include "CSS.h"
#include "Renderer.h" // Color, Gradient

// The CSS custom properties (--name: value) in effect on an element: the
// ones it defines itself, with any var() in them already substituted, plus
// (through `parent`) everything it inherits. Layered rather than copied,
// since pages commonly define hundreds on :root and a few more on many
// elements below it.
struct CSSVars {
    std::unordered_map<std::wstring, std::wstring> own;
    std::shared_ptr<const CSSVars> parent;
    const std::wstring* find(const std::wstring& name) const {
        for (const CSSVars* s = this; s; s = s->parent.get()) {
            auto it = s->own.find(name);
            if (it != s->own.end()) return &it->second;
        }
        return nullptr;
    }
};

// A background-size/-position length: pixels, a percentage, or auto.
struct BgLength {
    float value = 0;
    bool percent = false;
    bool isAuto = false;
};

// One layer of `background`/`background-image`: an image or a gradient,
// with how it's sized, placed and repeated. Painted inside the border.
struct BackgroundLayer {
    std::wstring image; // url(...) target as written (resolved when drawn); empty for a gradient
    std::shared_ptr<const Gradient> gradient;
    enum class Size { Auto, Cover, Contain, Explicit } size = Size::Auto;
    BgLength width{ 0, false, true }, height{ 0, false, true }; // Size::Explicit
    BgLength posX{ 0, true, false }, posY{ 0, true, false };    // 0% 0%: the top left
    bool repeatX = true, repeatY = true;
};

// A length that may be `auto` - for top/right/bottom/left. `value` is in
// pixels, or a percentage when `percent` is set.
struct Len {
    float value = 0;
    bool percent = false;
    bool isAuto = true;
};

// One box-shadow. Inset shadows are parsed but not drawn.
struct BoxShadow {
    float x = 0, y = 0, blur = 0, spread = 0;
    Color color{ 0, 0, 0, 1 };
    bool inset = false;
};

// Simple layout box result
struct LayoutBox {
    // Interactive form controls get a box of their own; the Engine draws
    // and handles them using `el`.
    enum Control { NoControl, TextField, Button, Checkbox, Select };

    int x, y, width, height;
    std::wstring background; // e.g. "#rrggbb"
    std::wstring text; // text inside (for leaf text-only boxes); a Button's label
    std::wstring href; // link target if this text is inside an <a href>
    std::wstring imageSrc; // <img>'s raw (unresolved) src attribute; empty for non-image boxes
    int fontSize = 14;
    // Text colour (any CSS color string - see parseColor; empty = the default
    // black) and weight, both inherited like font-size. Links get their blue
    // from computeStyle as a default, so an author rule can override it.
    std::wstring color;
    bool bold = false;
    bool italic = false;
    const std::wstring* family = nullptr; // installed font face; nullptr = the default (see TextPaint)
    // text-decoration, drawn across `decorationWidth` - the word, plus the
    // space after it when the next word on the line is decorated the same
    // way, so an underlined phrase gets one continuous line.
    bool underline = false;
    bool lineThrough = false;
    int decorationWidth = 0;

    // A border, drawn as a hollow frame (see Engine::render) so an
    // unset `background` still shows whatever's behind the box through
    // the middle - matching a real CSS border, which never fills its box.
    int borderWidth = 0;
    std::wstring borderColor;

    // border-radius, per corner: top-left, top-right, bottom-right,
    // bottom-left. A percentage is of the box's smaller side - so 50% makes
    // a square a circle, but a wide box a pill rather than CSS's ellipse
    // (corners are always circular here). Resolved when painting, once the
    // box's final size is known (see cornerRadii).
    struct CornerRadius { float value = 0; bool percent = false; };
    CornerRadius radius[4];
    // Background images/gradients, first on top (as listed in CSS), and
    // box shadows, first on top too.
    std::vector<BackgroundLayer> backgrounds;
    std::vector<BoxShadow> shadows;

    // From positioning and overflow (see LayoutRoot::layoutBlockChild):
    // - paintKey: boxes are stably sorted by it after layout, so it is the
    //   paint order (and the reverse hit-test order). 0 = normal flow; a
    //   positioned element's boxes get 2 * z-index + 1 (z-index auto = 0),
    //   so they paint above the flow at the same z, as in CSS.
    // - fixed: position: fixed - `y` is relative to the top of the
    //   viewport, so the box stays put while the page scrolls.
    // - sticky: an index into LayoutRoot::stickies, whose shift for the
    //   current scroll position is added to `y` (see Engine::boxShift).
    // - clipped: drawn and hit-tested only inside clip* (document
    //   coordinates) - an ancestor's overflow other than visible.
    int paintKey = 0;
    bool fixed = false;
    int sticky = -1;
    bool clipped = false;
    int clipX = 0, clipY = 0, clipW = 0, clipH = 0;

    bool rounded() const {
        for (const auto& r : radius) if (r.value > 0) return true;
        return false;
    }
    // The four radii in pixels, scaled down together where adjacent
    // corners would overlap along a side, as CSS does.
    void cornerRadii(float out[4]) const;

    Control control = NoControl;
    // The element this box was generated from: a control's own element,
    // the element a background/image box belongs to, or (for a text box)
    // the direct parent element of that text. Used for click hit-testing
    // (Engine::dispatchClick) so addEventListener('click', ...) can find
    // which element - and its ancestors, for bubbling - a click landed on.
    Element* el = nullptr;
    Element* form = nullptr; // the enclosing <form>, if any

    // opacity:0 / visibility:hidden (own or inherited from an ancestor):
    // still occupies its layout position (x/y/width/height above are real),
    // just skipped by Engine::render's paint loop. Click hit-testing is
    // deliberately unaffected - see Engine::render's comment.
    bool visuallyHidden = false;
};

struct LayoutRoot {
    int viewportWidth = 800;
    int viewportHeight = 600; // for vh units, height: %, and the containing block of fixed/root-level absolute boxes
    // Whether the last layout depended on viewportHeight - so the engine
    // knows a change of window height needs a relayout (a width change
    // always does).
    bool usedViewportHeight = false;

    // position: sticky elements, referred to by LayoutBox::sticky. At a
    // scroll position, one's boxes shift down by scrollY + top - naturalTop,
    // kept between 0 and maxShift (where it meets its parent's bottom).
    struct Sticky {
        int top = 0;
        int naturalTop = 0;
        int maxShift = 0;
    };
    std::vector<Sticky> stickies;
    Document* doc = nullptr;
    Node* rootNode = nullptr;
    std::vector<LayoutBox> boxes;
    std::wstring currentHref; // href of the enclosing <a>, set during layout
    Element* currentForm = nullptr; // the enclosing <form>, set during layout
    const std::vector<CSS::Rule>* rules = nullptr; // from <style> blocks on the page
    const CSS::HoverSet* hover = nullptr; // what :hover matches this pass; null = nothing hovered

    // Returns the pixel width of `text` at `fontSize`. Used to wrap text; a
    // rough per-character estimate is used when unset.
    std::function<float(const std::wstring&, int, bool bold, bool italic, const std::wstring* family)> measureText;

    // Resolves an <img>'s raw `src`, fetching/decoding (and caching) it if
    // not already cached, and returns its natural pixel size via (outW,
    // outH). Used to size a box that doesn't give explicit width/height.
    // Returns false if unset or the fetch/decode fails.
    std::function<bool(const std::wstring& src, int& outW, int& outH)> loadImage;

    void layout(); // compute boxes from rootNode
private:
    // The inherited properties besides font-size: `color` (a raw CSS color
    // string, empty = default black), bold (font-weight), and custom
    // properties. Passed down the tree alongside inheritedFontSize, exactly
    // the same way. `vars` is shared, not copied, between an element and
    // the descendants that don't change any custom property - a new layer
    // is only made where one does.
    enum class TextAlign { Left, Center, Right };
    enum class TextTransform { None, Uppercase, Lowercase, Capitalize };
    struct TextPaint {
        std::wstring color;
        bool bold = false;
        std::shared_ptr<const CSSVars> vars;
        bool italic = false;
        // The installed font face font-family resolved to (see resolveFontFamily),
        // or nullptr for the default. Points into a set of interned names, so
        // copying it per word costs nothing.
        const std::wstring* family = nullptr;
        TextAlign align = TextAlign::Left;
        // line-height: a multiple of the font size (lineHeight >= 0), a fixed
        // pixel height (lineHeightPx >= 0), or neither for "normal".
        float lineHeight = -1;
        int lineHeightPx = -1;
        // text-decoration. Not inherited in CSS, but an ancestor's decoration
        // is drawn through its descendants' text, which comes to the same.
        bool underline = false;
        bool lineThrough = false;
        TextTransform transform = TextTransform::None;
    };
    // The height a line of `fontSize` text needs under this line-height.
    static int lineBand(const TextPaint& p, int fontSize);

    // `inheritedFontSize` is the size to use for `el`'s own direct text, and
    // the default for its children's font-size unless they set their own -
    // i.e. plain CSS inheritance, not reset to a hardcoded size each level.
    // `inheritedPaint` works the same way for color/font-weight.
    void layoutElement(Element* el, int x, int& y, int containingWidth, int inheritedFontSize,
                        bool inheritedVisuallyHidden, const TextPaint& inheritedPaint);
    std::wstring getAttr(Element* el, const std::wstring& key, const std::wstring& def = L"");
    int parseFontSize(const std::wstring& s, int def = 14);
    int resolveFontSize(const std::wstring& s, int baseFontSize, int def);
    // Same unit handling as resolveFontSize (px, or a percentage), but for
    // width/height/border/margin/padding, where a percentage resolves
    // against the containing block's width, not the font size - the same
    // base CSS itself uses for percentage margin/padding on every side,
    // including top/bottom (a well-known CSS quirk, not a bug here).
    // em/rem aren't supported for these properties (unlike font-size,
    // there's no single obviously-right base to multiply), and fall back
    // to `def` like any other unrecognized unit.
    int resolveLength(const std::wstring& s, int base, int def);
    float textWidth(const std::wstring& text, int fontSize, bool bold = false, bool italic = false,
                    const std::wstring* family = nullptr);
    float textWidth(const std::wstring& text, int fontSize, const TextPaint& p) {
        return textWidth(text, fontSize, p.bold, p.italic, p.family);
    }

    // One word of flowing inline content (from a text node, or from an
    // inline-level element like <a>/<b> flattened into its container's
    // run - see collectInline), tagged with the style it should render
    // with. isBreak marks a <br>: a forced line break with no text of its
    // own.
    struct InlineItem {
        std::wstring word;
        int fontSize = 14;
        std::wstring href;
        Element* owner = nullptr;
        bool isBreak = false;
        bool visuallyHidden = false;
        TextPaint paint;
        // An <img>/<svg> instead of a word: its box, already sized
        // (makeImageBox), for layoutInlineRun to place. `marginLeft`/
        // `marginRight` add space around it within the line.
        std::shared_ptr<const LayoutBox> image;
        int marginLeft = 0, marginRight = 0;
        // Whether whitespace separated this item from the one before it in
        // the HTML - only then does it get a space ("<em>x</em>," has none).
        bool spaceBefore = true;
        // A marker for whitespace with no word after it in its text node
        // ("Hello " or a lone " " between elements): the next item, from
        // wherever it comes, then gets a space before it.
        bool isSpace = false;
    };
    // Splits `text` on whitespace, appending one InlineItem per word.
    void appendWords(const std::wstring& text, int fontSize, const std::wstring& href,
                      Element* owner, std::vector<InlineItem>& out, bool visuallyHidden,
                      const TextPaint& paint);
    void collectInline(Element* el, int inheritedFontSize, int containingWidth, std::vector<InlineItem>& out,
                        bool inheritedVisuallyHidden, const TextPaint& inheritedPaint);
    // `align` is the text-align of the block the run belongs to.
    void layoutInlineRun(const std::vector<InlineItem>& items, int x, int& y, int containingWidth,
                         TextAlign align = TextAlign::Left);

    // The subset of an element's cascaded style that layout cares about -
    // computed once per element and shared by both plain elements and form
    // controls, so both respond to the same CSS/inline styling.
    enum class Display { Block, Inline, None, Grid, Flex };
    enum class FlexDirection { Row, Column };
    enum class FlexWrap { NoWrap, Wrap, WrapReverse };
    enum class JustifyContent { FlexStart, Center, FlexEnd, SpaceBetween, SpaceAround };
    // Only Stretch (the default) and FlexStart/Center/FlexEnd's cross-axis
    // *positioning* are supported - see layoutFlex's comment for exactly
    // what each does (and doesn't) on each axis.
    enum class AlignItems { Stretch, FlexStart, Center, FlexEnd };
    // Only meaningful with an explicit `width` set; ContentBox (the CSS
    // default) means `width` names the content box, so padding/border add
    // to it - BorderBox means `width` already includes them. See
    // layoutElement's box-model math for how each is turned into an
    // outer/content width.
    enum class BoxSizing { ContentBox, BorderBox };
    // One track of a `grid-template-columns` list: either a fixed pixel
    // width, or a share of whatever width is left after every fixed track
    // and gap is subtracted (a "fr" unit - e.g. "1fr 2fr" splits the
    // remainder 1:2).
    struct GridTrack {
        bool isFr;
        float value;
    };
    struct ComputedStyle {
        std::wstring background;
        int marginTop = 6, marginBottom = 6, marginLeft = 0, marginRight = 0;
        int paddingTop = 6, paddingRight = 6, paddingBottom = 6, paddingLeft = 6;
        int width = -1; // -1 = auto: fill the container, same as if unset (today's only behavior)
        // min-/max-width in px (in the same box as `width`, per box-sizing),
        // -1 for none.
        int minWidth = -1, maxWidth = -1;
        // margin-left/-right: auto - a block narrower than its container
        // takes the leftover space there (both: centred).
        bool marginLeftAuto = false, marginRightAuto = false;
        // The used height in px (content box, or border box with
        // box-sizing: border-box), or -1 for auto. A percentage resolves
        // against the containing block's height when that is definite
        // (LayoutRoot::containingHeight_), else it's auto, as in CSS.
        int height = -1;
        int borderWidth = 0;
        std::wstring borderColor;
        LayoutBox::CornerRadius radius[4]; // border-radius - see LayoutBox
        std::vector<BackgroundLayer> backgrounds; // see LayoutBox
        std::vector<BoxShadow> shadows;
        // Whether this element's own rules set color / a font property
        // (including `inherit`). Form controls only use those - like
        // browsers, whose built-in styles stop controls inheriting the
        // page's text colour and font.
        bool colorSet = false;
        bool fontSet = false;
        // Positioning. top/right/bottom/left as written (percentages of the
        // containing block); z-index with zAuto when it's `auto`.
        enum class Position { Static, Relative, Absolute, Fixed, Sticky } position = Position::Static;
        Len top, right, bottom, left;
        int zIndex = 0;
        bool zAuto = true;
        // overflow other than visible, per axis: the box clips its
        // descendants along it. (hidden/clip/auto/scroll all clip - there's
        // no scrolling inside an element yet.)
        bool clipX = false, clipY = false;
        // min-/max-height in px, -1 for none/auto (resolved like height).
        int minHeight = -1, maxHeight = -1;
        BoxSizing boxSizing = BoxSizing::ContentBox;
        int fontSize = 14;
        Display display = Display::Block;
        // Only meaningful with display:grid. Empty means no explicit
        // grid-template-columns was set - layoutGrid then falls back to one
        // full-width column, so items still stack rather than disappear.
        std::vector<GridTrack> gridTemplateColumns;
        // Only meaningful with display:grid. Empty means every row is
        // auto-sized (tallest item in it) - unlike columns, there's no
        // "one row" fallback needed, since rows already grow as items are
        // placed into them. A track's `isFr` is ignored for rows (treated
        // as auto/unset) - fr needs a definite container height to
        // distribute against, and this engine's pages are always
        // auto-height (see layoutFlex's column-direction comment for the
        // same limitation).
        std::vector<GridTrack> gridTemplateRows;
        // Only meaningful with display:grid. One entry per row, each a list
        // of column cell names for that row ("." means no area, same as
        // real CSS) - e.g. `"header header" "sidebar main"` parses to
        // {{"header","header"}, {"sidebar","main"}}. Empty means no named
        // areas are in use.
        std::vector<std::vector<std::wstring>> gridTemplateAreas;
        int rowGap = 0, columnGap = 0; // from `gap`/`row-gap`/`column-gap` - shared by grid and flex, same properties either way
        // Only meaningful on a grid item (read from the item's own style by
        // its container's layoutGrid). 1-based CSS grid line numbers, as
        // authored; 0 means unset. An item needs *both* its column and row
        // set to be explicitly placed - one set without the other is
        // treated as fully automatic instead of partially placed (see
        // layoutGrid's comment on why).
        int gridColumnStart = 0, gridColumnEnd = 0;
        int gridRowStart = 0, gridRowEnd = 0;
        // Only meaningful on a grid item. The area name from `grid-area:
        // <name>` - empty means unset. Only the named-area form is
        // supported, not grid-area's alternate 4-value line-based syntax
        // (`grid-area: <row-start> / <col-start> / <row-end> / <col-end>`) -
        // use grid-column/grid-row for that instead.
        std::wstring gridArea;
        // Only meaningful with display:flex, on the container.
        FlexDirection flexDirection = FlexDirection::Row;
        JustifyContent justifyContent = JustifyContent::FlexStart;
        AlignItems alignItems = AlignItems::Stretch;
        // Only meaningful on a flex item (read from the item's own style by
        // its container's layoutFlex, not used by the item's own layout).
        // 0 (unset) is the real CSS default too - an item only grows if
        // this is explicitly positive.
        float flexGrow = 0;
        bool flexGrowSet = false; // distinguishes an explicit flex-grow:0 (or flex:none) from unset
        // How much of a line's overflow this item gives up, weighted by
        // its basis (the spec's "scaled shrink factor"). 1 is the CSS default.
        float flexShrink = 1;
        // Outer px, or -1 for auto (fall back to `width`). Set by
        // flex-basis or the `flex` shorthand (`flex: 1` means a basis of 0).
        int flexBasis = -1;
        // Only meaningful with display:flex, on the container.
        FlexWrap flexWrap = FlexWrap::NoWrap;
        // Raw `opacity`/`visibility` as authored, plus the resolved,
        // inheritance-aware flag layout code actually checks (own opacity
        // <= 0, own visibility:hidden, or an ancestor already hidden this
        // way). Distinct from Display::None: a visually-hidden element
        // still occupies its normal layout space, just isn't painted - see
        // layoutElement's/Engine::render's comments.
        float opacity = 1.0f;
        bool visibilityHidden = false;
        bool visuallyHidden = false;
        // Resolved color/font-weight: inherited unless this element's own
        // rules (or its tag's default - <a href> is blue, <b>/<strong>/<th>/
        // headings are bold) set them.
        TextPaint paint;
    };
    ComputedStyle computeStyle(Element* e, int inheritedFontSize, int containingWidth,
                                bool inheritedVisuallyHidden, const TextPaint& inheritedPaint);
    // Parses one corner radius: px, %, em/rem (against `fontSize`), or 0.
    static bool parseRadius(const std::wstring& v, int fontSize, LayoutBox::CornerRadius& out);
    // Splits a shorthand value like "4px 8px" on whitespace and expands it
    // to all four sides per CSS's 1/2/3/4-value shorthand rule (used for
    // both `margin` and `padding`). Missing tokens, or a value that fails
    // to parse, fall back to `def`.
    void parseBoxShorthand(const std::wstring& v, int containingWidth, int def,
                            int& top, int& right, int& bottom, int& left);
    // Parses a grid-template-columns/grid-template-rows value (the track
    // grammar is identical for both axes, so one parser serves both - see
    // the .cpp for exactly what's supported: px/%/fr tracks and
    // repeat(N, <track>); an unsupported keyword like auto or minmax()
    // becomes 1fr, so the track *count* an author wrote is always honored
    // even where the sizing isn't).
    std::vector<GridTrack> parseGridTemplateTracks(const std::wstring& v, int containingWidth);
    // Parses a grid-column/grid-row shorthand value - "2" (start, span 1),
    // "2 / 4" (start/end line numbers), or "2 / span 3" (start + span) -
    // into 1-based start/end line numbers. Returns false (leaving start/
    // end untouched) for anything else: named lines, negative/from-the-end
    // indices, and a bare "span N" with no start aren't supported.
    bool parseGridLinePlacement(const std::wstring& v, int& start, int& end);
    // Parses a grid-template-areas value - one or more quoted strings, each
    // one grid row, each whitespace-separated token in it one column's area
    // name ("." means no area). Returns an empty grid (not a partial one)
    // for anything malformed: an unterminated quoted string, or rows with
    // different column counts - real CSS requires every row to name the
    // same number of columns, and this engine does too, just by rejecting
    // the whole thing rather than trying to reconcile mismatched rows.
    std::vector<std::vector<std::wstring>> parseGridTemplateAreas(const std::wstring& v);
    // Parses the grid-template shorthand's area-string form - e.g.
    // `"header header" 40px "sidebar main" 1fr / 100px 1fr` - into areas
    // (via parseGridTemplateAreas), row tracks (one per area-row, an
    // optional track size token right after its closing quote - see the
    // .cpp for how "no size given" is distinguished from "explicit 0"),
    // and column tracks (the part after the top-level '/', via
    // parseGridTemplateTracks). Only this form is supported, not the
    // plainer "<rows> / <columns>" form with no area strings at all - use
    // the grid-template-rows/columns longhands for that instead.
    void parseGridTemplateShorthand(const std::wstring& v, int containingWidth,
                                     std::vector<std::vector<std::wstring>>& areas,
                                     std::vector<GridTrack>& rowTracks, std::vector<GridTrack>& colTracks);
    void layoutControl(Element* el, int x, int& y, int containingWidth, const ComputedStyle& style);
    void layoutImage(Element* el, int x, int& y, int containingWidth, const ComputedStyle& style);
    LayoutBox makeImageBox(Element* el, int containingWidth, const ComputedStyle& style);
    // Adds an <img>/<svg> to an inline run, as one item that wraps like a word.
    void appendImage(Element* el, int containingWidth, const ComputedStyle& style, std::vector<InlineItem>& out);
    // The box-model + content treatment layoutElement gives any block-level
    // child (background/border box, margin/padding, then recurse into
    // children or - if `style.display` is Grid - into layoutGrid). Factored
    // out so layoutGrid can give each grid item the exact same treatment,
    // for one specific element, without duplicating the box-model math.
    void layoutBlockChild(Element* e, int x, int& y, int containingWidth, const ComputedStyle& style);
    // Places `el`'s children into a grid instead of flowing them vertically.
    // See the .cpp for the full algorithm; in short:
    // 1. Resolves grid-template-columns into pixel column widths, same as
    //    before - padded with implicit 1fr tracks first if
    //    grid-template-areas names more columns than it has tracks for.
    // 2. An item with grid-area set, naming an area that appears in
    //    grid-template-areas, is placed at that name's bounding box (every
    //    cell the name appears in - real CSS requires those cells to form
    //    a rectangle; this doesn't specially validate that, it just takes
    //    the bounding box regardless). Otherwise, an item with *both*
    //    grid-column and grid-row set is placed into those exact cells
    //    (clamped to the template's column count - a line beyond it
    //    doesn't create an implicit column). One axis set without the
    //    other, or a grid-area naming nothing in the template, is treated
    //    as fully automatic, not partially placed - a deliberate
    //    simplification, not an oversight.
    // 3. Every other item auto-places row-major (grid-auto-flow: row, the
    //    CSS default), walking past any cell an explicit item already
    //    claimed. This is simpler than real CSS's own auto-placement
    //    (which packs more tightly around explicit items) and can leave a
    //    gap a "dense" packing algorithm would have filled instead.
    // 4. Row heights: an explicit grid-template-rows track wins if set
    //    (fr tracks excepted - see its ComputedStyle comment); otherwise a
    //    row is as tall as the tallest single-row item placed in it. An
    //    item spanning multiple rows bumps the *last* row it spans if the
    //    rows it's already in aren't tall enough for it, rather than
    //    distributing the difference across all of them.
    void layoutGrid(Element* el, int x, int& y, int containingWidth, const ComputedStyle& style);
    // Places `el`'s children along style.flexDirection's main axis. See the
    // .cpp for the full explanation of what's supported and why (row vs.
    // column are different enough to be effectively two algorithms):
    // row-direction items without an explicit width share leftover space
    // by an implicit flex-grow:1 (or their real flex-grow, if set) - not
    // spec-accurate (real flexbox sizes them by content) but avoids
    // collapsing to zero, the same tradeoff layoutGrid already makes for
    // an untemplated grid. Column-direction items are normal block
    // children stacked vertically; justify-content has no effect there,
    // since a flex container with no definite height (this engine's pages
    // are always "auto" height) has no leftover space to distribute along
    // that axis - the same behavior real CSS shows for an auto-height
    // flex column, not a shortcut unique to this engine.
    void layoutFlex(Element* el, int x, int& y, int containingWidth, const ComputedStyle& style);
    // Lays out one flex/grid item (a control, an image, or a block) at
    // local origin (0, 0) and `width`, returning its boxes instead of
    // appending them to `boxes`; `height` receives how tall it came out.
    std::vector<LayoutBox> layoutItemDetached(Element* item, int width, const ComputedStyle& style, int& height);
    // A shrink-to-fit width for an item with no width of its own: lays it
    // out at `available` and measures how far right its content (text,
    // images, controls) actually reaches, plus its right padding/border/
    // margin. Stands in for the max-content size this engine otherwise has
    // no notion of; capped at `available`.
    int shrinkToFitWidth(Element* item, const ComputedStyle& style, int available);

    // Ancestors of the element layoutElement is currently iterating the
    // children of (root first); used to match descendant selectors ("a b").
    std::vector<Element*> ancestorStack;

    // Every element's parsed class list, computed at most once per layout()
    // call and reused across every rule that tests it - see CSS::ClassCache
    // and computeStyle. Cleared at the start of layout(): safe to reuse
    // within one pass (the DOM doesn't mutate mid-layout) but not across
    // passes, since a page's own class="" attributes can change between them.
    CSS::ClassCache classCache;

    // Rules bucketed by what their rightmost compound requires - an id,
    // else its first class, else its tag, else nothing (universal) - so
    // computeStyle only tests an element against rules that could match
    // it, instead of every rule on the page. Each rule sits in exactly one
    // bucket. Rebuilt at the start of every layout() - cheap next to the
    // layout itself, and it can't go stale when a stylesheet arrives or
    // the page changes.
    struct RuleIndex {
        std::unordered_map<std::wstring, std::vector<const CSS::Rule*>> byId, byClass, byTag;
        std::vector<const CSS::Rule*> universal;
    } ruleIndex;
    void rebuildRuleIndex();

    // --- Height, positioning, overflow ---------------------------------
    // The content height of the block being laid out into, when it's
    // definite (an explicit height, or the viewport at the root) - what
    // height: % resolves against; -1 when it depends on the content.
    int containingHeight_ = -1;
    // Lengths that resolve against a height: px, %, vh/vw/vmin/vmax, em.
    int resolveHeight(const std::wstring& v, int fontSize, int def);

    // Absolute and fixed elements are laid out after their containing
    // block - the nearest positioned ancestor's padding box, or the
    // viewport - has its final size, so right/bottom work. Each is
    // recorded where it's met in the flow (its static position, used when
    // it gives no top/left) along with the context its own descendants
    // need: the ancestors their selectors match against, and the
    // enclosing link/form.
    struct OutOfFlow {
        Element* el = nullptr;
        ComputedStyle style;
        int staticX = 0, staticY = 0;
        bool hasStatic = true;
        std::vector<Element*> ancestors;
        std::wstring href;
        Element* form = nullptr;
    };
    struct ContainingBlock {
        std::vector<OutOfFlow> pending;
    };
    // Innermost positioned ancestor last; [0] is the viewport.
    std::vector<ContainingBlock*> positioned_;
    void deferOutOfFlow(Element* e, ComputedStyle style, int staticX, int staticY, bool hasStatic);
    void layoutOutOfFlow(ContainingBlock& cb, int cbX, int cbY, int cbW, int cbH, bool isViewport);
    // Gives the boxes [from, boxes.size()) a positioned element's paint
    // key (see LayoutBox::paintKey).
    void applyPaintKey(size_t from, const ComputedStyle& style);
    // Clips the boxes [from, to) to a rectangle along the clipped axes.
    void clipBoxes(size_t from, size_t to, int x, int y, int w, int h, bool alongX, bool alongY);
    // Sticky elements whose parent block is still being laid out, by
    // level; the parent sets their maxShift when it ends.
    std::vector<std::vector<int>> openStickies_;
};
