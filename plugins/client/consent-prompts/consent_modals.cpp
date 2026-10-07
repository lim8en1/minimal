#include "consent_modals.hpp"
#include "minimal/client/policy.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>

#include <ftxui/component/event.hpp>
#include <ftxui/component/mouse.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/terminal.hpp>

namespace minimal::client::consent_prompts {

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
                                     const plugins::WidgetKit& widgets, const Theme& theme,
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
    auto body = Renderer([request_state, state, input, &toggles, &widgets, &theme] {
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
        return widgets.ModalFrame(theme, "permission", std::move(rows), {}, footer_text);
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

ftxui::Component MakeConfirmationModal(const PendingConfirmation& request,
                                       const plugins::WidgetKit& widgets, const Theme& theme,
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
    auto body = Renderer(buttons, [buttons_val, request_state, has_cancel, &widgets, &theme, interacted] {
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
        return widgets.ModalFrame(theme, "confirm", std::move(rows), {},
                                  has_cancel ? "esc: cancel" : "") |
               size(WIDTH, LESS_THAN, kConfirmModalMaxWidth);
    });
    // Escape picks the "cancel" option when the modal offers one: both
    // destructive confirms do, and the picker/permission modals already honor
    // Escape -- the inconsistency made the most destructive dialogs the only
    // ones without a keyboard out.
    const std::string cancel_option = "cancel";
    //
    // Without a cancel option (the server's Yes/No), focus starts on the
    // first answer, so a reflexive Enter as the modal opens -- the user was
    // mid-way through submitting a message -- answered "Yes". The permission
    // modal's arming rule applies: until some other key has touched the
    // modal, Enter only arms it (and the focus frame appears).
    return CatchEvent(body, [on_answer, has_cancel, cancel_option, interacted](const Event& event) {
        const bool armed = *interacted;
        *interacted = true;
        if (!has_cancel && !armed && event == Event::Return) return true;
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

}  // namespace minimal::client::consent_prompts
