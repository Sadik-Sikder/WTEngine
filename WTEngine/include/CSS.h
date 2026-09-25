// CSS.h
#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct Element; // defined in DOM.h; used here only as a pointer

namespace CSS {

// How specific a selector is, used to break ties when more than one rule
// matches the same element: an id beats any number of classes, and a class
// beats a bare tag name.
struct Specificity {
    int ids = 0, classes = 0, tags = 0;
    bool operator<(const Specificity& o) const {
        if (ids != o.ids) return ids < o.ids;
        if (classes != o.classes) return classes < o.classes;
        return tags < o.tags;
    }
};

// `[name]`, `[name=value]`, and the ~= |= ^= $= *= operators, with an
// optional trailing ` i` for case-insensitive value matching.
struct AttrSelector {
    std::wstring name;   // lowercased
    wchar_t op = 0;      // 0 = presence only; otherwise '=', '~', '|', '^', '$', '*'
    std::wstring value;
    bool caseInsensitive = false;
};

// The pseudo-classes this engine can evaluate. Anything else (and every
// pseudo-element - ::before, ::placeholder, ...) makes parseSelector reject
// the whole selector, as before.
struct PseudoClass {
    enum Kind {
        Hover, FirstChild, LastChild, OnlyChild, NthChild, NthLastChild,
        FirstOfType, LastOfType, OnlyOfType, NthOfType, NthLastOfType,
        Root, Empty, AnyLink, Never, // Never: :visited/:focus/:active/... - valid, but never true here
    } kind;
    int a = 0, b = 0; // for the nth-* kinds: matches index (1-based) n where n = a*k + b for some k >= 0
};

// How a compound relates to the one before it in the chain.
enum class Combinator { Descendant, Child, NextSibling, SubsequentSibling };

// One compound in a selector chain, e.g. the `div.card` in `div.card > p`.
// An empty tag matches any element (a bare `.class`, `#id`, `[attr]`, or
// `*` written on its own).
struct CompoundSelector {
    std::wstring tag;
    std::vector<std::wstring> classes;
    std::wstring id;
    std::vector<AttrSelector> attrs;
    std::vector<PseudoClass> pseudos;
    std::vector<CompoundSelector> nots; // :not(...) arguments, each a single compound
    // The combinator between this compound and the previous one in the
    // chain (ignored on chain.front()).
    Combinator combinator = Combinator::Descendant;
};

struct Rule {
    std::vector<CompoundSelector> chain; // outermost ancestor first; chain.back() is the element itself
    std::vector<std::pair<std::wstring, std::wstring>> declarations;
    Specificity specificity;
    int order = 0; // source order, to break specificity ties
    // A width this rule's @media condition requires the viewport to be at
    // least/at most (px), or -1 for no constraint on that side. Set by
    // parseStylesheet for a rule found inside a "@media ... (min-width:
    // Npx) ..."/"(max-width:Npx)" block; not checked by matches() itself
    // (which stays viewport-agnostic) - the caller (LayoutRoot::computeStyle)
    // checks it against the live viewport alongside matches(), so it's
    // re-evaluated on every relayout (e.g. a window resize), not just once.
    int mediaMinWidth = -1, mediaMaxWidth = -1;
};

// Parses one selector (e.g. "div.card#id", "#target", "ul > li:first-child",
// "input[type=text]") into a chain - the same grammar parseStylesheet uses for each
// comma-separated selector in a rule, exposed standalone for matching
// outside of stylesheet application (e.g. querySelector). False if the
// chain ends up empty (unsupported syntax, or an empty selector).
bool parseSelector(const std::wstring& selector, std::vector<CompoundSelector>& chain);

// Parses the text of one or more <style> blocks into rules. Understands
// tag/.class/#id/* selectors, attribute selectors, the pseudo-classes in
// PseudoClass, :not(<compound>), compounds (div.card[title]:hover),
// comma-separated groups, and all four combinators (descendant, >, +, ~);
// strips comments. A bare-media-type @media block ("@media
// screen{...}", "@media print{...}", "@media all{...}", or no type at
// all) is evaluated outright (screen/all/none = always applies, print =
// never) and its content parsed as if unwrapped. A @media condition
// combining a type with (min-width:Npx)/(max-width:Npx) via "and" (e.g.
// "screen and (min-width:1120px)") is also handled, but differently -
// since the viewport isn't known at parse time, every rule found inside
// is tagged with the width bound (Rule::mediaMinWidth/mediaMaxWidth) and
// checked against the live viewport wherever rules are matched, not
// decided once here. Every other @-rule - @supports, @import, @font-face,
// @keyframes, a comma-separated media query list, or any feature besides
// min-width/max-width (prefers-color-scheme, hover, orientation, ...) -
// is still always skipped, never conditionally applied.
// Not supported (the selector is skipped): pseudo-elements, and
// pseudo-classes not listed in PseudoClass.
std::vector<Rule> parseStylesheet(const std::wstring& css);

// Parses a `prop: value; prop: value` block - the same grammar for the body
// of a stylesheet rule and for an inline style="" attribute.
std::vector<std::pair<std::wstring, std::wstring>> parseDeclarations(const std::wstring& block);

// Caches each element's parsed class list (see matches()'s `cache` param) -
// classesOf would otherwise re-split the same class="" string from scratch
// on every single match attempt. Keyed by raw pointer, so an instance is
// only valid as long as none of its elements are freed and none of their
// `class` attributes change - true for exactly one layout pass (the DOM
// never mutates mid-layout), which is the only place this is worth using.
using ClassCache = std::unordered_map<const Element*, std::vector<std::wstring>>;

// The elements :hover currently applies to: the element under the mouse
// and all of its ancestors. Only ever compared by pointer, never
// dereferenced, so a stale entry left by a DOM mutation is harmless (it
// can't match anything that isn't at that same address, and the set is
// rebuilt on the next mouse move anyway).
using HoverSet = std::unordered_set<const Element*>;

// True if `rule` matches `el`, given the chain of `el`'s ancestors from the
// root down (root first; does not include `el` itself). Sibling
// combinators and the structural pseudo-classes (:first-child, ...) use
// `parent` pointers to find siblings.
//
// `cache`, if given, speeds up repeated matching against the same elements
// - e.g. layout calling this once per rule for every element on the page,
// where the same element's class list would otherwise be recomputed for
// every rule that has a class in its selector. Leave it null for a one-off
// match (e.g. querySelector), where there's nothing to amortize.
// `hover` null means nothing is hovered (:hover never matches).
bool matches(const Rule& rule, const std::vector<Element*>& ancestors, Element* el,
             ClassCache* cache = nullptr, const HoverSet* hover = nullptr);

// Whether hovering or un-hovering any of `changed` could change a style:
// true if some :hover compound in `rules` matches one of them (with :hover
// assumed true). Lets the engine skip a relayout when the mouse moves over
// elements no :hover rule cares about. Unlike HoverSet, these *are*
// dereferenced, so every pointer must be a live element.
bool hoverCouldAffect(const std::vector<Rule>& rules, const std::vector<const Element*>& changed);
// Whether any rule uses :hover at all.
bool hasHoverRules(const std::vector<Rule>& rules);

} // namespace CSS
