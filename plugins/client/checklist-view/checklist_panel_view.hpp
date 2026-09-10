#pragma once

// The checklist panel view: one toggleable Panel view with TWO surfaces over
// the shared poller's caches -- FOCUSED (the current tab's tasklist, from
// the get cache) and SUMMARY (one line per open tab, from the list cache +
// the focus seam's display names). Tab flips surfaces; the view is strictly
// read-only (the model owns checklist mutations via the tool).

#include <memory>
#include <string>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include "minimal/client/plugins/registries.hpp"
#include "checklist_poller.hpp"

namespace minimal::checklist_view {

using minimal::client::Theme;

class ChecklistPanelView final : public plugins::View {
public:
    ChecklistPanelView(std::shared_ptr<ChecklistPoller> poller, plugins::FocusProvider* focus);

    const plugins::ViewSpec& spec() const override;
    ftxui::Component component() override;
    void OnTheme(const Theme& theme) override;

private:
    enum class Mode { Focused, Summary };

    ftxui::Color ColorFor(const char* role) const;
    ftxui::Element StatusGlyph(const Item& item) const;
    ftxui::Element FocusedSurface() const;
    ftxui::Element SummarySurface() const;
    ftxui::Element RenderBody() const;

    std::shared_ptr<ChecklistPoller> poller_;
    plugins::FocusProvider* focus_;
    plugins::ViewSpec spec_;
    ftxui::Component component_;
    Mode mode_ = Mode::Focused;
    Theme theme_;  // pushed once via OnTheme; an unpushed theme colors Default
};

}  // namespace minimal::checklist_view
