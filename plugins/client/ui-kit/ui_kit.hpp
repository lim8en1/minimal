#pragma once

// ui-kit -- the bundled widget kit (registries.hpp WidgetKit): the
// filterable menu, the schema form, the model selector and the modal chrome
// every view builds on. Replace it by disabling this plugin and enabling
// another that registers a kit; the widget-kit slot is required, so with
// none the TUI refuses to start.

#include "minimal/client/sdk/registries.hpp"

namespace minimal::client::ui_kit {

class UiKit final : public plugins::WidgetKit {
public:
    ftxui::Component FilterableMenuModal(
        std::function<std::vector<MenuRow>(const std::string& filter)> build_rows,
        const FilterableMenuSpec& spec, FilterableMenuCallbacks callbacks,
        const Theme& theme) const override;
    ftxui::Component SchemaForm(std::function<SchemaFormState()> source,
                                SchemaFormCallbacks callbacks, const Theme& theme) const override;
    ftxui::Element ModalFrame(const Theme& theme, const std::string& title, ftxui::Elements rows,
                              ftxui::Elements pinned, const std::string& footer,
                              ModalAccent accent) const override;
    ftxui::Element ModalRule(const Theme& theme) const override;
    ftxui::Component ModelSelector(ModelSelectorSpec spec, ModelSelectorCallbacks callbacks,
                                   const Theme& theme) const override;
};

}  // namespace minimal::client::ui_kit
