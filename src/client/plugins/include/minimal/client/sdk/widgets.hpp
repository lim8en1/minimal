#pragma once

// The widget kit slot: the standard widgets views are built from. The
// contracts (rows, specs, callbacks, state) are widget_types.hpp.

#include <functional>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include "minimal/client/sdk/theme.hpp"
#include "minimal/client/sdk/widget_types.hpp"

namespace minimal::client::plugins {

// --- Widget kit (EXCLUSIVE) ------------------------------------------------

// The standard widgets views are built from: the filterable menu every
// picker is, the schema form, the modal chrome. A view asks the kit
// (ClientPluginRegistrationContext::widgets) rather than building its own,
// so replacing the kit -- disable the default, register another -- restyles
// every view built on it at once. The contracts (rows, specs, callbacks,
// state) are widget_types.hpp; a kit owns only how they look and behave.
// The slot is required: the bundled ui-kit fills it.
//
// The theme comes with every call: plugins load before the theme does.
class WidgetKit {
public:
    virtual ~WidgetKit() = default;

    // A type-to-filter list in a modal window. `build_rows` runs on every
    // keystroke and every rebuild: cheap and side-effect free.
    virtual ftxui::Component FilterableMenuModal(
        std::function<std::vector<MenuRow>(const std::string& filter)> build_rows,
        const FilterableMenuSpec& spec, FilterableMenuCallbacks callbacks,
        const Theme& theme) const = 0;

    // A form over a config schema (SchemaFormState::describe). `source` is
    // read on every render and event: cheap. The component implements
    // EscapeAware -- its inline editor wants Escape while it is open.
    virtual ftxui::Component SchemaForm(std::function<SchemaFormState()> source,
                                        SchemaFormCallbacks callbacks,
                                        const Theme& theme) const = 0;

    // A titled modal window: `rows` scroll within the screen's height,
    // `pinned` stays visible below them (details, the last outcome), and
    // `footer` -- the key hints -- is pinned last. Empty parts are omitted.
    // `accent`: whether the user is being asked something (ModalAccent).
    virtual ftxui::Element ModalFrame(const Theme& theme, const std::string& title,
                                      ftxui::Elements rows, ftxui::Elements pinned,
                                      const std::string& footer, ModalAccent accent) const = 0;

    // A divider between sections of a modal's body.
    virtual ftxui::Element ModalRule(const Theme& theme) const = 0;

    // A model chooser that fetches the list itself through `spec.fetch`,
    // on the caller's worker (`spec.post`); ctrl+r refreshes discovery.
    virtual ftxui::Component ModelSelector(ModelSelectorSpec spec,
                                           ModelSelectorCallbacks callbacks,
                                           const Theme& theme) const = 0;
};

}  // namespace minimal::client::plugins
