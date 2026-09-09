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
#include <memory>

#include <ftxui/component/component.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/client/state.hpp"

namespace minimal::client {

// Ephemeral checkbox state, one per offered grant. Lives in RunTui.
struct GrantToggles {
    std::deque<bool> selected;   // deque, not vector<bool>: the checkboxes need stable bool*

    // The keys the operator chose, FILTERED to what the request offered -- an
    // answer can never mint a grant for a binary no prompt displayed.
    std::vector<std::string> GrantedKeys(const PendingPermission& request) const;
};

ftxui::Component MakePermissionModal(const PendingPermission& request, GrantToggles& toggles,
                                     const Theme& theme,
                                     std::function<void(bool allow)> on_answer);

ftxui::Component MakeConfirmationModal(const PendingConfirmation& request, const Theme& theme,
                                       std::function<void(std::string chosen)> on_answer);

// The model's structured questions (tool-ask plugin): one modal, up to four
// question blocks. `multiple` questions render checkboxes, single-pick
// questions a radio list; `allow_other` questions get a "Type your own
// answer" toggle with an input line. Enter submits whatever was answered (a
// question left blank is simply not answered); Escape dismisses -- the
// on_answer callback receives an `answered == false` response, which the
// server turns into "user dismissed", never into a selection.
ftxui::Component MakeAskModal(const PendingAsk& request, const Theme& theme,
                              std::function<void(v1::AskUserResponse response)> on_answer);

}  // namespace minimal::client
