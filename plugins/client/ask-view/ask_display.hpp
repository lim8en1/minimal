#pragma once

// The "ask" display renderer: the transcript row of an `ask_user` call, drawn
// from the tool's display payload (plugins/server/tool-ask/ask_tool.cpp
// Display()): every question and what the user answered. Keyed by display
// kind, not tool name, so an aliased tool keeps it.

#include "minimal/client/sdk/render.hpp"

namespace minimal::ask_view {

class AskDisplayRenderer final : public minimal::client::plugins::DisplayRenderer {
public:
    std::string_view kind() const override { return "ask"; }
    // Starts expanded: the row is the user's only durable record of what
    // they answered -- the modal closes the moment they submit.
    std::optional<bool> collapsed_by_default() const override { return false; }
    ftxui::Element Render(const minimal::client::plugins::ToolResultView& result,
                          const minimal::client::Theme& theme,
                          const minimal::client::plugins::RowRenderContext& ctx) const override;
};

}  // namespace minimal::ask_view
