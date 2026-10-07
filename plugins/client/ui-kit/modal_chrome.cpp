#include "modal_chrome.hpp"

#include <algorithm>
#include <utility>

#include <ftxui/screen/terminal.hpp>
#include "policy.hpp"

namespace minimal::client::ui_kit {

namespace {

// What the body may take before the pinned rows, the hint line and the
// window borders stop fitting on screen: without a cap the window ran past
// the terminal's bottom edge, and the hints -- the last row -- were exactly
// what got clipped. A tiny or unknown terminal degrades to the smallest
// budget that still leaves the hint line room.
int BodyHeightBudget() {
    const int dimy = ftxui::Terminal::Size().dimy;
    constexpr int kReserved = kModalFrameRows + kModalOverlaySlackRows;
    if (dimy > kReserved + kMinModalBodyRows) return dimy - kReserved;
    return std::max(dimy - kModalFrameRows, 1);
}

// One column of breathing room each side, in the BODY color: the window's
// attention tint is for the frame and the title -- the call to act -- not
// for every word inside it.
ftxui::Element Body(const Theme& theme, ftxui::Element content) {
    return ftxui::hbox({ftxui::text(" "), std::move(content) | ftxui::flex, ftxui::text(" ")}) |
           ftxui::color(theme.ColorFor("assistant_message"));
}

}  // namespace

ftxui::Element ModalRule(const Theme& theme) {
    // Structure, not a call to act: the hairline role.
    return ftxui::separator() | ftxui::color(theme.ColorFor("border_unfocused"));
}

ftxui::Element ModalFrame(const Theme& theme, const std::string& title, ftxui::Elements rows,
                          ftxui::Elements pinned, const std::string& footer, ModalAccent accent,
                          int max_width) {
    using namespace ftxui;
    // The pinned rows (and their rule) come out of the scrolling budget, so
    // the frame as a whole still fits and the hints stay on screen.
    const int pinned_rows = pinned.empty() ? 0 : kModalPinnedMaxRows + 1;
    Elements body{vbox(std::move(rows)) | yframe | vscroll_indicator |
                  size(HEIGHT, LESS_THAN, std::max(BodyHeightBudget() - pinned_rows, 1))};
    if (!pinned.empty()) {
        body.push_back(ModalRule(theme));
        body.push_back(vbox(std::move(pinned)) | yframe |
                       size(HEIGHT, LESS_THAN, kModalPinnedMaxRows));
    }
    if (!footer.empty()) {
        body.push_back(ModalRule(theme));
        body.push_back(paragraph(footer) | Muted(theme));
    }
    return window(text(" " + title + " "), Body(theme, vbox(std::move(body)))) |
           size(WIDTH, LESS_THAN, max_width) |
           color(theme.ColorFor(accent == ModalAccent::Attention ? "attention" : "border_focused"));
}

}  // namespace minimal::client::ui_kit
