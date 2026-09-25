#pragma once

// The "ask" prompt renderer: the modal for the server tool-ask plugin's
// `ask_user` questions. The wire shapes are tool-ask's (see
// plugins/server/tool-ask/ask_types.hpp); core relays them as opaque JSON.

#include <functional>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <nlohmann/json.hpp>

#include "minimal/client/plugins/registries.hpp"
#include "minimal/client/render/theme.hpp"

namespace minimal::ask_view {

struct AskOption {
    std::string label;
    std::string description;
};

struct AskQuestion {
    std::string question;
    std::string header;
    std::vector<AskOption> options;
    bool multiple = false;
    bool allow_other = true;
};

// Throws std::invalid_argument on a payload that is not an ask request -- the
// host then answers the prompt UNAVAILABLE rather than drawing half a modal.
std::vector<AskQuestion> ParsePayload(const nlohmann::json& payload);

// The modal. Keys, while editing:
//   Enter      on a single-pick list: pick the highlighted row, then move to
//              the next question (or into the free-text line for "Type your
//              own answer"); on a checkbox list or the text line: next
//              question. Past the last question: the review list -- except a
//              lone single-pick question, which submits at once.
//   1-9        pick (or toggle) that row of the focused list.
//   Tab        next question, leaving this one as it is (skip).
//   space      pick/toggle the highlighted row.
//   Esc        in the text line: back to its list. Elsewhere: dismiss -- a
//              second Esc is required once anything is answered.
// In review: Enter submits, Esc goes back to editing. Ctrl+C always passes
// through to the host's turn-cancel. `asker` names who is asking ("" = the
// main agent).
ftxui::Component MakeAskModal(std::vector<AskQuestion> questions, std::string asker,
                              const minimal::client::Theme& theme,
                              std::function<void(minimal::client::plugins::PromptReply)> on_reply);

// Whether ONE row of a single-pick question wears the "current choice"
// styling. An untouched question wears it NOWHERE: FTXUI's Radiobox clamps
// its selected index from -1 to 0 at first render, so without this gate row 0
// would render as a pre-picked answer the submission would not send.
bool RadioRowSelected(bool touched, bool active, bool focused);

// One option row: marker, the label wrapped, its description wrapped and
// dimmed beneath it.
ftxui::Element WrappedOptionEntry(const ftxui::EntryState& s, const char* marker_on,
                                  const char* marker_off, const std::string& description,
                                  const ftxui::Decorator& selection,
                                  const ftxui::Decorator& muted, bool touched);

class AskPromptRenderer final : public minimal::client::plugins::PromptRenderer {
public:
    std::string_view kind() const override { return "ask"; }
    ftxui::Component Make(
        const minimal::client::plugins::PromptView& prompt, const minimal::client::Theme& theme,
        std::function<void(minimal::client::plugins::PromptReply)> on_reply) const override;
};

}  // namespace minimal::ask_view
