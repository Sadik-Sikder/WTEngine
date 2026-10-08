// Layout.h
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <map>
#include <climits>
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
    //   paint order (and the reverse hit-test order). 0 = normal flow; 1 =
    //   a float's (above in-flow backgrounds, as in CSS); a positioned
    //   element's boxes get 4 * z-index + 2 (z-index auto = 0), so they
    //   paint above the flow and floats at the same z.
    // - fixed: position: fixed - `y` is relative to the top of the
    //   viewport, so the box stays put while the page scrolls.
    // - sticky: an index into LayoutRoot::stickies, whose shift for the
    //   current scroll position is added to `y` (see Engine::boxShift).
    // - clipped: drawn and hit-tested only inside clip* (document
    //   coordinates) - an ancestor's overflow other than visible.
    int paintKey = 0;
    // Non-zero: not a real box but an anchor - the zero-size marker
    // deferOutOfFlow leaves where an absolute element would have been in
    // the flow. It moves with the boxes around it (a flex item, table cell
    // or float is laid out at (0, 0) and moved later), so layoutOutOfFlow
    // reads the element's static position from it. Removed at the end of
    // layout(); never painted.
    int anchor = 0;
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
    // The page is in quirks mode (Document::quirks): a table then doesn't
    // inherit text-align or font styling from outside it.
    bool quirks = false;

    // Returns the pixel width of `text` at `fontSize`. Used to wrap text; a
    // rough per-character estimate is used when unset.
    std::function<float(const std::wstring&, int, bool bold, bool italic, const std::wstring* family)> measureText;
    // What measureText's results depend on besides its arguments (the page
    // zoom): textWidth caches widths across layouts, and starts afresh when
    // this changes.
    float measureScale = 1.0f;

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
    // list-style-type. String: a quoted string, used as the marker as written.
    enum class ListStyle { Disc, Circle, Square, Decimal, DecimalLeadingZero, LowerAlpha, UpperAlpha,
                           LowerRoman, UpperRoman, LowerGreek, String, None };
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
        // <center>, or align=center on a <div> or table part (CSS's
        // -webkit-center): block children narrower than the line are centred
        // too, not just text.
        bool centerBlocks = false;
        // line-height: a multiple of the font size (lineHeight >= 0), a fixed
        // pixel height (lineHeightPx >= 0), or neither for "normal".
        float lineHeight = -1;
        int lineHeightPx = -1;
        // text-decoration. Not inherited in CSS, but an ancestor's decoration
        // is drawn through its descendants' text, which comes to the same.
        bool underline = false;
        bool lineThrough = false;
        TextTransform transform = TextTransform::None;
        // list-style-type and list-style-position: inherited, so set on a list
        // and used by its items (see addListMarker). `listString` is the marker
        // for ListStyle::String.
        ListStyle listType = ListStyle::Disc;
        bool listInside = false;
        std::wstring listString;
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
    // em and rem work too (see emBase_); an unrecognized unit falls back to
    // `def`.
    int resolveLength(const std::wstring& s, int base, int def);
    // What em resolves against in resolveLength: the font size of the
    // element whose style is being computed (computeStyle keeps it current).
    // rem is always against the 14px root size.
    int emBase_ = 14;
    float textWidth(const std::wstring& text, int fontSize, bool bold = false, bool italic = false,
                    const std::wstring* family = nullptr);
    // Widths measured so far, by font (face, size, bold, italic) and then
    // text: the same words come up over and over, on a page and from one
    // layout to the next, and each measurement is a GDI call. Font faces
    // are interned (see TextPaint::family), so the pointer identifies one.
    struct FontKey {
        const std::wstring* family;
        int size;
        bool bold, italic;
        bool operator==(const FontKey& o) const { return family == o.family && size == o.size && bold == o.bold && italic == o.italic; }
    };
    struct FontKeyHash {
        size_t operator()(const FontKey& k) const {
            return std::hash<const void*>{}(k.family) ^ ((size_t)k.size << 2 | (size_t)k.bold << 1 | (size_t)k.italic) * 0x9E3779B97F4A7C15ull;
        }
    };
    std::unordered_map<FontKey, std::unordered_map<std::wstring, float>, FontKeyHash> textCache_;
    size_t textCacheSize_ = 0;
    float cachedScale_ = 1.0f;
    float textWidth(const std::wstring& text, int fontSize, const TextPaint& p) {
        return textWidth(text, fontSize, p.bold, p.italic, p.family);
    }

    struct FloatItem; // a float met in inline content - see below
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
        // A float, instead of a word: placed by layoutInlineRun where the
        // run reaches it - at the top of the current line if that's still
        // empty, else below it.
        std::shared_ptr<FloatItem> floatItem;
        // A block-level element met inside inline content (<a><div>, or a
        // <div> in a <span>): layoutInlineRun ends the line there, lays the
        // block out on its own lines, and carries on below it - CSS's
        // anonymous block boxes around a block inside an inline.
        std::shared_ptr<FloatItem> blockItem;
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
    // The Table* values are the parts of a table (display: table-row etc.,
    // or the default for <tr>, <td>, ...). Outside a table they're laid out
    // as plain blocks - no anonymous table is made around them.
    enum class Display { Block, Inline, None, Grid, Flex, Table, TableRowGroup, TableHeaderGroup,
                         TableFooterGroup, TableRow, TableCell, TableCaption };
    // vertical-align, as table cells use it (baseline is treated as top).
    // Unset: a cell takes its row's, then its row group's, else middle.
    enum class VAlign { Unset, Top, Middle, Bottom };
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
        // A content-sized track (auto, min-content, max-content,
        // fit-content()): as wide as the widest item that sits in it alone.
        bool isAuto = false;
        // An upper bound in px, or -1: fit-content(<cap>), or the max of
        // minmax(<min>, <cap>) on an fr-like track that grows up to it.
        float cap = -1;
    };
    struct ComputedStyle {
        std::wstring background;
        // No margin or padding unless the tag's default (computeStyle - as a
        // browser's built-in stylesheet gives <p>, <ul>, headings, ...) or
        // an author rule sets some.
        int marginTop = 0, marginBottom = 0, marginLeft = 0, marginRight = 0;
        int paddingTop = 0, paddingRight = 0, paddingBottom = 0, paddingLeft = 0;
        int width = -1; // -1 = auto: fill the container, same as if unset (today's only behavior)
        // min-/max-width in px (in the same box as `width`, per box-sizing),
        // -1 for none.
        int minWidth = -1, maxWidth = -1;
        // margin-left/-right: auto - a block narrower than its container
        // takes the leftover space there (both: centred).
        bool marginLeftAuto = false, marginRightAuto = false;
        bool centeredByParent = false; // the parent's text-align centres blocks (TextPaint::centerBlocks)
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
        // its container's layoutGrid). Start/end are CSS grid line numbers
        // as authored: 0 means auto, a negative one counts back from the
        // explicit grid's last line (-1 = the last line). *Span is the
        // `span N` from either side (0 = none). Either axis can be set
        // without the other - layoutGrid auto-places the unset one.
        int gridColumnStart = 0, gridColumnEnd = 0, gridColumnSpan = 0;
        int gridRowStart = 0, gridRowEnd = 0, gridRowSpan = 0;
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
        // How much of a line's overflow this item gives up, weighted by
        // its basis (the spec's "scaled shrink factor"). 1 is the CSS default.
        float flexShrink = 1;
        // Outer px, or -1 for auto (fall back to `width`). Set by
        // flex-basis or the `flex` shorthand (`flex: 1` means a basis of 0).
        int flexBasis = -1;
        // Only meaningful with display:flex, on the container.
        FlexWrap flexWrap = FlexWrap::NoWrap;
        // float and clear. A float is laid out like an absolute element
        // (shrink-to-fit, its own float context) but stays in the flow's
        // float context - see placeFloat.
        enum class Float { None, Left, Right } floatSide = Float::None;
        enum class Clear { None, Left, Right, Both } clear = Clear::None;
        bool flowRoot = false; // display: flow-root - a block that contains its floats
        // display: list-item - an <li> by default. It gets a marker (addListMarker).
        bool listItem = false;
        // Tables: border-collapse and border-spacing (horizontal, vertical)
        // on the table; vertical-align on a cell, row or row group.
        bool borderCollapse = false;
        int borderSpacingX = 2, borderSpacingY = 2;
        VAlign verticalAlign = VAlign::Unset;
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
    // the .cpp for exactly what's supported: px/%/em/rem/vw/fr tracks,
    // auto/min-content/max-content/fit-content(), minmax() and
    // repeat(N, <track>); anything else becomes 1fr, so the track *count*
    // an author wrote is always honored even where the sizing isn't).
    // `fontSize` is what em resolves against.
    std::vector<GridTrack> parseGridTemplateTracks(const std::wstring& v, int containingWidth, int fontSize);
    // Parses one side of a grid-column/grid-row - "auto", a line number
    // "N" (negative counts from the end), or "span N" - into `line`
    // (0 = auto) or `span` (0 = none). Named lines aren't supported.
    bool parseGridLine(const std::wstring& v, int& line, int& span);
    // Parses a grid-column/grid-row shorthand - "2", "span 3", "1 / -1",
    // "2 / span 3", "span 2 / 5", with or without spaces around the '/' -
    // into start/end lines and a span (see parseGridLine). Returns false
    // (leaving the outputs untouched) for anything else.
    bool parseGridLinePlacement(const std::wstring& v, int& start, int& end, int& span);
    // Parses a grid-template-areas value - one or more quoted strings (single
    // or double quotes), each one grid row, each whitespace-separated token
    // in it one column's area name ("." means no area). Returns an empty grid (not a partial one)
    // for anything malformed: an unterminated quoted string, or rows with
    // different column counts - real CSS requires every row to name the
    // same number of columns, and this engine does too, just by rejecting
    // the whole thing rather than trying to reconcile mismatched rows.
    std::vector<std::vector<std::wstring>> parseGridTemplateAreas(const std::wstring& v);
    // Parses the grid-template shorthand: either the area-string form - e.g.
    // `"header header" 40px "sidebar main" 1fr / 100px 1fr` - into areas
    // (via parseGridTemplateAreas), row tracks (one per area-row, an
    // optional track size token right after its closing quote - see the
    // .cpp for how "no size given" is distinguished from "explicit 0"),
    // and column tracks (the part after the top-level '/', via
    // parseGridTemplateTracks); or the plain `<rows> / <columns>` form,
    // track lists on both sides.
    void parseGridTemplateShorthand(const std::wstring& v, int containingWidth, int fontSize,
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
    //    the bounding box regardless). Otherwise its grid-column/grid-row
    //    resolve per axis to a cell range (lines, negative lines from the
    //    end, span N), or stay auto with just a span size. Columns clamp
    //    to the template's count - a line beyond it doesn't create an
    //    implicit column.
    // 3. CSS's sparse row-flow auto-placement: fully definite items
    //    first, then definite-row items in the first columns that fit,
    //    then the rest in source order behind a forward-only cursor (a
    //    span that doesn't fit in the rest of a row wraps). No "dense"
    //    packing, and a full definite row overlaps rather than growing
    //    implicit columns.
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
    // A flex or grid item for the run of text nodes starting at el->children[i]
    // (`i` is moved to the run's last one): CSS's anonymous item - an element
    // with no tag, holding that text, which computeStyle gives only inherited
    // style and whose words layoutElement credits to the container. Made once
    // per layout() pass; nullptr for a run of only whitespace, which makes none.
    Element* anonymousItem(Element* el, size_t& i);
    std::unordered_map<const Node*, std::unique_ptr<Element>> anonymousItems_;
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
    // The narrowest `item` can be without its content overflowing: its
    // widest word, image or fixed-width box, plus its own chrome - what a
    // flex item won't shrink below (CSS's min-width: auto).
    int minContentWidth(Element* item, const ComputedStyle& style);
    // Both of those: lays `item` out at `width` in measuring mode and
    // measures how far its content reaches (outer, margins included).
    // `narrow`: laid out at its narrowest, where nested boxes count too.
    int contentWidth(Element* item, const ComputedStyle& style, int width, bool narrow);

    // --- Floats ---------------------------------------------------------
    // A float (or a block - InlineItem::blockItem) found in inline content,
    // with what it needs to be laid out later, when layoutInlineRun reaches
    // it.
    struct FloatItem {
        Element* el = nullptr;
        ComputedStyle style;
        std::vector<Element*> ancestors;
        std::wstring href;
        Element* form = nullptr;
    };
    // A placed float's margin box, in the same coordinates as the boxes
    // around it.
    struct FloatBox {
        bool left = true;
        int x = 0, y = 0, w = 0, h = 0;
    };
    // The floats of one block formatting context: a float affects every
    // line in it, nested blocks' included, until it ends. A new one starts
    // at the root, in every detached layout (flex/grid items, table cells,
    // absolute elements, floats) and in a block that contains its floats
    // (overflow other than visible, display: flow-root, table, flex, grid).
    struct FloatContext {
        std::vector<FloatBox> floats;
        int lastTop = INT_MIN; // a float never sits higher than an earlier one
        int bottom(bool left, bool right) const; // lowest bottom among those sides' floats, or INT_MIN
    };
    FloatContext* floatCtx_ = nullptr;
    // Set by layoutItemDetached: the block it lays out is a float context
    // root whatever its style says (layoutBlockChild reads and clears it).
    bool detachedRoot_ = false;
    // The horizontal space [left, right) floats leave in [x, x + width) for
    // something occupying [top, top + height). Returns whether any float
    // narrowed it; `nextTop` gets the bottom of the highest float that did
    // (where to look next if it doesn't fit).
    bool spaceBeside(int top, int height, int x, int width, int& left, int& right, int& nextTop) const;
    // Lays `item` out and floats it to the left or right of [cbX, cbX + cbW)
    // at `y` or below, wherever its width fits beside earlier floats.
    void placeFloat(const FloatItem& item, int cbX, int cbW, int y);
    // Lays out a block or control met inside inline content
    // (InlineItem::blockItem) at (x, y), in the context it was met in.
    void layoutInlineBlock(const FloatItem& item, int x, int& y, int width);
    // The y a block with `clear` must start at (or below).
    int clearance(ComputedStyle::Clear clear) const;
    // An inline item standing for the float `e` (or, `block`, for a
    // block-level element inside inline content - InlineItem::blockItem),
    // with the context it's laid out in (ancestors, link, form) as it is
    // here.
    InlineItem makeFloatItem(Element* e, ComputedStyle style, bool block = false);

    // --- Tables (layoutTable) ------------------------------------------
    // A table's rows and cells, read from the DOM, with each column's
    // min/max widths measured. Built by buildTable before the table's own
    // width is settled (an auto-width table is as wide as its columns
    // want), then used by layoutTable to place everything.
    struct TableCell {
        Element* el = nullptr;
        ComputedStyle style; // margins zeroed, width/height turned into column/row hints
        std::vector<Element*> ancestors; // ancestorStack to lay it out with
        Element* form = nullptr; // a <form> sitting between table parts, if any
        int row = 0, col = 0, rowSpan = 1, colSpan = 1;
        int minWidth = 0, maxWidth = 0; // border box
        int fixedWidth = -1; // an explicit width (border box), or -1 - the column's preferred width, not a minimum
        VAlign valign = VAlign::Middle;
    };
    struct TableRow {
        Element* el = nullptr; // nullptr for an anonymous row (cells with no <tr>)
        ComputedStyle style;
        int group = -1;     // index into TableModel::groups
        int section = 0;    // rowspan doesn't cross from one section into another
        int minHeight = 0;
    };
    struct TableModel {
        std::vector<std::pair<Element*, ComputedStyle>> captions;
        std::vector<std::pair<Element*, ComputedStyle>> groups; // row groups (<tbody>...), by TableRow::group
        std::vector<TableRow> rows;
        std::vector<TableCell> cells; // in row order
        int numCols = 0;
        std::vector<int> colMin, colMax;
        std::vector<bool> colFixed;
        // Gaps between columns/rows, and from the table's content edge to the
        // outer cells. border-collapse makes them negative: neighbouring
        // cell borders overlap instead of sitting side by side.
        int gapX = 0, gapY = 0, edgeX = 0, edgeY = 0;
        int minWidth = 0, maxWidth = 0; // of the cell grid, gaps included
    };
    std::unique_ptr<TableModel> buildTable(Element* table, const ComputedStyle& style, int available);
    void layoutTable(Element* el, int x, int& y, int contentWidth, const ComputedStyle& style, const TableModel& t);
    // A table cell laid out at local (0, 0) and `width`: its boxes and its
    // border-box height. While measuring_, results are memoized per element
    // and width for the rest of this layout() pass - each cell is measured
    // at two widths, and the measuring layouts of nested tables would
    // otherwise multiply level by level. A real (non-measuring) layout is
    // never cached, so its side effects (absolute descendants registering
    // with their containing block, sticky elements) always happen.
    std::pair<std::vector<LayoutBox>, int> layoutCell(const TableCell& cell, int width);
    std::map<std::pair<const Element*, int>, std::pair<std::vector<LayoutBox>, int>> cellCache_;
    // Set while measuring a content width - a table cell's min/max-content
    // width, or shrinkToFitWidth's: text isn't aligned or broken mid-word,
    // lines can be as narrow as one word, and free space isn't handed out
    // (flex-grow, justify-content, auto margins, centring), since all of
    // those would make content reach further than it needs.
    bool measuring_ = false;

    // Margin collapsing (layoutBlockChild). marginEndY_ is where the last
    // margin laid out ends - a block's bottom margin, or a parent's top
    // margin with nothing (no border, padding, content) after it yet - and
    // pendingMargin_ its size. A block starting exactly there merges its
    // top margin with it instead of adding to it. openTops_ holds, for each
    // parent whose top margin is still open to its first child, where its
    // content starts and (newTop) where its box top ends up once that
    // child's margin has merged with it.
    int marginEndY_ = INT_MIN;
    int pendingMargin_ = 0;
    struct OpenTop { int y; int newTop; };
    std::vector<OpenTop> openTops_;

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
    // A Bloom filter of tag names, ids and classes: for an element, those of
    // all its ancestors; for a rule, those its selector requires of the
    // element's ancestors (the compounds left of a descendant or child
    // combinator). A rule whose requirements aren't all in the element's
    // filter can't match, so computeStyle skips CSS::matches for it - most
    // descendant rules (".navbox a") fail that way without walking the
    // ancestor chain. False positives just mean a full match is tried.
    struct AncestorFilter {
        uint64_t bits[8] = {};
        void add(size_t hash) {
            bits[(hash >> 6) & 7] |= 1ull << (hash & 63);
            bits[(hash >> 15) & 7] |= 1ull << ((hash >> 9) & 63);
        }
        void merge(const AncestorFilter& o) { for (int i = 0; i < 8; i++) bits[i] |= o.bits[i]; }
        bool covers(const AncestorFilter& need) const {
            for (int i = 0; i < 8; i++) if (need.bits[i] & ~bits[i]) return false;
            return true;
        }
    };
    struct RuleIndex {
        std::unordered_map<std::wstring, std::vector<const CSS::Rule*>> byId, byClass, byTag;
        std::vector<const CSS::Rule*> universal;
        std::vector<AncestorFilter> required; // per rule, by its position in *rules
    } ruleIndex;
    void rebuildRuleIndex();
    // The filter of ancestorStack's elements, kept as a stack alongside it:
    // computeStyle revalidates it against ancestorStack (which is pushed,
    // popped and swapped wholesale in many places) and recomputes only what
    // changed.
    std::vector<Element*> filterFor_;
    std::vector<AncestorFilter> filterStack_;
    const AncestorFilter& ancestorFilter();
    // `el`'s parsed class="" list, cached for this pass (classCache).
    const std::vector<std::wstring>& classesOf(Element* el);

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
        int anchor = 0; // the LayoutBox::anchor marking the static position, 0 if none
        std::vector<Element*> ancestors;
        std::wstring href;
        Element* form = nullptr;
    };
    int nextAnchor_ = 1;
    struct ContainingBlock {
        std::vector<OutOfFlow> pending;
    };
    // Innermost positioned ancestor last; [0] is the viewport.
    std::vector<ContainingBlock*> positioned_;
    void deferOutOfFlow(Element* e, ComputedStyle style, int staticX, int staticY, bool hasStatic);
    // `searchFrom`: where in `boxes` the containing block's own boxes start
    // - its absolute elements' anchors are among them.
    void layoutOutOfFlow(ContainingBlock& cb, int cbX, int cbY, int cbW, int cbH, bool isViewport, size_t searchFrom);
    // Gives the boxes [from, boxes.size()) a positioned element's paint
    // key (see LayoutBox::paintKey).
    void applyPaintKey(size_t from, const ComputedStyle& style);
    // Clips the boxes [from, to) to a rectangle along the clipped axes.
    void clipBoxes(size_t from, size_t to, int x, int y, int w, int h, bool alongX, bool alongY);
    // Sticky elements whose parent block is still being laid out, by
    // level; the parent sets their maxShift when it ends.
    std::vector<std::vector<int>> openStickies_;

    // --- List markers ------------------------------------------------
    // An <li>'s number: from its `value`, else one on from the <li> before it
    // (one back in a <ol reversed>), else the list's `start` (default 1, or
    // the item count when reversed). Computed for a whole list at once and
    // kept for this layout() pass.
    int listOrdinal(Element* li);
    std::unordered_map<Element*, int> listOrdinals_;
    // The marker's text for a counter style ("3.", "c.", "iv."), or empty for
    // the shapes (disc, circle, square), which are drawn as boxes.
    static std::wstring markerText(const TextPaint& p, int ordinal);
    // A disc/circle/square marker, sized for `fontSize`, at (0, 0).
    static LayoutBox markerShape(const TextPaint& p, int fontSize);
    // An outside marker: placed left of the list item's content box
    // (`contentX`), level with the first line among the boxes from `from`
    // on, or with `contentTop` when the item has none.
    void addListMarker(Element* e, const ComputedStyle& sv, int contentX, int contentTop, size_t from);
    // An inside marker waits here for layoutElement to put it at the start
    // of `insideMarkerFor_`'s inline content.
    std::vector<InlineItem> insideMarker_;
    Element* insideMarkerFor_ = nullptr;
};
