// EngineForms.cpp - form controls: drawing, focus, editing and submission.
// (Part of Engine; kept in its own file so Engine.cpp stays about layout/paint.)
#define NOMINMAX
#include "Engine.h"
#include "Renderer.h"
#include "Fetcher.h"
#include "JSBinding.h"
#include "JSEngine.h"
#include <algorithm>
#include <cmath>
#include <cwctype>

namespace {
    const int kInputPad = 8;    // gap between a field's edge and its text
    const float kDefaultFont = 14; // used only where a box has no fontSize (shouldn't normally happen)

    const Color kCtlBorder{ 0.60f, 0.62f, 0.66f, 1.0f };
    const Color kCtlFocus{ 0.20f, 0.45f, 0.90f, 1.0f };
    const Color kWhite{ 1, 1, 1, 1 };
    const Color kButtonFill{ 0.92f, 0.93f, 0.95f, 1.0f };
    const Color kSelection{ 0.68f, 0.82f, 1.0f, 1.0f };
    const Color kInk{ 0, 0, 0, 1 };
    const Color kPlaceholder{ 0.55f, 0.55f, 0.58f, 1.0f };

    std::wstring attrOf(const Element* el, const wchar_t* key) {
        auto it = el->attrs.find(key);
        return it == el->attrs.end() ? L"" : it->second;
    }

    std::wstring lower(std::wstring s) {
        for (auto& c : s) c = (wchar_t)towlower(c);
        return s;
    }

    bool isPassword(const Element* el) {
        return lower(attrOf(el, L"type")) == L"password";
    }

    // --- <select>/<option> helpers -----------------------------------
    // A <select>'s selection lives in the DOM the same way a checkbox's
    // does (attrs["checked"] there, attrs["selected"] on the chosen
    // <option> here) - so the closed box's label is read live at draw
    // time below, never baked into a LayoutBox, and picking an option
    // never needs a re-layout.

    std::vector<Element*> selectOptions(const Element* selectEl) {
        std::vector<Element*> out;
        for (auto& child : selectEl->children) {
            if (child->type != Node::ELEMENT) continue;
            auto* e = static_cast<Element*>(child.get());
            if (e->tag == L"option") out.push_back(e);
        }
        return out;
    }

    // No option explicitly marked selected -> the first one, matching how
    // real HTML defaults an unmarked <select>.
    Element* selectedOption(const Element* selectEl) {
        Element* first = nullptr;
        for (Element* opt : selectOptions(selectEl)) {
            if (!first) first = opt;
            if (opt->attrs.count(L"selected")) return opt;
        }
        return first;
    }

    std::wstring optionLabel(const Element* optionEl) {
        for (auto& child : optionEl->children) {
            if (child->type == Node::TEXT) return static_cast<TextNode*>(child.get())->text;
        }
        return L"";
    }

    std::wstring optionValue(const Element* optionEl) {
        auto it = optionEl->attrs.find(L"value");
        return it != optionEl->attrs.end() ? it->second : optionLabel(optionEl);
    }

    void chooseOption(Element* selectEl, Element* optionEl) {
        for (Element* opt : selectOptions(selectEl)) {
            if (opt == optionEl) opt->attrs[L"selected"] = L"";
            else opt->attrs.erase(L"selected");
        }
    }

    // <button> defaults to submit; <input> only when type=submit/image.
    bool isSubmitButton(const Element* el) {
        std::wstring type = lower(attrOf(el, L"type"));
        if (el->tag == L"button") return type.empty() || type == L"submit";
        return type == L"submit" || type == L"image";
    }

    void appendField(std::string& body, const std::wstring& name, const std::wstring& value) {
        if (name.empty()) return; // unnamed controls aren't submitted
        if (!body.empty()) body.push_back('&');
        body += urlEncodeForm(name) + "=" + urlEncodeForm(value);
    }

    // Gathers name=value pairs from every control inside `el`, in document order.
    void collectFields(const Element* el, const Element* submitter, std::string& body) {
        for (auto& child : el->children) {
            if (child->type != Node::ELEMENT) continue;
            auto* c = static_cast<Element*>(child.get());
            std::wstring name = attrOf(c, L"name");

            if (c->tag == L"input") {
                std::wstring type = lower(attrOf(c, L"type"));
                if (type == L"submit" || type == L"image") {
                    if (c == submitter) appendField(body, name, attrOf(c, L"value"));
                }
                else if (type == L"button" || type == L"reset" || type == L"file") {
                    // never submitted
                }
                else if (type == L"checkbox" || type == L"radio") {
                    if (c->attrs.count(L"checked")) {
                        std::wstring v = attrOf(c, L"value");
                        appendField(body, name, v.empty() ? L"on" : v);
                    }
                }
                else {
                    appendField(body, name, attrOf(c, L"value"));
                }
            }
            else if (c->tag == L"button") {
                if (c == submitter) appendField(body, name, attrOf(c, L"value"));
            }
            else if (c->tag == L"select") {
                if (Element* opt = selectedOption(c)) appendField(body, name, optionValue(opt));
            }

            collectFields(c, submitter, body);
        }
    }
}

const LayoutBox* Engine::controlAt(int x, int y) const {
    if (y < topInset) return nullptr;
    int docY = y - topInset + scrollY;

    for (auto it = layoutRoot.boxes.rbegin(); it != layoutRoot.boxes.rend(); ++it) {
        const auto& b = *it;
        if (b.control == LayoutBox::NoControl) continue;
        if (x >= b.x && x < b.x + b.width && docY >= b.y && docY < b.y + b.height) return &b;
    }
    return nullptr;
}

Engine::Cursor Engine::cursorAt(int x, int y, Renderer& renderer) const {
    if (const LayoutBox* c = controlAt(x, y))
        return c->control == LayoutBox::TextField ? Cursor::IBeam : Cursor::Hand;
    return linkAt(x, y, renderer).empty() ? Cursor::Arrow : Cursor::Hand;
}

void Engine::focusInput(const LayoutBox& box) {
    focusedEl = box.el;
    focusedForm = box.form;
    editor.setText(attrOf(box.el, L"value"));
    editor.setMasked(isPassword(box.el));
    inputScrollX = 0;
}

void Engine::blurInput() {
    focusedEl = nullptr;
    focusedForm = nullptr;
}

void Engine::syncValue() {
    if (focusedEl) focusedEl->attrs[L"value"] = editor.text();
}

bool Engine::onClick(int x, int y, double now, Renderer& renderer) {
    if (openSelect) {
        // Consume this click no matter where it lands - picks an option if
        // it hit one of the dropdown's rows, otherwise just dismisses it
        // (clicking outside a native <select> popup doesn't also activate
        // whatever's underneath).
        if (y >= topInset) {
            int docY = y - topInset + scrollY;
            int rowHeight = std::max(24, openSelectBox.fontSize + 10);
            int rowY = openSelectBox.y + openSelectBox.height;
            std::vector<Element*> opts = selectOptions(openSelect);
            for (size_t i = 0; i < opts.size(); i++) {
                int top = rowY + (int)i * rowHeight;
                if (x >= openSelectBox.x && x < openSelectBox.x + openSelectBox.width &&
                    docY >= top && docY < top + rowHeight) {
                    chooseOption(openSelect, opts[i]);
                    break;
                }
            }
        }
        openSelect = nullptr;
        return true;
    }

    const LayoutBox* b = controlAt(x, y);
    if (!b) {
        blurInput();
        return false;
    }

    switch (b->control) {
    case LayoutBox::TextField: {
        bool isDouble = editor.registerClick(x, now);
        if (focusedEl != b->el) focusInput(*b);
        float fs = b->fontSize > 0 ? (float)b->fontSize : kDefaultFont;
        float localX = x - (b->x + kInputPad) + inputScrollX;
        if (isDouble) editor.selectWordAt(localX, renderer, fs);
        else editor.placeCaretAt(localX, renderer, fs);
        break;
    }
    case LayoutBox::Button:
        blurInput();
        if (b->form && isSubmitButton(b->el)) queueSubmit(b->form, b->el);
        break;
    case LayoutBox::Checkbox:
        blurInput();
        if (b->el->attrs.count(L"checked")) b->el->attrs.erase(L"checked");
        else b->el->attrs[L"checked"] = L"";
        break;
    case LayoutBox::Select:
        blurInput();
        openSelect = b->el;
        openSelectBox = *b;
        break;
    default:
        break;
    }
    return true;
}

bool Engine::dispatchClick(int x, int y) {
    if (!jsEngine) return false;
    if (y < topInset) return false;
    int docY = y - topInset + scrollY;

    // Topmost (last-painted) box wins, same hit-test style as controlAt -
    // a bounding-box check rather than linkAt's tighter glyph-width check,
    // since a click listener's target usually covers its whole box (e.g. a
    // <div onclick>), not just the visible text inside it.
    Element* target = nullptr;
    for (auto it = layoutRoot.boxes.rbegin(); it != layoutRoot.boxes.rend(); ++it) {
        const auto& b = *it;
        if (!b.el) continue;
        if (x >= b.x && x < b.x + b.width && docY >= b.y && docY < b.y + b.height) {
            target = b.el;
            break;
        }
    }
    if (!target) return false;

    return ::dispatchClick(jsEngine->context(), target);
}

void Engine::onChar(unsigned int cp) {
    if (!focusedEl) return;
    editor.onChar(cp);
    syncValue();
}

void Engine::insertText(const std::wstring& s) {
    if (!focusedEl) return;
    editor.insert(s);
    syncValue();
}

bool Engine::onEditKey(int key) {
    if (!focusedEl) return false;
    bool handled = editor.onEditKey(key);
    syncValue();
    return handled;
}

void Engine::selectAllInput() {
    if (focusedEl) editor.selectAll();
}

std::wstring Engine::focusedSelection() const {
    if (!focusedEl || editor.masked()) return L""; // never copy a password
    return editor.selectedText();
}

void Engine::focusNextInput(bool backwards) {
    std::vector<const LayoutBox*> fields;
    for (const auto& b : layoutRoot.boxes) {
        if (b.control == LayoutBox::TextField) fields.push_back(&b);
    }
    if (fields.empty()) return;

    size_t cur = fields.size(); // "none focused"
    for (size_t i = 0; i < fields.size(); i++) {
        if (fields[i]->el == focusedEl) cur = i;
    }
    size_t next;
    if (cur == fields.size()) next = backwards ? fields.size() - 1 : 0;
    else next = backwards ? (cur + fields.size() - 1) % fields.size() : (cur + 1) % fields.size();

    const LayoutBox& target = *fields[next];
    focusInput(target);
    editor.selectAll();

    // Scroll the field into view if it's off-screen.
    int top = target.y - scrollY;
    if (top < 0 || top + target.height > viewHeight())
        scroll((target.y - viewHeight() / 3) - scrollY);
}

void Engine::submitFocused() {
    if (focusedEl && focusedForm) queueSubmit(focusedForm, nullptr);
}

void Engine::queueSubmit(Element* form, Element* submitter) {
    submission = FormSubmission{};
    submission.action = attrOf(form, L"action");
    submission.post = lower(attrOf(form, L"method")) == L"post";
    collectFields(form, submitter, submission.body);
    hasSubmission = true;
}

bool Engine::takeSubmission(FormSubmission& out) {
    if (!hasSubmission) return false;
    out = std::move(submission);
    hasSubmission = false;
    return true;
}

void Engine::drawControl(Renderer& r, const LayoutBox& b, int sy, double t) {
    const float fx = (float)b.x, fy = (float)sy, fw = (float)b.width, fh = (float)b.height;
    const bool focused = b.el == focusedEl;
    const float kFont = b.fontSize > 0 ? (float)b.fontSize : kDefaultFont;
    const Color fill = b.background.empty() ? kWhite : parseColor(b.background);

    switch (b.control) {
    case LayoutBox::TextField: {
        r.drawRect(fx - 1, fy - 1, fw + 2, fh + 2, focused ? kCtlFocus : kCtlBorder);
        r.drawRect(fx, fy, fw, fh, fill);

        std::wstring shown;
        if (focused) {
            shown = editor.display();
        }
        else {
            shown = attrOf(b.el, L"value");
            if (isPassword(b.el)) shown.assign(shown.size(), (wchar_t)0x2022); // bullet
        }

        r.setClip(fx + 1, fy + 1, fw - 2, fh - 2); // long text scrolls inside the field
        float textX = fx + kInputPad;

        if (focused) {
            float viewW = fw - 2 * kInputPad;
            float caretX = r.measureText(shown.substr(0, editor.cursor()), kFont);
            if (caretX - inputScrollX > viewW) inputScrollX = caretX - viewW;
            if (caretX - inputScrollX < 0) inputScrollX = caretX;
            textX -= inputScrollX;

            if (editor.hasSelection()) {
                float x0 = r.measureText(shown.substr(0, editor.selLow()), kFont);
                float x1 = r.measureText(shown.substr(0, editor.selHigh()), kFont);
                r.drawRect(textX + x0, fy + 3, x1 - x0, fh - 6, kSelection);
            }
        }

        if (shown.empty()) {
            std::wstring hint = attrOf(b.el, L"placeholder");
            if (!hint.empty()) r.drawText(fx + kInputPad, fy + 4, hint, kFont, kPlaceholder);
        }
        else {
            r.drawText(textX, fy + 4, shown, kFont, kInk);
        }

        if (focused && !editor.hasSelection() && std::fmod(t, 1.0) < 0.6) { // blinking caret
            float caretX = r.measureText(shown.substr(0, editor.cursor()), kFont);
            r.drawRect(fx + kInputPad + caretX - inputScrollX, fy + 4, 1.5f, fh - 8, kInk);
        }
        r.clearClip();
        break;
    }
    case LayoutBox::Button: {
        r.drawRect(fx - 1, fy - 1, fw + 2, fh + 2, kCtlBorder);
        r.drawRect(fx, fy, fw, fh, b.background.empty() ? kButtonFill : parseColor(b.background));
        r.setClip(fx + 1, fy + 1, fw - 2, fh - 2);
        float tw = r.measureText(b.text, kFont);
        r.drawText(fx + std::max((fw - tw) / 2, 4.0f), fy + 5, b.text, kFont, kInk);
        r.clearClip();
        break;
    }
    case LayoutBox::Checkbox: {
        r.drawRect(fx - 1, fy - 1, fw + 2, fh + 2, kCtlBorder);
        r.drawRect(fx, fy, fw, fh, fill);
        if (b.el->attrs.count(L"checked")) r.drawRect(fx + 4, fy + 4, fw - 8, fh - 8, kCtlFocus);
        break;
    }
    case LayoutBox::Select: {
        // Border/fill like TextField, clipped label like Button - the
        // closed box always shows whichever <option> is currently
        // selected, read live rather than baked in at layout time (see
        // chooseOption/selectedOption), so picking one never needs a
        // re-layout.
        r.drawRect(fx - 1, fy - 1, fw + 2, fh + 2, openSelect == b.el ? kCtlFocus : kCtlBorder);
        r.drawRect(fx, fy, fw, fh, fill);
        r.setClip(fx + 1, fy + 1, fw - 2, fh - 2);
        if (Element* opt = selectedOption(b.el)) r.drawText(fx + kInputPad, fy + 4, optionLabel(opt), kFont, kInk);
        r.clearClip();
        break;
    }
    default:
        break;
    }
}

// Draws openSelect's dropdown, if one is open: a bordered list positioned
// right below its closed box (openSelectBox, captured when it was opened -
// see Engine::onClick), one row per <option>, the selected one highlighted.
// This isn't part of layoutRoot.boxes - it's transient UI state, not
// document flow - so it's drawn as a final overlay pass, the same "on top
// of the page" treatment main.cpp gives AddressBar.
void Engine::drawOpenSelect(Renderer& r) {
    if (!openSelect) return;

    const LayoutBox& b = openSelectBox;
    const float fx = (float)b.x;
    const float fy = (float)(b.y - scrollY + topInset + b.height);
    const float fw = (float)b.width;
    const float kFont = b.fontSize > 0 ? (float)b.fontSize : kDefaultFont;
    const float rowHeight = (float)std::max(24, b.fontSize + 10);

    std::vector<Element*> opts = selectOptions(openSelect);
    Element* selected = selectedOption(openSelect);
    const float listHeight = rowHeight * (float)opts.size();

    r.drawRect(fx - 1, fy - 1, fw + 2, listHeight + 2, kCtlBorder);
    r.drawRect(fx, fy, fw, listHeight, kWhite);
    for (size_t i = 0; i < opts.size(); i++) {
        float rowY = fy + (float)i * rowHeight;
        if (opts[i] == selected) r.drawRect(fx, rowY, fw, rowHeight, kSelection);
        r.setClip(fx + 1, rowY + 1, fw - 2, rowHeight - 2);
        r.drawText(fx + kInputPad, rowY + (rowHeight - kFont) / 2, optionLabel(opts[i]), kFont, kInk);
        r.clearClip();
    }
}
