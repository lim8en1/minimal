#pragma once

// THE SECURITY UI. Never pluggable, under any circumstance
// (docs/EXTENSIBILITY.md §4.1) -- the consent-scoping wording obligations are
// core-owned, and they are load-bearing, not decoration:
//
//   * A grant is BLANKET permission for a binary, any arguments, this session.
//     The modal must say so in those words -- a human judging "allow git
//     push --force" is judging the wrong thing; the offer is "allow git *".
//   * Disclosure is a LOWER BOUND. When complete == false the modal must say
//     what the offer does NOT cover (syntactic blindness only), so "allow"
//     is never read as "this exact command and nothing else".
//
// Answers carry granted_keys filtered to what was OFFERED; the server
// enforces the same filter (wire_permission_checker), so the client filter is
// defense in depth, not the boundary.

#include <deque>
#include <functional>
#include <memory>
#include <string>

#include <ftxui/component/component.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/client/state.hpp"

namespace minimal::client {

// Ephemeral checkbox state, one per offered grant. Lives in RunTui.
struct GrantToggles {
    std::deque<bool> selected;   // deque, not vector<bool>: avoids the proxy-reference
                                 // surprises vector<bool> has whenever code takes &selected[i]

    // The keys the operator chose, FILTERED to what the request offered -- an
    // answer can never mint a grant for a binary no prompt displayed.
    std::vector<std::string> GrantedKeys(const PendingPermission& request) const;
};

// The four answers, in the order the modal lists them (and numbers them 1-4).
enum class PermissionChoice { Allow, Deny, DenyWithMessage, DenyAndStop };

struct PermissionVerdict {
    PermissionChoice choice = PermissionChoice::Deny;
    std::string message;   // DenyWithMessage only; never empty there (see below)
};

// ONE unified cursor walks every row: the grant checkboxes (if any), then the
// four answers. ↑/↓ move it; Space/Enter act on whatever it currently points
// at (toggle a grant, or pick an answer); 1-4 pick an answer directly; Tab
// jumps between the grants and the answers. Clicking a row does what
// activating it would -- a grant toggles, an answer is picked -- immediately,
// with no separate confirm step (a click is a deliberate, aimed action).
// Grants sent only ride an Allow. "deny with message" opens an inline input:
// Enter sends it (an empty message degrades to a plain Deny), Escape backs
// out to the list. For every KEYBOARD path (not clicks -- see above), the
// FIRST event only arms the modal -- nothing but Escape answers or toggles a
// grant until the user has touched a key, so a stray Return, space or digit
// landing in the same input batch as the request's own arrival cannot answer
// or toggle anything on an unread prompt. Ctrl+C always falls through to the
// root dispatch's turn-cancel branch.
ftxui::Component MakePermissionModal(const PendingPermission& request, GrantToggles& toggles,
                                     const Theme& theme,
                                     std::function<void(PermissionVerdict verdict)> on_answer);

ftxui::Component MakeConfirmationModal(const PendingConfirmation& request, const Theme& theme,
                                       std::function<void(std::string chosen)> on_answer);

// The model's structured questions (tool-ask plugin): one modal, up to four
// question blocks. `multiple` questions render checkboxes, single-pick
// questions a radio list; `allow_other` questions get a "Type your own
// answer" toggle whose input line exists (and is focusable) only while
// checked. Enter first freezes the answers into a review list (one line per
// question); Enter again submits whatever was answered (a question left
// blank is simply not answered), Escape from review goes back to editing,
// and Escape while editing dismisses -- the on_answer callback receives an
// `answered == false` response, which the server turns into "user
// dismissed", never into a selection. Unhandled keys fall through: Ctrl+C
// still reaches the root dispatch's turn-cancel branch while the modal (or
// its review list) is up.
ftxui::Component MakeAskModal(const PendingAsk& request, const Theme& theme,
                              std::function<void(v1::AskUserResponse response)> on_answer);

}  // namespace minimal::client
