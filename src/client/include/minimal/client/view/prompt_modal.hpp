#pragma once

// THE SECURITY UI. Never pluggable, under any circumstance
// (docs/EXTENSIBILITY.md §4.1) -- the consent-scoping wording obligations are
// core-owned, and they are load-bearing, not decoration:
//
//   * A grant is BLANKET: for a binary, any arguments ("/usr/bin/git *
//     (any arguments)"); for file edits, every file in the project. The
//     label the server sends carries that wording and the modal prints it
//     verbatim with "this session" -- a human judging "allow git push
//     --force" is judging the wrong thing; the offer is "allow git *".
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
#include "minimal/client/policy.hpp"

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

// Modal chrome shared by every blocking prompt -- core's permission and
// confirmation modals and the prompt renderers client plugins register -- so a
// plugin's modal looks like the core ones.

// A modal's internal divider (hairline role, not the window's attention tint).
ftxui::Element ModalRule(const Theme& theme);
// How tall the scrollable body may be before the pinned hint line and the
// window borders stop fitting on screen.
int ModalBodyHeightBudget();
// The body: padded one column each side, in the body color.
ftxui::Element ModalBody(const Theme& theme, ftxui::Element content);

// The whole chrome of a prompt-style modal: a titled window framed in the
// attention color; `rows` in the body color, scrolling under the height
// budget; the key hints (`footer`, omitted when empty) pinned below a rule --
// visible however much content there is -- and wrapped rather than clipped.
// `max_width` caps the frame (kModalMaxWidth by default).
ftxui::Element ModalFrame(const Theme& theme, const std::string& title, ftxui::Elements rows,
                          const std::string& footer, int max_width = kModalMaxWidth);

}  // namespace minimal::client
