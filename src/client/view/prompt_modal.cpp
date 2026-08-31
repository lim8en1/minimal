#include "minimal/client/view/prompt_modal.hpp"

#include <ftxui/component/event.hpp>

namespace minimal::client {

std::vector<std::string> GrantToggles::GrantedKeys(const PendingPermission& request) const {
    std::vector<std::string> keys;
    const std::size_t count = std::min(selected.size(), request.grants.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (selected[i]) keys.push_back(request.grants[i].key());
    }
    return keys;
}

ftxui::Component MakePermissionModal(const PendingPermission& request, GrantToggles& toggles,
                                     const Theme& theme,
                                     std::function<void(bool allow)> on_answer) {
    using namespace ftxui;

    toggles.selected.assign(request.grants.size(), false);

    // One checkbox per offered grant. Labels carry THE WORDING OBLIGATION:
    // blanket per-binary, any arguments, this session -- a human judging
    // "allow git push --force" would be judging the wrong thing.
    Components grant_boxes;
    for (std::size_t i = 0; i < request.grants.size(); ++i) {
        CheckboxOption option;
        option.label = ftxui::ConstStringRef(std::string(""));
        option.checked = ftxui::Ref<bool>(&toggles.selected[i]);
        grant_boxes.push_back(Checkbox(std::move(option)));
    }
    auto grant_container = Container::Vertical(std::move(grant_boxes));

    // By value for everything the closure outlives the factory: only `request`
    // and `theme` are caller-owned references. (The dangling-reference bug this
    // rule prevents is the one the first smoke run hit.)
    const auto grant_container_val = grant_container;   // outlives the factory: by value
    const auto grant_boxes_val = grant_boxes;
    auto body = Renderer(grant_container, [grant_boxes_val, grant_container_val, &request, &theme] {
        Elements rows;
        rows.push_back(text("permission required") | bold | color(theme.ColorFor("attention")));
        rows.push_back(text("tool: " + request.tool_name) | dim);
        rows.push_back(paragraph(request.summary));
        if (!request.complete) {
            // THE LOWER-BOUND OBLIGATION: say what the disclosure does not
            // cover, so "allow" is never read as "exactly this command".
            rows.push_back(
                paragraph("This is what the command NAMED -- it may reach more. "
                          "Allowing grants this tool, not this text.") |
                color(theme.ColorFor("error")));
        }
        if (!grant_boxes_val.empty()) {
            rows.push_back(text("session grants:") | dim);
            for (std::size_t i = 0; i < request.grants.size(); ++i) {
                rows.push_back(hbox({grant_container_val->ChildAt(static_cast<int>(i))->Render(),
                                     text(" allow " + request.grants[i].label() +
                                          " -- any arguments, this session") |
                                         color(theme.ColorFor("attention"))}));
            }
        }
        rows.push_back(text("enter: allow   ·   esc: deny   ·   tab/arrow: toggle grants") | dim);
        return window(text(" permission "), vbox(std::move(rows)) | size(WIDTH, LESS_THAN, 80)) |
               color(theme.ColorFor("attention"));
    });

    // RunTui's on_answer reads toggles.GrantedKeys(request) -- the filtered
    // keys ride along from state it owns, so the modal never mints a key.
    return CatchEvent(body, [&](const Event& event) {
        if (event == Event::Return) {
            on_answer(true);
            return true;
        }
        if (event == Event::Escape) {
            on_answer(false);
            return true;
        }
        return false;
    });
}

ftxui::Component MakeConfirmationModal(const PendingConfirmation& request, const Theme& theme,
                                       std::function<void(std::string chosen)> on_answer) {
    using namespace ftxui;

    Components option_components;
    for (const auto& option : request.options) {
        option_components.push_back(Button(option, [option, on_answer] { on_answer(option); }));
    }
    auto buttons = ftxui::Container::Horizontal(std::move(option_components));
    const auto buttons_val = buttons;   // outlives the factory: by value
    auto body = Renderer(buttons, [buttons_val, &request, &theme] {
        Elements rows;
        rows.push_back(text(request.title) | bold | color(theme.ColorFor("attention")));
        rows.push_back(paragraph(request.detail));
        rows.push_back(buttons_val->Render() | hcenter);
        return window(text(" confirm "), vbox(std::move(rows)) | size(WIDTH, LESS_THAN, 70)) |
               color(theme.ColorFor("attention"));
    });
    return body;
}

}  // namespace minimal::client
