#include "minimal/client/view/commands_help.hpp"

#include "minimal/client/utf8.hpp"
#include "minimal/client/view/filterable_menu.hpp"

namespace minimal::client {

ftxui::Component MakeCommandsHelpView(const std::vector<CommandCatalogEntry>& catalog,
                                      const Theme& theme, std::function<void()> on_close,
                                      std::function<bool()> session_available,
                                      std::function<void()>* reset_state) {
    // BY VALUE, not by reference: `catalog` is this factory's OWN parameter,
    // and the returned Component (and its closures) outlive this call -- the
    // exact "closures outlive the factory" trap keymap_help.cpp documents for
    // `bindings`.
    auto build_rows = [catalog, session_available](const std::string& filter) {
        std::vector<MenuRow> rows;
        // Same exclusion the /-autocomplete applies: requires_session
        // entries are hidden while the focused tab has no session, so the
        // overlay never advertises a command the sessionless tab would
        // only refuse.
        const bool session_ok = !session_available || session_available();
        for (const auto& entry : catalog) {
            if (entry.requires_session && !session_ok) continue;
            std::string label = "/" + entry.name + "  -  " + entry.summary;
            if (entry.submits_prompt) label += "  (submits a prompt as your turn)";
            if (entry.read_only) label += "  (read-only)";
            // Case-insensitive, like every other picker (picker_model,
            // agent/model pickers, keymap help): lowered needle, lowered
            // haystack.
            if (filter.empty() ||
                ToLower(label).find(ToLower(filter)) != std::string::npos) {
                rows.push_back({std::move(label), entry.name});
            }
        }
        return rows;
    };

    const FilterableMenuSpec spec{
        .window_title = "commands",
        .hint_line = " /help <command>: detail   esc: close",
        .status_suffix = nullptr,
        .empty_message = " (no commands match)",
        .empty_threshold = 0,
        .reset_state = reset_state,
    };

    // Nothing to select: Enter closes the overlay exactly like Escape --
    // this is a reference, not a picker.
    const FilterableMenuCallbacks callbacks{
        .on_select = [on_close](const MenuRow&) { if (on_close) on_close(); },
        .on_close = on_close,
        .on_extra_key = nullptr,
    };

    return MakeFilterableMenuModal(build_rows, spec, callbacks, theme);
}

}  // namespace minimal::client
