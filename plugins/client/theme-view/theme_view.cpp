#include "theme_view.hpp"

#include <utility>

#include "minimal/client/utf8.hpp"
#include "minimal/client/view/rebuilding.hpp"

namespace minimal::theme_view {

std::vector<client::MenuRow> BuildThemeRows(const std::vector<std::string>& names,
                                            const std::string& current,
                                            const std::string& filter) {
    const std::string needle = client::ToLower(filter);
    std::vector<client::MenuRow> rows;
    for (const auto& name : names) {
        if (!needle.empty() && client::ToLower(name).find(needle) == std::string::npos) continue;
        // The id stays the bare name: SetTheme takes it verbatim.
        rows.push_back({(name == current ? "● " : "  ") + name, name, /*header=*/false});
    }
    return rows;
}

const plugins::ViewSpec& ThemeView::spec() const {
    static const plugins::ViewSpec kSpec{kViewId, "switch theme", plugins::ViewPlacement::Modal};
    return kSpec;
}

ftxui::Component ThemeView::component() {
    return ftxui::Make<client::RebuildingComponent>(stale_, [seams = seams_, theme = &theme_] {
        auto names = seams.ui->ThemeNames();
        const auto close = [ui = seams.ui] { ui->CloseView(kViewId); };
        return seams.widgets->FilterableMenuModal(
            [names = std::move(names), current = theme->name()](const std::string& filter) {
                return BuildThemeRows(names, current, filter);
            },
            {.window_title = "switch theme",
             .hint_line = " enter: switch  ·  esc: close",
             .empty_message = " (no themes match)"},
            {.on_select =
                 [actions = seams.actions, close](const client::MenuRow& row) {
                     close();
                     actions->SetTheme(row.id);
                 },
             .on_close = close},
            *theme);
    });
}

bool ThemeCommand::Run(const std::vector<std::string>& positional, const std::string&) {
    if (positional.empty()) {
        seams_.ui->ShowView(kViewId);
    } else {
        seams_.actions->SetTheme(positional.front());
    }
    return true;
}

}  // namespace minimal::theme_view
