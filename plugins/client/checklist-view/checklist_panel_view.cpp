#include "checklist_panel_view.hpp"

#include <utility>

#include <ftxui/component/event.hpp>

namespace minimal::checklist_view {

using minimal::client::Theme;

ChecklistPanelView::ChecklistPanelView(std::shared_ptr<ChecklistPoller> poller,
                                       plugins::FocusProvider* focus)
    : poller_(std::move(poller)), focus_(focus),
      spec_{"checklist.panel", "checklist", plugins::ViewPlacement::Panel, "ctrl+k"} {
    auto body = ftxui::Renderer([this] { return RenderBody(); });
    // Surface toggle: Tab flips FOCUSED <-> SUMMARY. The core's panel focus
    // routing means this handler runs only while the panel holds focus, so
    // the editor's Tab is never touched. Consumed here so it also cannot
    // cycle anything else; every other event falls through (this view is
    // read-only -- the model owns checklist mutations).
    component_ = ftxui::CatchEvent(std::move(body), [this](const ftxui::Event& event) {
        if (event == ftxui::Event::Tab) {
            mode_ = mode_ == Mode::Focused ? Mode::Summary : Mode::Focused;
            return true;
        }
        return false;
    });
}

const plugins::ViewSpec& ChecklistPanelView::spec() const { return spec_; }

ftxui::Component ChecklistPanelView::component() { return component_; }

void ChecklistPanelView::OnTheme(const Theme& theme) { theme_ = theme; }

ftxui::Color ChecklistPanelView::ColorFor(const char* role) const {
    return theme_.ColorFor(role);
}

ftxui::Element ChecklistPanelView::StatusGlyph(const Item& item) const {
    using namespace ftxui;
    switch (item.status) {
        case Status::Completed:
            return hbox({text("[x]") | dim, text(" ")});
        case Status::InProgress:
            // The active marker gets the accent: it is what "what's running
            // right now" wants the eye to find.
            return text("→  ") | color(ColorFor("assistant_message")) | bold;
        case Status::Pending:
            return hbox({text("[ ]"), text(" ")});
    }
    return text(" ");
}

ftxui::Element ChecklistPanelView::FocusedSurface() const {
    using namespace ftxui;
    const ChecklistPoller::FocusedSnapshot snapshot = poller_->Focused();
    Elements rows;

    if (!snapshot.ever_fetched) {
        rows.push_back(text(" fetching…") | dim | color(ColorFor("system_message")));
        return vbox(std::move(rows));
    }
    if (!snapshot.valid) {
        rows.push_back(text(" last update failed; retrying") | dim | color(ColorFor("error")));
    }
    if (snapshot.items.empty()) {
        if (snapshot.valid) {
            rows.push_back(text(" (no tasks)") | dim | color(ColorFor("system_message")));
        }
        return vbox(std::move(rows));
    }

    for (const auto& item : snapshot.items) {
        rows.push_back(hbox({StatusGlyph(item), paragraph(item.text)}));
    }
    const Counts counts = CountOf(snapshot.items);
    rows.push_back(text(" ") | dim);  // spacer
    rows.push_back(text(std::to_string(counts.done) + "/" + std::to_string(counts.total) +
                        " done") |
                    dim | color(ColorFor("system_message")));
    return vbox(std::move(rows));
}

ftxui::Element ChecklistPanelView::SummarySurface() const {
    using namespace ftxui;
    Elements rows;

    if (!poller_->AllEverFetched()) {
        rows.push_back(text(" fetching…") | dim | color(ColorFor("system_message")));
        return vbox(std::move(rows));
    }
    const auto all_items = poller_->AllItems();
    if (!poller_->AllValid()) {
        rows.push_back(text(" last update failed; retrying") | dim | color(ColorFor("error")));
    }

    // Open tabs only, in the host's own order (focused first): the summary
    // is per-TAB, the thing the user can act on -- a session whose tab is
    // closed has no surface to jump to. This view registers ONLY when the
    // context carried a FocusProvider, so focus_ is live here.
    const std::string focused_session = focus_->FocusedSessionId();
    std::size_t lines = 0;
    for (const auto& tab : focus_->Tabs()) {
        if (tab.session_id.empty()) continue;  // sessionless: no tasks possible
        std::vector<Item> items;
        if (const auto it = all_items.find(tab.session_id); it != all_items.end()) {
            items = it->second;
        }
        const Counts counts = CountOf(items);
        auto line = hbox({
            text(tab.session_id == focused_session ? "▸ " : "  ") |
                color(ColorFor("border_focused")),
            text(tab.display_name.empty() ? tab.session_id : tab.display_name) |
                color(ColorFor("assistant_message")),
            text("  " + std::to_string(counts.done) + "/" + std::to_string(counts.total)) |
                dim | color(ColorFor("system_message")),
        });
        if (counts.active > 0) {
            line = hbox({std::move(line),
                         text("  " + std::to_string(counts.active) + " active") |
                             color(ColorFor("assistant_message"))});
        }
        rows.push_back(std::move(line));
        ++lines;
    }
    if (lines == 0) {
        rows.push_back(text(" (no open tabs with tasks)") | dim |
                       color(ColorFor("system_message")));
    }
    return vbox(std::move(rows));
}

ftxui::Element ChecklistPanelView::RenderBody() const {
    using namespace ftxui;
    Elements rows;
    // The mode hint rides the top: which surface is showing, and the key
    // that flips it (the panel receives events only while focused).
    rows.push_back(hbox({
        text(mode_ == Mode::Focused ? " tasks · this tab" : " tasks · all tabs") |
            color(ColorFor("syntax_function")),
        text(" (tab to switch)") | dim | color(ColorFor("system_message")),
    }));
    rows.push_back(mode_ == Mode::Focused ? FocusedSurface() : SummarySurface());
    return vbox(std::move(rows));
}

}  // namespace minimal::checklist_view
