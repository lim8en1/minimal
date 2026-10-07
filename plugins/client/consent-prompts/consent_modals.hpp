#pragma once

// The consent prompts: the permission and confirmation modals core asks
// through the "permission" and "confirmation" prompt kinds. The wording
// obligations are this renderer's -- part of the kinds' contract
// (docs/EXTENSIBILITY.md §4.2), load-bearing, not decoration:
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

#include "minimal/client/sdk/widgets.hpp"
#include "minimal/client/policy.hpp"
#include "minimal/client/sdk/theme.hpp"

namespace minimal::client::consent_prompts {

// A permission request, as the "permission" prompt kind's payload carries it.
struct PendingPermission {
    std::string request_id;
    std::string tool_name;
    v1::ToolCategory category = v1::TOOL_CATEGORY_UNSPECIFIED;  // UI tint
    std::string summary;   // one-line description shown in the prompt
    bool complete = false; // the server offered full disclosure (false = lower bound)
    std::string reason;    // the rule that made this call ask (display only)
    // Session-grant offers ("/usr/bin/make *"): blanket, for the whole binary,
    // any arguments -- worded exactly that way.
    std::vector<v1::GrantOffer> grants;
};

// A confirmation, as the "confirmation" prompt kind's payload carries it.
struct PendingConfirmation {
    std::string request_id;
    std::string kind;    // "tool_permission" | "command_confirmation" | a host's own
    std::string title;
    std::string detail;
    std::vector<std::string> options;
};


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
                                     const plugins::WidgetKit& widgets, const Theme& theme,
                                     std::function<void(PermissionVerdict verdict)> on_answer);

ftxui::Component MakeConfirmationModal(const PendingConfirmation& request,
                                       const plugins::WidgetKit& widgets, const Theme& theme,
                                       std::function<void(std::string chosen)> on_answer);

}  // namespace minimal::client::consent_prompts
