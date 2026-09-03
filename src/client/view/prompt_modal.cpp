#include "minimal/client/view/prompt_modal.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

namespace minimal::client {

std::vector<std::string> GrantToggles::GrantedKeys(const PendingPermission& request) const {
    std::vector<std::string> keys;
    const std::size_t count = std::min(selected.size(), request.grants.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (selected[i]) keys.push_back(request.grants[i].key());
    }
    return keys;
}

namespace {

using ftxui::Element;
using ftxui::Elements;

// The modal body's height budget: what the content may take before the pinned
// hint line and the window borders stop fitting on screen. Without a cap the
// modal simply ran past the terminal's bottom edge -- and the KEYBOARD HINTS,
// being the last row, were exactly what got clipped ("permission prompt lacks
// button hints"). The hints are pinned OUTSIDE the scrollable region and the
// content scrolls inside it, so they render no matter how long the summary or
// the grants list is.
int ModalBodyHeightBudget() {
    const int dimy = ftxui::Terminal::Size().dimy;
    // window borders (2) + hint line (1) + slack for the tab bar/editor the
    // modal overlays; a tiny or unknown terminal degrades to a small fixed
    // budget rather than an uncapped modal.
    return dimy > 8 ? dimy - 6 : 10;
}

// One column of breathing room on each side: window() draws its border
// flush against the content otherwise, so transcript-like text (a bash
// command, a file path) reads as if it were spilling past the frame.
Element PadHorizontal(Element content) {
    return ftxui::hbox({ftxui::text(" "), std::move(content) | ftxui::flex, ftxui::text(" ")});
}

}  // namespace

ftxui::Component MakePermissionModal(const PendingPermission& request, GrantToggles& toggles,
                                     const Theme& theme,
                                     std::function<void(bool allow)> on_answer) {
    using namespace ftxui;

    // The request is COPIED into component-owned state. RunTui passes a member
    // of a per-frame snapshot; binding a reference left the modal reading freed
    // memory on every frame after the one that built it. `theme` stays a
    // reference -- it is a RunTui local that provably outlives the modal.
    const auto request_state = std::make_shared<PendingPermission>(request);

    toggles.selected.assign(request.grants.size(), false);

    // One checkbox per offered grant. Labels carry THE WORDING OBLIGATION:
    // blanket per-binary, any arguments, this session -- a human judging
    // "allow git push --force" would be judging the wrong thing.
    Components grant_boxes;
    for (std::size_t i = 0; i < request_state->grants.size(); ++i) {
        CheckboxOption option;
        option.label = ftxui::ConstStringRef(std::string(""));
        option.checked = ftxui::Ref<bool>(&toggles.selected[i]);
        grant_boxes.push_back(Checkbox(std::move(option)));
    }
    // Copy BEFORE the move below: the first draft copied the moved-from
    // (empty) vector into the renderer capture, so the grants section's
    // `!empty()` guard hid every offer -- the modal shipped never rendering a
    // single grant, caught by the R1 harness tests.
    auto boxes_copy = grant_boxes;
    auto grant_container = Container::Vertical(std::move(grant_boxes));

    // By value for EVERYTHING the closure outlives the factory -- including
    // the on_answer parameter: a `[&]` capture of it dangles the moment the
    // factory returns, which is exactly the first-frame crash class the
    // smoke run hit in the render lambdas. (The old comment claimed `request`
    // was a safe caller-owned reference; the snapshot refactor made it a
    // per-frame temporary instead. Copy, don't trust the caller's lifetime.)
    const auto grant_container_val = grant_container;   // outlives the factory: by value
    const auto grant_boxes_val = std::move(boxes_copy);
    // Set on the first event the modal sees: before that, the scroll anchor
    // is the top of the content (the title/summary a human must read first);
    // after it, the frame follows the ACTIVE grant checkbox, so tab/arrow
    // navigation scrolls the list the user is moving through.
    const auto interacted = std::make_shared<bool>(false);
    auto body = Renderer(grant_container,
                         [grant_boxes_val, grant_container_val, request_state, &theme,
                          interacted] {
        const PendingPermission& request = *request_state;
        Elements rows;
        rows.push_back(text("permission required") | bold | color(theme.ColorFor("attention")));
        rows.push_back(text("tool: " + request.tool_name) | dim);
        rows.push_back(paragraph(request.summary));
        if (!request.complete) {
            // THE LOWER-BOUND OBLIGATION: say what the disclosure does not
            // cover, so "allow" is never read as "exactly this command".
            rows.push_back(
                paragraph("This is what the command NAMED -- it may reach more. "
                          "Allowing grants this tool, not this text.") |
                color(theme.ColorFor("error")));
        }
        // Which checkbox is active right now: the container's ActiveChild
        // compared against the boxes we hold copies of. DOM-level `focus`
        // only (like ChatView's scroll anchor) -- it steers the enclosing
        // yframe, it never steals keyboard focus.
        int active_grant = -1;
        for (int i = 0; i < static_cast<int>(grant_boxes_val.size()); ++i) {
            if (grant_container_val->ActiveChild() == grant_boxes_val[i]) {
                active_grant = i;
                break;
            }
        }
        if (!request.grants.empty()) {
            rows.push_back(text("session grants:") | dim);
            for (std::size_t i = 0; i < request.grants.size(); ++i) {
                auto row = hbox({grant_container_val->ChildAt(static_cast<int>(i))->Render(),
                                 text(" allow " + request.grants[i].label() +
                                      " -- any arguments, this session") |
                                     color(theme.ColorFor("attention"))});
                if (static_cast<int>(i) == active_grant) {
                    row |= focus;
                    row |= bgcolor(theme.ColorFor("status_bar_bg"));
                }
                rows.push_back(std::move(row));
            }
        }

        // The scroll anchor: the summary until the user touches a key, then
        // the active grant row (already marked above). One anchor at a time
        // -- two `focus` markers would fight over the frame's viewport.
        if (!(*interacted) || request.grants.empty()) {
            rows[2] |= focus;   // the summary
        }

        // The footer only advertises affordances that actually do something:
        // tab/arrow and space are dead keys on a request with no grants to
        // move between or toggle, and advertising them anyway just invites
        // a press that does nothing. Two-space separators, matching the
        // pickers' own hint lines (session_picker.cpp etc.), not the three
        // this line used to use -- also just enough narrower to keep the
        // full list on one row once interior padding (below) is in play.
        std::string footer_text = "enter: allow  ·  esc: deny";
        if (!request.grants.empty()) footer_text += "  ·  tab/arrow: move  ·  space: toggle grants";

        // Content scrolls; the keyboard hints stay pinned below it -- visible
        // no matter how much content there is (the clipped-footer bug).
        // paragraph(), not text(): with every affordance listed, this line
        // can run past the modal's own width (interior padding narrows it
        // further) -- text() would silently clip the tail (the exact "toggle
        // grants" the user needs), paragraph() wraps onto a second line.
        return window(text(" permission "),
                      PadHorizontal(vbox({
                          vbox(std::move(rows)) | yframe | vscroll_indicator |
                              size(HEIGHT, LESS_THAN, ModalBodyHeightBudget()),
                          separator(),
                          paragraph(footer_text) | dim,
                      }))) |
               // 82, not 80: PadHorizontal above spends 2 columns of the
               // modal's own width on the new interior margin -- widening
               // the cap by the same 2 keeps the footer's actual usable
               // width (and therefore its one-line layout) unchanged from
               // before the margin was added.
               size(WIDTH, LESS_THAN, 82) | color(theme.ColorFor("attention"));
    });
    // RunTui's on_answer reads toggles.GrantedKeys(request) -- the filtered
    // keys ride along from state it owns, so the modal never mints a key.
    // `on_answer` is captured BY VALUE: the CatchEvent outlives the factory
    // and a by-reference capture of the parameter was a dangling call.
    return CatchEvent(body, [on_answer, interacted](const Event& event) {
        *interacted = true;
        if (event == Event::Return) {
            on_answer(true);
            return true;
        }
        if (event == Event::Escape) {
            on_answer(false);
            return true;
        }
        return false;
    });
}

ftxui::Component MakeConfirmationModal(const PendingConfirmation& request, const Theme& theme,
                                       std::function<void(std::string chosen)> on_answer) {
    using namespace ftxui;

    // Same rule as the permission modal: the request is COPIED (RunTui passes
    // per-frame snapshot members), the callback captured by value.
    const auto request_state = std::make_shared<PendingConfirmation>(request);

    // The wire confirmations (/clear, /permission-mode) offer "Yes"/"No" with
    // no cancel option -- Escape is not an answer there (below), so the footer
    // must not advertise one.
    const bool has_cancel =
        std::find(request_state->options.begin(), request_state->options.end(),
                  "cancel") != request_state->options.end();

    Components option_components;
    for (const auto& option : request.options) {
        option_components.push_back(Button(option, [option, on_answer] { on_answer(option); }));
    }
    auto buttons = ftxui::Container::Horizontal(std::move(option_components));
    const auto buttons_val = buttons;   // outlives the factory: by value
    const auto interacted = std::make_shared<bool>(false);
    auto body = Renderer(buttons, [buttons_val, request_state, has_cancel, &theme, interacted] {
        const PendingConfirmation& request = *request_state;
        Elements rows;
        rows.push_back(text(request.title) | bold | color(theme.ColorFor("attention")));
        rows.push_back(paragraph(request.detail));
        auto button_row = buttons_val->Render() | hcenter;
        if (*interacted) button_row |= focus;   // frame follows the buttons once touched
        rows.push_back(std::move(button_row));
        // Content scrolls; the esc hint stays pinned below it. (No esc hint
        // at all when the wire request offers no cancel option -- see the
        // CatchEvent below.)
        return window(text(" confirm "),
                      PadHorizontal(vbox({
                          vbox(std::move(rows)) | yframe | vscroll_indicator |
                              size(HEIGHT, LESS_THAN, ModalBodyHeightBudget()),
                          has_cancel ? separator() : emptyElement(),
                          has_cancel ? text("esc: cancel") | dim : emptyElement(),
                      }))) |
               size(WIDTH, LESS_THAN, 72) | color(theme.ColorFor("attention"));  // see the 82 note above
    });
    // Escape picks the "cancel" option when the modal offers one: both
    // destructive confirms do, and the picker/permission modals already honor
    // Escape -- the inconsistency made the most destructive dialogs the only
    // ones without a keyboard out.
    const std::string cancel_option = "cancel";
    return CatchEvent(body, [on_answer, has_cancel, cancel_option, interacted](const Event& event) {
        *interacted = true;
        if (event == Event::Escape) {
            if (has_cancel) {
                on_answer(cancel_option);
                return true;
            }
            return false;   // no cancel option: Escape is not an answer
        }
        return false;
    });
}

}  // namespace minimal::client
