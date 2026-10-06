#include "help_view.hpp"

#include <algorithm>
#include <functional>
#include <tuple>
#include <utility>

#include <ftxui/component/component_base.hpp>

#include "minimal/client/policy.hpp"
#include "minimal/client/utf8.hpp"

namespace minimal::help_view {

namespace {

// A menu built afresh whenever the view is shown again: `stale` is set by
// the view's OnShow (the host's UI thread) and honoured at the start of the
// next render or event -- never mid-frame, where freeing the menu being
// drawn would leave the frame writing into it.
class ReloadingMenu final : public ftxui::ComponentBase {
public:
    ReloadingMenu(std::shared_ptr<std::atomic<bool>> stale, std::function<ftxui::Component()> build)
        : stale_(std::move(stale)), build_(std::move(build)) {}

private:
    void Sync() {
        if (menu_ && !stale_->exchange(false)) return;
        stale_->store(false);
        if (menu_) DetachAllChildren();
        menu_ = build_();
        Add(menu_);
    }
    ftxui::Element OnRender() override {
        Sync();
        return menu_->Render();
    }
    bool OnEvent(ftxui::Event event) override {
        Sync();
        const ftxui::Component target = menu_;  // a callback may mark it stale
        return target->OnEvent(std::move(event));
    }
    bool Focusable() const override { return true; }
    ftxui::Component ActiveChild() override { return menu_; }

    std::shared_ptr<std::atomic<bool>> stale_;
    std::function<ftxui::Component()> build_;
    ftxui::Component menu_;
};

}  // namespace

std::vector<client::MenuRow> BuildKeymapRows(std::vector<plugins::KeyBindingInfo> bindings,
                                             const std::string& filter) {
    const std::string needle = client::ToLower(filter);
    std::erase_if(bindings, [&](const plugins::KeyBindingInfo& binding) {
        return !needle.empty() && client::ToLower(binding.id).find(needle) == std::string::npos &&
               client::ToLower(binding.key).find(needle) == std::string::npos &&
               client::ToLower(binding.description).find(needle) == std::string::npos;
    });
    std::sort(bindings.begin(), bindings.end(), [](const auto& a, const auto& b) {
        return std::tie(a.order, a.id) < std::tie(b.order, b.id);
    });
    std::size_t key_width = 0;
    for (const auto& binding : bindings) {
        key_width = std::max(key_width, client::Utf8ColumnWidth(binding.key));
    }
    key_width = std::min(key_width, client::kKeyColumnCap);

    std::vector<client::MenuRow> rows;
    const std::string* group = nullptr;
    for (const auto& binding : bindings) {
        if (group == nullptr || *group != binding.group) {
            group = &binding.group;
            // One space in from the frame; the menu indents the rows under it.
            rows.push_back({" " + binding.group, "", /*header=*/true});
        }
        std::string key = client::TruncateUtf8Columns(binding.key, key_width);
        key += std::string(key_width - std::min(key_width, client::Utf8ColumnWidth(key)), ' ');
        rows.push_back({key + "  " + binding.description, binding.id, /*header=*/false});
    }
    return rows;
}

std::vector<client::MenuRow> BuildCommandRows(const std::vector<client::CommandCatalogEntry>& entries,
                                              const std::string& filter) {
    const std::string needle = client::ToLower(filter);
    std::vector<client::MenuRow> rows;
    for (const auto& entry : entries) {
        std::string label = "/" + entry.name + "  " + entry.summary;
        if (entry.submits_prompt) label += "  (submits a prompt as your turn)";
        if (entry.read_only) label += "  (read-only)";
        if (!needle.empty() && client::ToLower(label).find(needle) == std::string::npos) continue;
        rows.push_back({std::move(label), entry.name, /*header=*/false});
    }
    return rows;
}

const plugins::ViewSpec& KeymapView::spec() const {
    // Not f1: on Mac keyboards F1 is a hardware brightness key by default
    // (needs Fn held, or "Use F1, F2, etc. keys as standard function keys"
    // enabled in System Settings) -- alt+h reaches every keyboard with no
    // configuration, and "h" for help is mnemonic.
    static const plugins::ViewSpec kSpec{kKeymapViewId, "key bindings",
                                         plugins::ViewPlacement::Modal, "alt+h"};
    return kSpec;
}

ftxui::Component KeymapView::component() {
    return ftxui::Make<ReloadingMenu>(stale_, [seams = seams_, theme = &theme_] {
        // Read once per open (UI thread): the filter runs over this copy.
        auto bindings = seams.ui->KeyBindings();
        const auto close = [ui = seams.ui] { ui->CloseView(kKeymapViewId); };
        return seams.widgets->FilterableMenuModal(
            [bindings = std::move(bindings)](const std::string& filter) {
                return BuildKeymapRows(bindings, filter);
            },
            {.window_title = "key bindings",
             .hint_line = " type a key, an action or what it does to filter  ·  esc: close",
             .empty_message = " (no bindings match)"},
            // Nothing to pick: Enter closes it like Escape -- a reference,
            // not a picker.
            {.on_select = [close](const client::MenuRow&) { close(); }, .on_close = close},
            *theme);
    });
}

const plugins::ViewSpec& CommandsView::spec() const {
    static const plugins::ViewSpec kSpec{kCommandsViewId, "commands",
                                         plugins::ViewPlacement::Modal};
    return kSpec;
}

ftxui::Component CommandsView::component() {
    return ftxui::Make<ReloadingMenu>(stale_, [seams = seams_, theme = &theme_] {
        auto entries = seams.ui->CommandCatalog();
        const auto close = [ui = seams.ui] { ui->CloseView(kCommandsViewId); };
        return seams.widgets->FilterableMenuModal(
            [entries = std::move(entries)](const std::string& filter) {
                return BuildCommandRows(entries, filter);
            },
            {.window_title = "commands",
             .hint_line = " enter: insert into the prompt  ·  /help <command>: detail  ·  "
                          "esc: close",
             .empty_message = " (no commands match)"},
            // The overlay is where you FIND a command: leaving it should not
            // mean retyping what you found.
            {.on_select =
                 [actions = seams.actions, close](const client::MenuRow& row) {
                     actions->InsertPrompt("/" + row.id + " ");
                     close();
                 },
             .on_close = close},
            *theme);
    });
}

bool KeymapCommand::Run(const std::vector<std::string>&, const std::string&) {
    ui_->ShowView(kKeymapViewId);  // arguments ignored: there is no per-binding detail
    return true;
}

bool HelpCommand::Run(const std::vector<std::string>& positional, const std::string&) {
    if (!positional.empty()) return false;  // /help <command>: the server's
    ui_->ShowView(kCommandsViewId);
    return true;
}

}  // namespace minimal::help_view
