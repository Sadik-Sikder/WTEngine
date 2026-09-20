// CSS.h
#pragma once
#include <string>
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

// One "E" in an "E F G"-style descendant selector, e.g. the `div.card` in
// `div.card p`. An empty tag matches any element (a bare `.class`, `#id`,
// or `*` written on its own).
struct CompoundSelector {
    std::wstring tag;
    std::vector<std::wstring> classes;
    std::wstring id;
};

struct Rule {
    std::vector<CompoundSelector> chain; // outermost ancestor first; chain.back() is the element itself
    std::vector<std::pair<std::wstring, std::wstring>> declarations;
    Specificity specificity;
    int order = 0; // source order, to break specificity ties
};

// Parses one selector (e.g. "div.card#id", "#target", ".foo bar") into a
// descendant chain - the same grammar parseStylesheet uses for each
// comma-separated selector in a rule, exposed standalone for matching
// outside of stylesheet application (e.g. querySelector). False if the
// chain ends up empty (unsupported syntax, or an empty selector).
bool parseSelector(const std::wstring& selector, std::vector<CompoundSelector>& chain);

// Parses the text of one or more <style> blocks into rules. Understands
// tag/.class/#id/* selectors, compounds (div.card), comma-separated groups,
// and plain descendant combinators ("a b" - not necessarily a direct
// parent); strips comments and drops @-rules (a conditional block like
// @media is always skipped, never conditionally applied).
// Not supported: >, +, ~ combinators, attribute selectors, pseudo-classes.
std::vector<Rule> parseStylesheet(const std::wstring& css);

// Parses a `prop: value; prop: value` block - the same grammar for the body
// of a stylesheet rule and for an inline style="" attribute.
std::vector<std::pair<std::wstring, std::wstring>> parseDeclarations(const std::wstring& block);

// True if `rule` matches `el`, given the chain of `el`'s ancestors from the
// root down (root first; does not include `el` itself).
bool matches(const Rule& rule, const std::vector<Element*>& ancestors, Element* el);

} // namespace CSS
