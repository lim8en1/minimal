#include "minimal/client/view/commands_help.hpp"

#include "minimal/client/view/filterable_menu.hpp"

namespace minimal::client {

ftxui::Component MakeCommandsHelpView(const std::vector<CommandCatalogEntry>& catalog,
                                      const Theme& theme, std::function<void()> on_close) {
    // BY VALUE, not by reference: `catalog` is this factory's OWN parameter,
    // and the returned Component (and its closures) outlive this call -- the
    // exact "closures outlive the factory" trap keymap_help.cpp documents for
    // `bindings`.
    auto build_rows = [catalog](const std::string& filter) {
        std::vector<MenuRow> rows;
        for (const auto& entry : catalog) {
            std::string label = "/" + entry.name + "  -  " + entry.summary;
            if (entry.submits_prompt) label += "  (submits a prompt as your turn)";
            if (entry.read_only) label += "  (read-only)";
            // Case-insensitive enough: the filter matches against the whole
            // label, so both names and summaries are searchable.
            if (filter.empty() || label.find(filter) != std::string::npos) {
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
