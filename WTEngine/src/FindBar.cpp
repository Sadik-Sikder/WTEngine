// FindBar.cpp
#define NOMINMAX
#include "FindBar.h"
#include "AddressBar.h"
#include "Renderer.h"
#include <GLFW/glfw3.h> // key codes only
#include <algorithm>
#include <cmath>

namespace {
    // Geometry, relative to the panel's top-left corner.
    const int kPanelW = 360;
    const int kPanelH = 38;
    const int kMarginRight = 24;  // clear of the page's scrollbar
    const int kFieldX = 8, kFieldY = 6, kFieldW = 196, kFieldH = 26;
    const int kTextPad = 6;
    const int kCountX = kFieldX + kFieldW + 8; // "3/12"
    const int kButtonW = 26;
    const int kPrevX = kPanelW - 3 * kButtonW - 12;
    const int kNextX = kPrevX + kButtonW + 2;
    const int kCloseX = kNextX + kButtonW + 6;
    const float kFont = 14;

    const wchar_t kUp[] = { 0x2191, 0 };    // upwards arrow
    const wchar_t kDown[] = { 0x2193, 0 };  // downwards arrow
    const wchar_t kClose[] = { 0x2715, 0 };

    const Color kPanelBg{ 0.97f, 0.97f, 0.98f, 1 };
    const Color kBorder{ 0.72f, 0.74f, 0.78f, 1 };
    const Color kBorderFocus{ 0.20f, 0.45f, 0.90f, 1 };
    const Color kField{ 1, 1, 1, 1 };
    const Color kInk{ 0, 0, 0, 1 };
    const Color kDim{ 0.50f, 0.50f, 0.53f, 1 };
    const Color kNoMatch{ 0.80f, 0.15f, 0.15f, 1 };
    const Color kSelection{ 0.68f, 0.82f, 1.0f, 1 };

    int panelX(int windowWidth) { return std::max(windowWidth - kPanelW - kMarginRight, 0); }
    const int kPanelY = AddressBar::kHeight + 6;
}

bool FindBar::contains(int x, int y, int w) const {
    int px = panelX(w);
    return open_ && x >= px && x < px + kPanelW && y >= kPanelY && y < kPanelY + kPanelH;
}

FindBar::Action FindBar::onClick(int x, int y, int w, Renderer& r, double now) {
    int lx = x - panelX(w), ly = y - kPanelY;
    bool onButtonRow = ly >= kFieldY && ly < kFieldY + kFieldH;
    if (onButtonRow && lx >= kPrevX && lx < kPrevX + kButtonW) return Action::Previous;
    if (onButtonRow && lx >= kNextX && lx < kNextX + kButtonW) return Action::Next;
    if (onButtonRow && lx >= kCloseX && lx < kCloseX + kButtonW) { close(); return Action::Close; }

    focused_ = true;
    if (lx >= kFieldX && lx < kFieldX + kFieldW) {
        bool isDouble = input_.registerClick(x, now);
        float localX = lx - (kFieldX + kTextPad) + scrollX_;
        if (isDouble) input_.selectWordAt(localX, r, kFont);
        else input_.placeCaretAt(localX, r, kFont);
    }
    return Action::None;
}

FindBar::Action FindBar::onChar(unsigned int codepoint) {
    input_.onChar(codepoint);
    return Action::Edited;
}

FindBar::Action FindBar::insert(const std::wstring& s) {
    input_.insert(s);
    return Action::Edited;
}

FindBar::Action FindBar::onKey(int key, bool shift) {
    if (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER) return shift ? Action::Previous : Action::Next;
    if (key == GLFW_KEY_ESCAPE) { close(); return Action::Close; }
    std::wstring before = input_.text();
    input_.onEditKey(key);
    return input_.text() != before ? Action::Edited : Action::None;
}

void FindBar::draw(Renderer& r, int w, int current, int count, double t) {
    if (!open_) return;
    const float px = (float)panelX(w), py = (float)kPanelY;

    // Panel with a border and a soft shadow line under it
    r.drawRect(px - 1, py - 1, (float)kPanelW + 2, (float)kPanelH + 2, kBorder);
    r.drawRect(px, py + kPanelH + 1, (float)kPanelW, 2, Color{ 0, 0, 0, 0.08f });
    r.drawRect(px, py, (float)kPanelW, (float)kPanelH, kPanelBg);

    // Search field
    const float fx = px + kFieldX, fy = py + kFieldY;
    r.drawRect(fx - 1, fy - 1, (float)kFieldW + 2, (float)kFieldH + 2, focused_ ? kBorderFocus : kBorder);
    r.drawRect(fx, fy, (float)kFieldW, (float)kFieldH, kField);

    const std::wstring& text = input_.text();
    const float viewW = (float)(kFieldW - 2 * kTextPad);
    float caretX = r.measureText(text.substr(0, input_.cursor()), kFont);
    if (caretX - scrollX_ > viewW) scrollX_ = caretX - viewW;
    if (caretX - scrollX_ < 0) scrollX_ = caretX;

    r.setClip(fx, fy, (float)kFieldW, (float)kFieldH);
    const float tx = fx + kTextPad - scrollX_, ty = fy + 4;
    if (text.empty()) {
        r.drawText(fx + kTextPad, ty, L"Find in page", kFont, kDim);
    } else {
        if (input_.hasSelection()) {
            float x0 = r.measureText(text.substr(0, input_.selLow()), kFont);
            float x1 = r.measureText(text.substr(0, input_.selHigh()), kFont);
            r.drawRect(tx + x0, fy + 3, x1 - x0, (float)kFieldH - 6, kSelection);
        }
        r.drawText(tx, ty, text, kFont, kInk);
    }
    if (focused_ && !input_.hasSelection() && std::fmod(t, 1.0) < 0.6)
        r.drawRect(tx + caretX, fy + 4, 1.5f, (float)kFieldH - 8, kInk);
    r.clearClip();

    // "3/12" - red "0/0" when there's a query with no match
    std::wstring countText = std::to_wstring(current + 1) + L"/" + std::to_wstring(count);
    if (count == 0) countText = L"0/0";
    r.drawText(px + kCountX, fy + 4, countText, kFont, (count == 0 && !text.empty()) ? kNoMatch : kDim);

    // Previous / next / close
    auto button = [&](int bx, const wchar_t* glyph, bool enabled) {
        float x = px + bx;
        float gw = r.measureText(glyph, 15);
        r.drawText(x + (kButtonW - gw) / 2, fy + 3, glyph, 15, enabled ? kInk : kDim);
    };
    button(kPrevX, kUp, count > 0);
    button(kNextX, kDown, count > 0);
    button(kCloseX, kClose, true);
}
