#include "ui_kit.hpp"

#include <utility>

#include "filterable_menu.hpp"
#include "modal_chrome.hpp"
#include "model_selector.hpp"
#include "schema_form.hpp"

namespace minimal::client::ui_kit {

ftxui::Component UiKit::FilterableMenuModal(
    std::function<std::vector<MenuRow>(const std::string& filter)> build_rows,
    const FilterableMenuSpec& spec, FilterableMenuCallbacks callbacks, const Theme& theme) const {
    return MakeFilterableMenuModal(std::move(build_rows), spec, std::move(callbacks), theme);
}

ftxui::Component UiKit::SchemaForm(std::function<SchemaFormState()> source,
                                   SchemaFormCallbacks callbacks, const Theme& theme) const {
    return MakeSchemaForm(std::move(source), std::move(callbacks), theme);
}

ftxui::Element UiKit::ModalFrame(const Theme& theme, const std::string& title,
                                 ftxui::Elements rows, ftxui::Elements pinned,
                                 const std::string& footer, ModalAccent accent) const {
    return ui_kit::ModalFrame(theme, title, std::move(rows), std::move(pinned), footer, accent);
}

ftxui::Element UiKit::ModalRule(const Theme& theme) const { return ui_kit::ModalRule(theme); }

ftxui::Component UiKit::ModelSelector(ModelSelectorSpec spec, ModelSelectorCallbacks callbacks,
                                      const Theme& theme) const {
    return MakeModelSelector(std::move(spec), std::move(callbacks), theme);
}

}  // namespace minimal::client::ui_kit
