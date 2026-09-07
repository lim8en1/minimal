#pragma once

// The /-commands help overlay: a modal listing every registered command --
// name, summary, and its traits (read-only / submits a prompt) -- with a
// type-to-filter box, opened by typing `/help` in the prompt. The listing is
// built from the client's ListCommands catalog (the same data the
// /-autocomplete offers), so it is always the full, live command set --
// previously /help's answer reached the client as a transient notice line
// capped at a handful of rows, and a catalog larger than that viewport was
// simply unreachable.
//
// Mirrors keymap_help.hpp minus selection -- there is nothing to switch to,
// Enter and Escape both just close it. A `/help <command>` invocation is NOT
// intercepted: it still reaches the server for the per-command detail notice.

#include <functional>
#include <vector>

#include <ftxui/component/component.hpp>

#include "minimal/client/render/theme.hpp"
#include "minimal/client/view/prompt_editor.hpp"

namespace minimal::client {

ftxui::Component MakeCommandsHelpView(const std::vector<CommandCatalogEntry>& catalog,
                                      const Theme& theme, std::function<void()> on_close,
                                      std::function<bool()> session_available = nullptr,
                                      std::function<void()>* reset_state = nullptr);

}  // namespace minimal::client
