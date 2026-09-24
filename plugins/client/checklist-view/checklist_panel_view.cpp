#include "checklist_panel_view.hpp"

#include <utility>

#include <ftxui/component/event.hpp>

namespace minimal::checklist_view {

using minimal::client::Theme;

ChecklistPanelView::ChecklistPanelView(std::shared_ptr<ChecklistFeed> feed,
                                       plugins::FocusProvider* focus)
    : feed_(std::move(feed)), focus_(focus),
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
    // Every glyph is 4 cells wide, so item text lines up whatever the status.
    switch (item.status) {
        case Status::Completed:
            return hbox({text("[x]") | color(ColorFor("muted")), text(" ")});
        case Status::InProgress:
            // The active marker gets the accent: it is what "what's running
            // right now" wants the eye to find.
            return hbox({text(" → ") | color(ColorFor("assistant_message")) | bold, text(" ")});
        case Status::Pending:
            return hbox({text("[ ]"), text(" ")});
    }
    return text("    ");
}

ftxui::Element ChecklistPanelView::StaleMarker() const {
    return ftxui::text(" (stale; reconnecting)") | ftxui::color(ColorFor("error"));
}

ftxui::Element ChecklistPanelView::FocusedSurface() const {
    using namespace ftxui;
    Elements rows;

    const std::string session_id = focus_->FocusedSessionId();
    if (session_id.empty()) {
        rows.push_back(text(" (no session in this tab)") | color(ColorFor("system_message")));
        return vbox(std::move(rows));
    }
    const ChecklistFeed::SessionSnapshot snapshot = feed_->Session(session_id);
    if (!snapshot.known) {
        rows.push_back(text(" fetching…") | color(ColorFor("system_message")));
        return vbox(std::move(rows));
    }
    if (snapshot.stale) rows.push_back(StaleMarker());
    if (snapshot.items.empty()) {
        rows.push_back(text(" (no tasks)") | color(ColorFor("system_message")));
        return vbox(std::move(rows));
    }

    // The first in-progress item takes the frame's focus, so a list taller
    // than the panel scrolls to what is running now.
    bool focused_row = false;
    for (const auto& item : snapshot.items) {
        auto row = hbox({StatusGlyph(item), paragraph(item.text)});
        if (!focused_row && item.status == Status::InProgress) {
            row = row | focus;
            focused_row = true;
        }
        rows.push_back(std::move(row));
    }
    const Counts counts = CountOf(snapshot.items);
    rows.push_back(text(" "));  // spacer
    rows.push_back(text(std::to_string(counts.done) + "/" + std::to_string(counts.total) +
                        " done") | color(ColorFor("system_message")));
    return vbox(std::move(rows));
}

ftxui::Element ChecklistPanelView::SummarySurface() const {
    using namespace ftxui;
    Elements rows;

    // Open tabs only, in the host's own order (focused first): the summary
    // is per-TAB, the thing the user can act on -- a session whose tab is
    // closed has no surface to jump to. Tabs with no tasks (or none known
    // yet) are left out: "no tasks" is not "0/0". This view registers ONLY
    // when the context carried a FocusProvider, so focus_ is live here.
    const std::string focused_session = focus_->FocusedSessionId();
    bool stale = false;
    for (const auto& tab : focus_->Tabs()) {
        if (tab.session_id.empty()) continue;  // sessionless: no tasks possible
        const auto snapshot = feed_->Session(tab.session_id);
        stale = stale || snapshot.stale;
        if (snapshot.items.empty()) continue;
        const Counts counts = CountOf(snapshot.items);
        auto line = hbox({
            text(tab.session_id == focused_session ? "▸ " : "  ") |
                color(ColorFor("border_focused")),
            text(tab.display_name.empty() ? tab.session_id : tab.display_name) |
                color(ColorFor("assistant_message")),
            text("  " + std::to_string(counts.done) + "/" + std::to_string(counts.total)) |
                color(ColorFor("system_message")),
        });
        if (counts.active > 0) {
            line = hbox({std::move(line),
                         text("  " + std::to_string(counts.active) + " active") |
                             color(ColorFor("assistant_message"))});
        }
        rows.push_back(std::move(line));
    }
    if (rows.empty()) {
        rows.push_back(text(" (no open tabs with tasks)") | color(ColorFor("system_message")));
    }
    if (stale) rows.insert(rows.begin(), StaleMarker());
    return vbox(std::move(rows));
}

ftxui::Element ChecklistPanelView::RenderBody() const {
    using namespace ftxui;
    // The mode hint rides the top, outside the scrolled frame: which surface
    // is showing, and the key that flips it (the panel receives events only
    // while focused).
    auto header = hbox({
        text(mode_ == Mode::Focused ? " tasks · this tab" : " tasks · all tabs") |
            color(ColorFor("syntax_function")),
        text(" (tab to switch)") | color(ColorFor("system_message")),
    });
    auto surface = mode_ == Mode::Focused ? FocusedSurface() : SummarySurface();
    return vbox({std::move(header), std::move(surface) | vscroll_indicator | yframe | flex});
}

}  // namespace minimal::checklist_view
