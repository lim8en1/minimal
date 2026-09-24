#include "minimal/client/view/prompt_modal.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
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

// A modal's internal divider, in the hairline role rather than the window's
// `attention` tint -- dividers are structure, not a call to act.
Element Rule(const Theme& theme) {
    return ftxui::separator() | ftxui::color(theme.ColorFor("border_unfocused"));
}

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
    // modal overlays; a tiny or unknown terminal degrades to the smallest
    // budget that still leaves room for the pinned hint line, rather than a
    // fixed value that could exceed dimy itself and clip the hints again --
    // the exact regression this cap exists to prevent, just at small sizes.
    if (dimy > 8) return dimy - 6;
    return std::max(dimy - 3, 1);
}

// One column of breathing room on each side: window() draws its border
// flush against the content otherwise, so transcript-like text (a bash
// command, a file path) reads as if it were spilling past the frame.
Element PadHorizontal(Element content) {
    return ftxui::hbox({ftxui::text(" "), std::move(content) | ftxui::flex, ftxui::text(" ")});
}

// A modal's body: padded, and in the BODY color. The window's `attention`
// tint is for the frame and the title -- the call to act -- not for every
// word inside it: an all-amber window left the summary, the hints and the
// answers with no hierarchy. Explicitly colored rows (the title line, the
// grant labels, muted hints) keep their own color; color() nests.
Element ModalBody(const Theme& theme, Element content) {
    return PadHorizontal(std::move(content)) | ftxui::color(theme.ColorFor("assistant_message"));
}

}  // namespace

namespace {

// The answer list, numbered 1-4 in this order (PermissionChoice order).
struct PermissionChoiceRow {
    PermissionChoice choice;
    const char* label;
    const char* hint;
};
constexpr PermissionChoiceRow kPermissionChoices[] = {
    {PermissionChoice::Allow, "allow", "run it this once"},
    {PermissionChoice::Deny, "deny", "the assistant is told no and carries on"},
    {PermissionChoice::DenyWithMessage, "deny with message",
     "tell the assistant what to do instead"},
    {PermissionChoice::DenyAndStop, "deny and stop", "end the turn here"},
};
constexpr int kPermissionChoiceCount =
    static_cast<int>(sizeof(kPermissionChoices) / sizeof(kPermissionChoices[0]));

// Ephemeral modal state, component-owned (shared by the renderer and the
// event handler) -- the same lifetime rule as the request copy below.
//
// ONE unified cursor drives both the grant checkboxes and the answer list:
// [0, grant_count) is a grant row, [grant_count, grant_count+4) an answer.
// The two used to be independent (an FTXUI Container::Vertical's own
// internal selection for the checkboxes, a plain int here for the answers),
// reachable by disjoint input: arrows moved the answer only and never
// reached the checkboxes, while the checkboxes were Tab+Space/mouse-only and
// the answer rows had no hit box at all. A single cursor and a single
// CatchEvent (below) drive both keyboard and mouse for the whole modal, so
// every row is reachable every way.
struct PermissionModalState {
    int cursor = 0;            // set to grant_count (Allow) once the request is known
    bool composing = false;    // the deny-with-message input is open
    bool interacted = false;   // the arming rule (see the CatchEvent)
    std::string message;
    // Per-row hit boxes, captured by `reflect()` during Render and read back
    // by the SAME CatchEvent on the next mouse event -- one path drives both
    // input methods, rather than delegating mouse handling to FTXUI's own
    // Checkbox components (which is what left the answer rows unclickable:
    // plain text has no hit box of its own).
    std::vector<ftxui::Box> grant_boxes;                                    // sized to grant_count
    std::vector<ftxui::Box> answer_boxes{std::size_t{kPermissionChoiceCount}};
};

std::string TrimmedMessage(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

}  // namespace

ftxui::Component MakePermissionModal(const PendingPermission& request, GrantToggles& toggles,
                                     const Theme& theme,
                                     std::function<void(PermissionVerdict verdict)> on_answer) {
    using namespace ftxui;

    // The request is COPIED into component-owned state. RunTui passes a member
    // of a per-frame snapshot; binding a reference left the modal reading freed
    // memory on every frame after the one that built it. `theme` stays a
    // reference -- it is a RunTui local that provably outlives the modal.
    const auto request_state = std::make_shared<PendingPermission>(request);
    const auto state = std::make_shared<PermissionModalState>();
    state->grant_boxes.assign(request.grants.size(), ftxui::Box{});
    // Default cursor: the first ANSWER (Allow), not the first grant -- a
    // grant is an addition to Allow, never the default action itself.
    state->cursor = static_cast<int>(request.grants.size());

    // The deny-with-message line. Deliberately OUTSIDE the component tree:
    // the CatchEvent below forwards keys to it only while composing, so it can
    // never take focus (or keystrokes) from the list while it is not shown --
    // the "hidden choice" bug class the ask modal's input guards against.
    // Single-line: Return is the send key, never a newline.
    InputOption input_option;
    input_option.multiline = false;
    const auto input =
        Input(&state->message, "what should the assistant do instead?", input_option);

    toggles.selected.assign(request.grants.size(), false);

    // By value for EVERYTHING the closure outlives the factory -- including
    // the on_answer parameter: a `[&]` capture of it dangles the moment the
    // factory returns, which is exactly the first-frame crash class the
    // smoke run hit in the render lambdas. (The old comment claimed `request`
    // was a safe caller-owned reference; the snapshot refactor made it a
    // per-frame temporary instead. Copy, don't trust the caller's lifetime.)
    //
    // NO checkbox components: a grant row draws its own "[x]"/"[ ]" glyph
    // from `toggles.selected[i]` and reflects its own hit box, same as an
    // answer row below -- one rendering shape for every row, one CatchEvent
    // (below) driving all of them. This is also what makes "exactly one
    // focus anchor" true: FTXUI's Checkbox unconditionally tags its own
    // render with a DOM `focus` marker regardless of which one is current
    // (vendored ftxui/component/checkbox.cpp), so N checkboxes meant N
    // competing anchors inside the same scrolling yframe; with no Checkbox
    // components at all, the one `|= focus` applied below is the only one
    // that exists.
    auto body = Renderer([request_state, state, input, &toggles, &theme] {
        const PendingPermission& request = *request_state;
        const int grant_count = static_cast<int>(request.grants.size());
        Elements rows;
        rows.push_back(text("permission required") | bold | color(theme.ColorFor("attention")));
        rows.push_back(text("tool: " + request.tool_name) | Muted(theme));
        rows.push_back(paragraph(request.summary));  // rows[2]: the scroll anchor below
        if (!request.reason.empty()) {
            // Which rule is asking -- the answer to "why am I being asked
            // this?", and the line to edit to stop being asked.
            rows.push_back(paragraph("asking because " + request.reason) | Muted(theme));
        }
        if (!request.complete) {
            // THE LOWER-BOUND OBLIGATION: say what the disclosure does not
            // cover, so "allow" is never read as "exactly this command".
            rows.push_back(
                paragraph("This is what the command NAMED — it may reach more. "
                          "Allowing grants this tool, not this text.") |
                color(theme.ColorFor("error")));
        }

        // cursor_row tracks whichever row the unified cursor currently
        // points at, grant or answer alike -- there is exactly one, so
        // there is exactly one candidate for the `|= focus` applied below.
        int cursor_row = -1;
        if (grant_count > 0) {
            rows.push_back(text("session grants (sent only with allow):") | Muted(theme));
            for (int i = 0; i < grant_count; ++i) {
                const bool checked =
                    static_cast<std::size_t>(i) < toggles.selected.size() && toggles.selected[i];
                auto row = hbox({text(checked ? "[x] " : "[ ] "),
                                 text("allow " + request.grants[i].label() +
                                      " · this session") |
                                     color(theme.ColorFor("attention"))});
                row |= reflect(state->grant_boxes[i]);
                if (i == state->cursor) {
                    row |= bgcolor(theme.ColorFor("status_bar_bg"));
                    cursor_row = static_cast<int>(rows.size());
                }
                rows.push_back(std::move(row));
            }
        }

        // The answer list. Highlighting is shown regardless of `interacted`
        // (Allow reads as the default from the first frame); only the
        // FOCUS/scroll target is conditioned on it, below.
        rows.push_back(text(""));
        for (int i = 0; i < kPermissionChoiceCount; ++i) {
            const auto& choice = kPermissionChoices[i];
            const bool is_selected = grant_count + i == state->cursor;
            std::string hint = choice.hint;
            if (choice.choice == PermissionChoice::Allow && !request.grants.empty()) {
                hint = "this once, plus any grants ticked above";
            }
            // Marker and label bold TOGETHER: one styled span, so the marker
            // reads as part of the selected answer.
            auto label = hbox({text(is_selected ? "❯ " : "  "),
                               text(std::to_string(i + 1) + ". " + choice.label)});
            if (is_selected) label |= bold;
            auto row = hbox({std::move(label), text("  " + hint) | Muted(theme)});
            row |= reflect(state->answer_boxes[static_cast<std::size_t>(i)]);
            rows.push_back(std::move(row));
            if (is_selected) cursor_row = static_cast<int>(rows.size()) - 1;
        }
        int message_row = -1;
        if (state->composing) {
            message_row = static_cast<int>(rows.size());
            rows.push_back(hbox({text("   message: "), input->Render() | flex}));
        }

        // The scroll anchor: the summary until the user touches a key, then
        // whatever the cursor points at -- a grant or an answer, or the
        // message line while composing. One anchor at a time: two `focus`
        // markers would fight over the frame's viewport.
        if (!state->interacted) {
            rows[2] |= focus;   // the summary
        } else if (message_row >= 0) {
            rows[message_row] |= focus;
        } else if (cursor_row >= 0) {
            rows[cursor_row] |= focus;
        }

        // The footer only advertises affordances that actually do something:
        // tab is a dead key on a request with no grants to jump to, and
        // advertising it anyway just invites a press that does nothing.
        // Two-space separators, matching the pickers' own hint lines
        // (session_picker.cpp etc.).
        std::string footer_text;
        if (state->composing) {
            footer_text = "enter: deny with this message  ·  esc: back to the list";
        } else if (grant_count > 0) {
            // Kept under the modal's 78 usable columns: a wrap could split an
            // affordance across two lines.
            footer_text = "↑/↓ 1-4: choose  ·  space/enter: pick  ·  tab: grants/answers  ·  "
                          "esc: deny";
        } else {
            footer_text = "↑/↓ 1-4: choose  ·  enter: confirm  ·  esc: deny";
        }

        // Content scrolls; the keyboard hints stay pinned below it -- visible
        // no matter how much content there is (the clipped-footer bug).
        // paragraph(), not text(): with every affordance listed, this line
        // can run past the modal's own width (interior padding narrows it
        // further) -- text() would silently clip the tail (the exact "toggle
        // grants" the user needs), paragraph() wraps onto a second line.
        return window(text(" permission "),
                      ModalBody(theme, vbox({
                          vbox(std::move(rows)) | yframe | vscroll_indicator |
                              size(HEIGHT, LESS_THAN, ModalBodyHeightBudget()),
                          Rule(theme),
                          paragraph(footer_text) | Muted(theme),
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
    // `toggles` is captured BY REFERENCE deliberately: it is RunTui's own
    // GrantToggles, the same object on_answer's closure (built by the caller)
    // reads via GrantedKeys -- a copy here would toggle a box nobody reads.
    return CatchEvent(body, [on_answer, state, input, request_state, &toggles](Event event) {
        if (event == Event::CtrlC) return false;   // the root's turn-cancel

        if (state->composing) {
            if (event == Event::Escape) {
                state->composing = false;   // back to the list; the text survives
                return true;
            }
            if (event == Event::Return) {
                std::string message = TrimmedMessage(state->message);
                if (message.empty()) {
                    on_answer({PermissionChoice::Deny, {}});
                } else {
                    on_answer({PermissionChoice::DenyWithMessage, std::move(message)});
                }
                return true;
            }
            // Everything else is typing.
            input->OnEvent(event);
            return true;
        }

        const int grant_count = static_cast<int>(request_state->grants.size());
        const int total = grant_count + kPermissionChoiceCount;

        // A CLICK is a deliberate, aimed action -- unlike a keystroke landing
        // in the same input batch as the request's own arrival (a paste's
        // trailing newline, a fast typist's stray key), there is no realistic
        // way to click a row by accident before having seen it. It acts
        // immediately, bypassing the arming rule below. Hover/motion/other
        // buttons are ignored outright: THE BUG this replaces -- any mouse
        // event, including a passive move, used to satisfy the arming rule,
        // so a stray Return arriving right after could answer an unread
        // prompt.
        if (event.is_mouse()) {
            const auto& mouse = event.mouse();
            if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed) return false;
            for (int i = 0; i < grant_count; ++i) {
                if (!state->grant_boxes[static_cast<std::size_t>(i)].Contain(mouse.x, mouse.y)) {
                    continue;
                }
                state->interacted = true;
                state->cursor = i;
                toggles.selected[static_cast<std::size_t>(i)] =
                    !toggles.selected[static_cast<std::size_t>(i)];
                return true;
            }
            for (int i = 0; i < kPermissionChoiceCount; ++i) {
                if (!state->answer_boxes[static_cast<std::size_t>(i)].Contain(mouse.x, mouse.y)) {
                    continue;
                }
                state->interacted = true;
                state->cursor = grant_count + i;
                switch (kPermissionChoices[i].choice) {
                    case PermissionChoice::Allow: on_answer({PermissionChoice::Allow, {}}); break;
                    case PermissionChoice::Deny: on_answer({PermissionChoice::Deny, {}}); break;
                    case PermissionChoice::DenyWithMessage: state->composing = true; break;
                    case PermissionChoice::DenyAndStop:
                        on_answer({PermissionChoice::DenyAndStop, {}});
                        break;
                }
                return true;
            }
            return false;   // click missed every row
        }

        // THE ARMING RULE, keyboard only: the first key this modal ever sees
        // only arms it -- it never itself answers or toggles a grant. Escape
        // is the one exception (denying on an unseen prompt is always the
        // safe outcome). A Return, digit or space that lands in the same
        // input batch as the request's own arrival must not read as an
        // answer to a prompt the user has not seen yet.
        const bool armed = state->interacted;
        state->interacted = true;

        if (event == Event::Escape) {
            on_answer({PermissionChoice::Deny, {}});
            return true;
        }
        if (event == Event::ArrowUp || event == Event::ArrowDown) {
            const int step = event == Event::ArrowUp ? -1 : 1;
            state->cursor = std::clamp(state->cursor + step, 0, total - 1);
            return true;
        }
        if (event == Event::Tab || event == Event::TabReverse) {
            // Jumps to the START of the other section -- the fast way across
            // a long grants list -- rather than stepping row by row. A quiet
            // no-op with nothing to jump to (no grants offered): the footer
            // already does not advertise it in that case.
            if (grant_count > 0) {
                state->cursor = state->cursor < grant_count ? grant_count : 0;
            }
            return true;
        }

        // Space and Enter act on whatever the cursor currently points at:
        // toggle a grant, or pick an answer. Digits pick an answer directly,
        // moving the cursor onto it first.
        int pick = -1;             // an answer index, once resolved below
        bool toggle_grant = false;
        if (event == Event::Return || event == Event::Character(" ")) {
            if (state->cursor < grant_count) {
                toggle_grant = true;
            } else {
                pick = state->cursor - grant_count;
            }
        } else if (event.is_character() && event.character().size() == 1) {
            const char c = event.character()[0];
            if (c >= '1' && c < '1' + kPermissionChoiceCount) pick = c - '1';
        }

        if (toggle_grant) {
            if (!armed) return true;   // arms; never toggles on the reveal itself
            const auto idx = static_cast<std::size_t>(state->cursor);
            if (idx < toggles.selected.size()) toggles.selected[idx] = !toggles.selected[idx];
            return true;
        }
        if (pick < 0) return false;   // an unrecognized key -- nothing left to forward it to

        state->cursor = grant_count + pick;
        if (!armed) return true;   // arms (and shows the pick), never answers

        switch (kPermissionChoices[pick].choice) {
            case PermissionChoice::Allow:
                on_answer({PermissionChoice::Allow, {}});
                break;
            case PermissionChoice::Deny:
                on_answer({PermissionChoice::Deny, {}});
                break;
            case PermissionChoice::DenyWithMessage:
                state->composing = true;   // answered on the input's Enter
                break;
            case PermissionChoice::DenyAndStop:
                on_answer({PermissionChoice::DenyAndStop, {}});
                break;
        }
        return true;
    });
}

ftxui::Component MakeConfirmationModal(const PendingConfirmation& request, const Theme& theme,
                                       std::function<void(std::string chosen)> on_answer) {
    using namespace ftxui;

    // Same rule as the permission modal: the request is COPIED (RunTui passes
    // per-frame snapshot members), the callback captured by value.
    const auto request_state = std::make_shared<PendingConfirmation>(request);

    // The wire confirmations (/clear) offer "Yes"/"No" with
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
    // Default keyboard focus off the destructive option: Container defaults
    // ActiveChild() to index 0, and the real call sites (close/quit's
    // "close anyway", the picker's "delete") put the destructive action
    // FIRST -- a reflexive Enter as the modal opens fired it with no arming
    // delay. Options with no cancel choice (the server's Yes/No) have no
    // single safe default and are left as index 0, unchanged.
    if (has_cancel) {
        for (std::size_t i = 0; i < request_state->options.size(); ++i) {
            if (request_state->options[i] == "cancel") {
                buttons_val->SetActiveChild(buttons_val->ChildAt(static_cast<int>(i)));
                break;
            }
        }
    }
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
                      ModalBody(theme, vbox({
                          vbox(std::move(rows)) | yframe | vscroll_indicator |
                              size(HEIGHT, LESS_THAN, ModalBodyHeightBudget()),
                          has_cancel ? Rule(theme) : emptyElement(),
                          has_cancel ? text("esc: cancel") | Muted(theme) : emptyElement(),
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

// --- Ask modal (tool-ask plugin) --------------------------------------------

namespace {

// Ephemeral per-question UI state, owned by the component itself (shared_ptr,
// captured by every closure): the modal is rebuilt only when request_id
// changes, and RunTui passes per-frame snapshot members -- so, exactly like
// the permission modal's request copy, the state must outlive the factory
// call that built the checkboxes pointing into it.
struct AskModalState {
    struct QuestionState {
        // Entry strings: option labels only (descriptions render separately,
        // wrapped, via each control's transform -- see WrappedOptionEntry).
        // Stored here (not built per frame) because ftxui's
        // ConstStringRef/ConstStringListRef borrow the storage.
        std::vector<std::string> entries;
        // multiple: one stable bool per option (deque, not vector<bool>:
        // the checkboxes need stable bool* -- same rule as GrantToggles).
        std::deque<bool> boxes;
        // single: the radiobox's selected index, -1 = nothing picked.
        int radio = -1;
        // FTXUI's Radiobox clamps *selected from -1 to 0 the first time it
        // RENDERS, with no user interaction -- so `radio >= 0` alone cannot
        // distinguish "the user picked option 0" from "the user never
        // touched this question." Only on_change (fired by an actual
        // selection change, not the render-time clamp) sets this.
        bool radio_touched = false;
        // "Other" is not a separate flag: it is the TRAILING entry of
        // `entries`/`boxes` (or `radio == options_size()`), appended when
        // the question's allow_other is set -- see MakeAskModal. Keeping it
        // in the same list as the real options makes exclusivity structural
        // (a Radiobox only ever has one selected index; a checked "other"
        // checkbox and a checked real checkbox are cross-cleared by index,
        // not by a second independent bool that could disagree with them).
        std::string other_text;
    };
    std::vector<QuestionState> questions;
    // The two-phase submit: false is editing (widgets live); true is the
    // frozen review list -- Enter confirms, Escape goes back to editing.
    bool reviewing = false;
};

v1::AskUserResponse CollectAnswer(const PendingAsk& request, const AskModalState& state) {
    v1::AskUserResponse response;
    response.set_request_id(request.request_id);
    response.set_answered(true);
    for (int i = 0; i < request.request.questions_size(); ++i) {
        const auto& question = request.request.questions(i);
        const auto& qs = state.questions[i];
        auto* slot = response.add_answers();
        if (question.multiple()) {
            // The trailing "other" checkbox (see QuestionState) wins only
            // while it is itself checked -- cross-clearing at the widget
            // layer (MakeAskModal) keeps it and the real boxes from ever
            // being true at once, so this is a read of that invariant, not
            // an independent tie-break.
            if (question.allow_other() && !qs.boxes.empty() && qs.boxes.back() &&
                !qs.other_text.empty()) {
                slot->set_other(qs.other_text);
                continue;
            }
            for (int j = 0; j < question.options_size(); ++j) {
                if (qs.boxes[j]) slot->add_selected(question.options(j).label());
            }
        } else {
            // The trailing "other" radio entry (index == options_size()):
            // Radiobox's own single-selection means `radio` can never be
            // both a real index and the other index at once.
            const int other_index = question.options_size();
            if (qs.radio_touched && qs.radio == other_index) {
                if (!qs.other_text.empty()) slot->set_other(qs.other_text);
            } else if (qs.radio_touched && qs.radio >= 0 && qs.radio < other_index) {
                slot->add_selected(question.options(qs.radio).label());
            }
        }
    }
    return response;
}

v1::AskUserResponse DismissedAnswer(const PendingAsk& request) {
    v1::AskUserResponse response;
    response.set_request_id(request.request_id);
    response.set_answered(false);
    return response;
}

// One review-screen line for ONE question, mirroring CollectAnswer's slot
// logic exactly -- whatever the review shows is what submission will send,
// so a drift between the two would display one thing and answer another.
std::string ReviewLine(const v1::AskQuestion& question,
                       const AskModalState::QuestionState& qs) {
    if (question.multiple()) {
        if (question.allow_other() && !qs.boxes.empty() && qs.boxes.back() &&
            !qs.other_text.empty()) {
            return "-> your answer: " + qs.other_text;
        }
        std::string joined;
        for (int j = 0; j < question.options_size() &&
                        j < static_cast<int>(qs.boxes.size());
             ++j) {
            if (!qs.boxes[j]) continue;
            if (!joined.empty()) joined += ", ";
            joined += question.options(j).label();
        }
        return joined.empty() ? "(not answered)" : "-> " + joined;
    }
    const int other_index = question.options_size();
    if (qs.radio_touched && qs.radio == other_index) {
        return qs.other_text.empty() ? "(not answered)" : "-> your answer: " + qs.other_text;
    }
    if (qs.radio_touched && qs.radio >= 0 && qs.radio < other_index) {
        return "-> " + question.options(qs.radio).label();
    }
    return "(not answered)";
}

}  // namespace

// Whether ONE option row of a single-pick (radio) question wears the
// "current choice" styling (bold label + selection highlight). An untouched
// question wears it NOWHERE: FTXUI's Radiobox clamps its selected index from
// -1 to 0 at first render, so without this gate option 0 renders
// bold/highlighted before any interaction -- which reads as a pre-picked
// answer the submission would not send (CollectAnswer/ReviewLine gate on
// radio_touched; this is that same contract, in the render). Pure so the
// rule is directly testable (tests/prompt_modal_test.cpp).
bool RadioRowSelected(bool touched, bool active, bool focused) {
    return touched && (active || focused);
}

// One option row: marker, then the label wrapped, then its description
// wrapped and dimmed beneath it -- both hanging under the label column
// instead of running off the modal's fixed width like a bare text() would.
// Mirrors RadioboxOption::Simple()/CheckboxOption::Simple() styling (bold
// when active, highlighted when focused) so the only visible change is wrapping.
// `touched` gates the choice styling through RadioRowSelected: a radio list
// the user has never interacted with wears no "current choice" look (see the
// header doc); checkbox lists always pass true -- their markers, not the
// focus, are what says "picked".
ftxui::Element WrappedOptionEntry(const ftxui::EntryState& s, const char* marker_on,
                                  const char* marker_off, const std::string& description,
                                  const ftxui::Decorator& selection,
                                  const ftxui::Decorator& muted, bool touched) {
    using namespace ftxui;
    Element marker = text(s.state ? marker_on : marker_off);
    Element label = paragraph(s.label);
    if (RadioRowSelected(touched, s.active, s.focused)) {
        label |= bold;
        label |= selection;
    }
    Elements column = {label};
    if (!description.empty()) column.push_back(paragraph(description) | muted);
    return hbox({marker, vbox(std::move(column)) | flex});
}

ftxui::Component MakeAskModal(const PendingAsk& request, const Theme& theme,
                              std::function<void(v1::AskUserResponse response)> on_answer) {
    using namespace ftxui;

    // Same rule as the other two modals: the request is COPIED, the callback
    // captured by value -- both are per-frame snapshot members.
    const auto request_state = std::make_shared<PendingAsk>(request);
    const auto state = std::make_shared<AskModalState>();
    state->questions.resize(request_state->request.questions_size());
    const ftxui::Decorator selection = SelectionStyle(theme);
    const ftxui::Decorator muted = Muted(theme);

    Components question_components;
    for (int i = 0; i < request_state->request.questions_size(); ++i) {
        const auto& question = request_state->request.questions(i);
        auto& qs = state->questions[i];

        // ONE container child per question (this block): the renderer below
        // addresses children as ChildAt(question index). The first draft
        // pushed the allow_other rows as SEPARATE children of `all`, so with
        // allow_other set -- which is always, it is not model-facing -- every
        // question after the first rendered the PREVIOUS question's other
        // block: questions 2..n existed in the container (focusable, so Tab
        // kept landing somewhere) but never rendered.
        Components block;

        // "Type your own answer" is a TRAILING ENTRY of the same list
        // (radio or checkbox), not a separate sibling control -- a sibling
        // checkbox with its own independent bool let a real pick and "other"
        // both read as marked at once, with CollectAnswer/ReviewLine silently
        // picking a winner the editing view never showed. Folding it into
        // the list makes exclusivity structural for the radio case (one
        // Radiobox, one selected index) and at least keeps one consistent
        // marker style for the checkbox case (exclusivity there is still
        // cross-cleared by index below -- Checkbox has no built-in
        // single-selection). The free-text Input is still a separate
        // sibling component (neither Radiobox nor Checkbox can host an
        // inline child): DetachAllChildren/Add toggles it into `other_container`
        // from whichever on_change below currently owns "other"-ness, so an
        // invisible-but-focusable Input is never left in the tree (the same
        // "hidden choice" bug class the old sibling-checkbox design already
        // guarded against).
        const bool other_allowed = question.allow_other();
        auto other_container = Container::Vertical({});
        auto other_input = Input(&qs.other_text, "your answer");
        const auto state_ref = state;   // shared_ptr copy: outlives the closures below
        const int index = i;

        if (question.multiple()) {
            for (const auto& option : question.options()) {
                qs.entries.push_back(option.label());
                qs.boxes.push_back(false);
            }
            int other_j = -1;
            if (other_allowed) {
                qs.entries.push_back("Type your own answer");
                qs.boxes.push_back(false);
                other_j = static_cast<int>(qs.entries.size()) - 1;
            }
            Components boxes;
            for (std::size_t j = 0; j < qs.entries.size(); ++j) {
                const bool is_other = static_cast<int>(j) == other_j;
                CheckboxOption option_state;
                option_state.label = ConstStringRef(qs.entries[j]);
                option_state.checked = Ref<bool>(&qs.boxes[j]);
                // Checkbox's own EntryState.index is always -1 (it renders
                // itself, not a list), so the description is baked into the
                // closure by j rather than looked up through the entry state.
                // The trailing "other" row has none of its own.
                const std::string description =
                    is_other ? "" : question.options(static_cast<int>(j)).description();
                option_state.transform = [description, selection, muted](const EntryState& s) {
                    // touched=true, always: a checkbox's picked-ness lives in
                    // its own marker, not the focus styling.
                    return WrappedOptionEntry(s, "▣ ", "☐ ", description, selection, muted,
                                              /*touched=*/true);
                };
                if (is_other) {
                    option_state.on_change = [other_container, other_input, state_ref, index] {
                        auto& qs2 = state_ref->questions[index];
                        other_container->DetachAllChildren();
                        if (qs2.boxes.back()) {
                            // Checking "other" cross-clears every real box --
                            // the wire contract (WireAskBroker::SlotIsValid)
                            // never accepts a selection alongside free text.
                            for (std::size_t k = 0; k + 1 < qs2.boxes.size(); ++k) {
                                qs2.boxes[k] = false;
                            }
                            other_container->Add(other_input);
                        }
                    };
                } else {
                    option_state.on_change = [j, other_container, state_ref, index,
                                              other_allowed] {
                        if (!other_allowed) return;
                        auto& qs2 = state_ref->questions[index];
                        if (qs2.boxes[j]) {
                            qs2.boxes.back() = false;
                            other_container->DetachAllChildren();
                        }
                    };
                }
                boxes.push_back(Checkbox(std::move(option_state)));
            }
            block.push_back(Container::Vertical(std::move(boxes)));
        } else {
            // Radiobox is the single-pick control: one stable index, no
            // manual "uncheck the others" bookkeeping -- which is exactly
            // why "other" lives as entries.back() here rather than a sibling
            // checkbox: Radiobox enforces "at most one selected" for free,
            // so a real pick and "other" can never both be current.
            std::vector<std::string> entries;
            for (const auto& option : question.options()) {
                entries.push_back(option.label());
            }
            const int other_index = static_cast<int>(entries.size());
            if (other_allowed) entries.push_back("Type your own answer");
            qs.entries = std::move(entries);
            RadioboxOption radio_state;
            radio_state.entries = ConstStringListRef(&qs.entries);
            radio_state.selected = &qs.radio;
            radio_state.on_change = [state_ref, index, other_container, other_input,
                                     other_index] {
                auto& qs2 = state_ref->questions[index];
                qs2.radio_touched = true;
                other_container->DetachAllChildren();
                if (qs2.radio == other_index) other_container->Add(other_input);
            };
            // Radiobox's EntryState.index tracks the list position, so the
            // description can be looked up straight from the question --
            // except for the trailing "other" entry, which has none.
            // touched reads radio_touched LIVE (through the same shared_ptr
            // on_change writes): the first arrow-down both records the touch
            // and re-styles the list within the same repaint, so the clamp's
            // option-0 highlight appears only once the cursor has actually
            // moved.
            radio_state.transform = [request_state, state_ref, index, selection, muted,
                                     other_index](const EntryState& s) {
                const std::string description =
                    s.index == other_index
                        ? ""
                        : request_state->request.questions(index).options(s.index).description();
                return WrappedOptionEntry(s, "◉ ", "○ ", description, selection, muted,
                                          state_ref->questions[index].radio_touched);
            };
            block.push_back(Radiobox(std::move(radio_state)));
        }

        if (other_allowed) block.push_back(other_container);

        question_components.push_back(Container::Vertical(std::move(block)));
    }
    auto all = Container::Vertical(std::move(question_components));
    const auto all_val = all;   // outlives the factory: by value
    const auto interacted = std::make_shared<bool>(false);

    auto body = Renderer(all, [all_val, request_state, state, &theme, interacted] {
        const PendingAsk& ask = *request_state;
        Elements rows;
        rows.push_back(text("the assistant asks") | bold | color(theme.ColorFor("attention")));
        if (state->reviewing) {
            // The frozen review list: one line per question, wording shared
            // with CollectAnswer (see ReviewLine). Nothing here is editable
            // -- the CatchEvent below freezes input -- so Enter is a genuine
            // confirmation of what will be sent, not another way to type.
            int first_line = -1;
            for (int i = 0; i < ask.request.questions_size(); ++i) {
                const auto& question = ask.request.questions(i);
                if (i > 0) rows.push_back(Rule(theme));
                const std::string name =
                    question.header().empty()
                        ? "Question " + std::to_string(i + 1)
                        : question.header();
                rows.push_back(text(name) | bold);
                rows.push_back(paragraph(ReviewLine(question, state->questions[i])));
                if (first_line < 0) first_line = static_cast<int>(rows.size()) - 1;
            }
            if (!(*interacted) && first_line >= 0) {
                rows[first_line] |= focus;   // the viewport follows the review list
            }
            return window(text(" question "),
                          ModalBody(theme, vbox({
                              vbox(std::move(rows)) | yframe | vscroll_indicator |
                                  size(HEIGHT, LESS_THAN, ModalBodyHeightBudget()),
                              Rule(theme),
                              paragraph("enter: submit  ·  esc: edit answers") | Muted(theme),
                          }))) |
                   size(WIDTH, LESS_THAN, 82) | color(theme.ColorFor("attention"));
        }
        for (int i = 0; i < ask.request.questions_size(); ++i) {
            const auto& question = ask.request.questions(i);
            if (i > 0) rows.push_back(Rule(theme));
            if (!question.header().empty()) {
                rows.push_back(text(question.header()) | bold);
            }
            rows.push_back(paragraph(question.question()));
            rows.push_back(all_val->ChildAt(i)->Render());
        }
        if (!(*interacted)) {
            rows[0] |= focus;   // the title anchors the viewport until first key
        }
        return window(text(" question "),
                      ModalBody(theme, vbox({
                          vbox(std::move(rows)) | yframe | vscroll_indicator |
                              size(HEIGHT, LESS_THAN, ModalBodyHeightBudget()),
                          Rule(theme),
                          paragraph("enter: review  ·  esc: dismiss  ·  tab/arrow: move  ·  "
                                    "space: pick/toggle") |
                              Muted(theme),
                      }))) |
               size(WIDTH, LESS_THAN, 82) | color(theme.ColorFor("attention"));
    });
    return CatchEvent(body, [on_answer, request_state, state, interacted](const Event& event) {
        *interacted = true;
        if (event == Event::Return) {
            if (state->reviewing) {
                on_answer(CollectAnswer(*request_state, *state));
            } else {
                state->reviewing = true;   // first Enter: freeze into the review list
            }
            return true;
        }
        if (event == Event::Escape) {
            // Escape peels ONE level: review back to editing, editing to
            // dismissal -- never review straight to dismissal, and never a
            // level the footer does not name. While editing this is exactly
            // the pre-review modal's contract (dismiss = answered=false,
            // "user dismissed", never a selection).
            if (state->reviewing) {
                state->reviewing = false;
                return true;
            }
            on_answer(DismissedAnswer(*request_state));
            return true;
        }
        if (state->reviewing) {
            // Review mode freezes the picks: swallow everything the editing
            // widgets could mutate with (arrows/tab/space/characters reach
            // the live container even while nothing of it renders) so the
            // list cannot change invisibly behind the review -- but return
            // false for the rest, so Ctrl+C still falls through to the root
            // dispatch's interrupt branch (turn.cancel's hardcoded second
            // key in keybinding_dispatch). A turn.cancel REMAPPED onto a
            // character key is eaten here -- known gap, same class the other
            // modal factories accept by hardcoding Escape.
            if (event == Event::CtrlC) return false;
            return true;
        }
        return false;
    });
}

}  // namespace minimal::client
