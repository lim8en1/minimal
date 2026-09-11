#include "minimal/client/view/prompt_modal.hpp"

#include <algorithm>
#include <deque>

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
struct PermissionModalState {
    int selected = 0;             // index into kPermissionChoices; allow by default
    bool composing = false;       // the deny-with-message input is open
    bool interacted = false;      // the arming rule (see the CatchEvent)
    bool grants_focused = false;  // last key moved the grants, not the list
    std::string message;
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
    auto body = Renderer(grant_container,
                         [grant_boxes_val, grant_container_val, request_state, state, input,
                          &theme] {
        const PendingPermission& request = *request_state;
        Elements rows;
        rows.push_back(text("permission required") | bold | color(theme.ColorFor("attention")));
        rows.push_back(text("tool: " + request.tool_name) | Muted(theme));
        rows.push_back(paragraph(request.summary));
        if (!request.complete) {
            // THE LOWER-BOUND OBLIGATION: say what the disclosure does not
            // cover, so "allow" is never read as "exactly this command".
            rows.push_back(
                paragraph("This is what the command NAMED — it may reach more. "
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
        int active_grant_row = -1;
        if (!request.grants.empty()) {
            rows.push_back(text("session grants (sent only with allow):") | Muted(theme));
            for (std::size_t i = 0; i < request.grants.size(); ++i) {
                auto row = hbox({grant_container_val->ChildAt(static_cast<int>(i))->Render(),
                                 text(" allow " + request.grants[i].label() +
                                      " · any arguments, this session") |
                                     color(theme.ColorFor("attention"))});
                if (static_cast<int>(i) == active_grant) {
                    row |= bgcolor(theme.ColorFor("status_bar_bg"));
                    active_grant_row = static_cast<int>(rows.size());
                }
                rows.push_back(std::move(row));
            }
        }

        // The answer list. The selection marker is the list's cursor; the
        // grants' background highlight is theirs -- two different shapes, so
        // the two cursors never read as one.
        rows.push_back(text(""));
        int selected_row = -1;
        for (int i = 0; i < kPermissionChoiceCount; ++i) {
            const auto& choice = kPermissionChoices[i];
            const bool is_selected = i == state->selected;
            std::string hint = choice.hint;
            if (choice.choice == PermissionChoice::Allow && !request.grants.empty()) {
                hint = "this once, plus any grants ticked above";
            }
            // Marker and label bold TOGETHER: one styled span, so the marker
            // reads as part of the selected answer.
            auto label = hbox({text(is_selected ? "❯ " : "  "),
                               text(std::to_string(i + 1) + ". " + choice.label)});
            if (is_selected) label |= bold;
            if (is_selected) selected_row = static_cast<int>(rows.size());
            rows.push_back(hbox({std::move(label), text("  " + hint) | Muted(theme)}));
        }
        int message_row = -1;
        if (state->composing) {
            message_row = static_cast<int>(rows.size());
            rows.push_back(hbox({text("   message: "), input->Render() | flex}));
        }

        // The scroll anchor: the summary until the user touches a key, then
        // whatever the last key moved -- the message line, the active grant,
        // or the selected answer. One anchor at a time: two `focus` markers
        // would fight over the frame's viewport.
        if (!state->interacted) {
            rows[2] |= focus;   // the summary
        } else if (message_row >= 0) {
            rows[message_row] |= focus;
        } else if (state->grants_focused && active_grant_row >= 0) {
            rows[active_grant_row] |= focus;
        } else if (selected_row >= 0) {
            rows[selected_row] |= focus;
        }

        // The footer only advertises affordances that actually do something:
        // tab and space are dead keys on a request with no grants to move
        // between or toggle, and advertising them anyway just invites a press
        // that does nothing. Two-space separators, matching the pickers' own
        // hint lines (session_picker.cpp etc.).
        std::string footer_text;
        if (state->composing) {
            footer_text = "enter: deny with this message  ·  esc: back to the list";
        } else {
            // Kept under the modal's 78 usable columns: a wrap could split an
            // affordance ("tab/space" from "grants") across two lines.
            footer_text = "↑/↓ 1-4: choose  ·  enter: confirm  ·  esc: deny";
            if (!request.grants.empty()) footer_text += "  ·  tab/space: grants";
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
    return CatchEvent(body, [on_answer, state, input](const Event& event) {
        // The FIRST event this modal ever sees only arms it -- it never
        // answers (Escape aside: denying is the safe default). A Return or a
        // digit that lands in the same input batch as the request's own
        // arrival (a fast typist's unrelated keystroke, a paste's trailing
        // newline) must not read as the user's answer to a prompt they have
        // not seen yet -- and with "deny and stop" on the list, that goes for
        // the deny answers too, not only for allow.
        const bool armed = state->interacted;
        state->interacted = true;
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
            // Everything else is typing. Swallowed even when the input
            // ignores it, so no key can reach the grant checkboxes behind it.
            input->OnEvent(event);
            return true;
        }

        if (event == Event::Escape) {
            on_answer({PermissionChoice::Deny, {}});
            return true;
        }
        if (event == Event::ArrowUp || event == Event::ArrowDown) {
            const int step = event == Event::ArrowUp ? -1 : 1;
            state->selected = std::clamp(state->selected + step, 0, kPermissionChoiceCount - 1);
            state->grants_focused = false;
            return true;
        }
        if (event == Event::Tab || event == Event::TabReverse || event == Event::Character(" ")) {
            state->grants_focused = true;
            return false;   // the grant container moves/toggles
        }

        int pick = -1;
        if (event == Event::Return) {
            pick = state->selected;
        } else if (event.is_character() && event.character().size() == 1) {
            const char c = event.character()[0];
            if (c >= '1' && c < '1' + kPermissionChoiceCount) pick = c - '1';
        }
        if (pick < 0) return false;
        state->selected = pick;
        state->grants_focused = false;
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
        bool use_other = false;
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
        if (qs.use_other && !qs.other_text.empty()) {
            slot->set_other(qs.other_text);
            continue;
        }
        if (question.multiple()) {
            for (int j = 0; j < question.options_size(); ++j) {
                if (qs.boxes[j]) slot->add_selected(question.options(j).label());
            }
        } else if (qs.radio_touched && qs.radio >= 0 && qs.radio < question.options_size()) {
            slot->add_selected(question.options(qs.radio).label());
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

// One option row: marker, then the label wrapped, then its description
// wrapped and dimmed beneath it -- both hanging under the label column
// instead of running off the modal's fixed width like a bare text() would.
// Mirrors RadioboxOption::Simple()/CheckboxOption::Simple() styling (bold
// when active, highlighted when focused) so the only visible change is wrapping.
ftxui::Element WrappedOptionEntry(const ftxui::EntryState& s, const char* marker_on,
                                  const char* marker_off, const std::string& description,
                                  const ftxui::Decorator& selection,
                                  const ftxui::Decorator& muted) {
    using namespace ftxui;
    Element marker = text(s.state ? marker_on : marker_off);
    Element label = paragraph(s.label);
    if (s.active) label |= bold;
    if (s.focused) label |= selection;
    Elements column = {label};
    if (!description.empty()) column.push_back(paragraph(description) | muted);
    return hbox({marker, vbox(std::move(column)) | flex});
}

// One review-screen line for ONE question, mirroring CollectAnswer's slot
// logic exactly -- whatever the review shows is what submission will send,
// so a drift between the two would display one thing and answer another.
std::string ReviewLine(const v1::AskQuestion& question,
                       const AskModalState::QuestionState& qs) {
    if (qs.use_other && !qs.other_text.empty()) {
        return "-> your answer: " + qs.other_text;
    }
    if (question.multiple()) {
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
    if (qs.radio_touched && qs.radio >= 0 && qs.radio < question.options_size()) {
        return "-> " + question.options(qs.radio).label();
    }
    return "(not answered)";
}

}  // namespace

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

        if (question.multiple()) {
            for (const auto& option : question.options()) {
                qs.entries.push_back(option.label());
                qs.boxes.push_back(false);
            }
            Components boxes;
            for (std::size_t j = 0; j < qs.entries.size(); ++j) {
                CheckboxOption option_state;
                option_state.label = ConstStringRef(qs.entries[j]);
                option_state.checked = Ref<bool>(&qs.boxes[j]);
                // Checkbox's own EntryState.index is always -1 (it renders
                // itself, not a list), so the description is baked into the
                // closure by j rather than looked up through the entry state.
                const std::string description = question.options(static_cast<int>(j)).description();
                option_state.transform = [description, selection, muted](const EntryState& s) {
                    return WrappedOptionEntry(s, "▣ ", "☐ ", description, selection, muted);
                };
                boxes.push_back(Checkbox(std::move(option_state)));
            }
            block.push_back(Container::Vertical(std::move(boxes)));
        } else {
            // Radiobox is the single-pick control: one stable index, no
            // manual "uncheck the others" bookkeeping.
            std::vector<std::string> entries;
            for (const auto& option : question.options()) {
                entries.push_back(option.label());
            }
            qs.entries = std::move(entries);
            RadioboxOption radio_state;
            radio_state.entries = ConstStringListRef(&qs.entries);
            radio_state.selected = &qs.radio;
            const auto state_ref = state;   // shared_ptr copy: outlives the closure
            const int index = i;
            radio_state.on_change = [state_ref, index] {
                state_ref->questions[index].radio_touched = true;
            };
            // Radiobox's EntryState.index tracks the list position, so the
            // description can be looked up straight from the question.
            radio_state.transform = [request_state, index, selection,
                                     muted](const EntryState& s) {
                const std::string description =
                    request_state->request.questions(index).options(s.index).description();
                return WrappedOptionEntry(s, "◉ ", "○ ", description, selection, muted);
            };
            block.push_back(Radiobox(std::move(radio_state)));
        }

        if (question.allow_other()) {
            // The free-text Input is a focusable child ONLY while "Type your
            // own answer" is checked: an invisible-but-focusable component is
            // the exact "hidden choice" bug class (Tab lands on a row that
            // renders nothing), and the always-on input spent a modal row
            // most asks never use. The checkbox stays attached throughout --
            // DetachAllChildren clears the container, so the sync re-adds
            // box first, then input only when checked. The checkbox is built
            // after the closure that syncs it, so it rides a shared holder
            // read at fire time (ftxui flips *checked, then runs on_change
            // inline or posts it to the app loop -- either way the components
            // are alive).
            auto other = Container::Vertical({});
            auto input = Input(&qs.other_text, "your answer");
            struct OtherParts {
                Component box;
            };
            const auto parts = std::make_shared<OtherParts>();
            const auto state_ref = state;   // shared_ptr copy: outlives the closure
            const int index = i;
            CheckboxOption box_opt;
            box_opt.label = ConstStringRef("Type your own answer");
            box_opt.checked = Ref<bool>(&qs.use_other);
            box_opt.on_change = [other, input, parts, state_ref, index] {
                other->DetachAllChildren();
                other->Add(parts->box);
                if (state_ref->questions[index].use_other) other->Add(input);
            };
            parts->box = Checkbox(std::move(box_opt));
            other->Add(parts->box);
            block.push_back(other);
        }

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
                          paragraph("enter: review  ·  esc: dismiss  ·  tab/arrow: move") | Muted(theme),
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
