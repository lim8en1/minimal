#pragma once

// help-view -- the TUI's two reference overlays, as plugin views: the key
// bindings ("help.overlay": alt+h, /keymap) and the /-commands
// ("help.commands": a bare /help). Both read the host afresh on every open
// (HostUi::KeyBindings / CommandCatalog -- core's vocabulary, the focused
// tab's commands) and draw through the widget kit. Picking a command puts
// "/name " in the prompt (HostActions::InsertPrompt).

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/component.hpp>

#include "minimal/client/sdk/registries.hpp"
#include "minimal/client/sdk/theme.hpp"

namespace minimal::help_view {

namespace client = minimal::client;
namespace plugins = minimal::client::plugins;

// The keymap view keeps core's old action id, so a keybindings.json entry
// for it still binds it.
inline constexpr const char* kKeymapViewId = "help.overlay";
inline constexpr const char* kCommandsViewId = "help.commands";

// The keymap overlay's rows: every binding whose id, key or description
// contains `filter` (case-insensitively), a header per group, in the order
// the host gives (core's groups and curated order, then views by id); the
// key in a column of its own.
std::vector<client::MenuRow> BuildKeymapRows(std::vector<plugins::KeyBindingInfo> bindings,
                                             const std::string& filter);

// The commands overlay's rows: "/name  summary", with what kind it is.
std::vector<client::MenuRow> BuildCommandRows(const std::vector<client::CommandCatalogEntry>& entries,
                                              const std::string& filter);

// The host seams both views use (non-null; host-owned).
struct Seams {
    plugins::HostUi* ui;
    plugins::HostActions* actions;
    const plugins::WidgetKit* widgets;
};

class KeymapView final : public plugins::View {
public:
    explicit KeymapView(Seams seams) : seams_(seams) {}
    const plugins::ViewSpec& spec() const override;
    ftxui::Component component() override;
    void OnTheme(const client::Theme& theme) override { theme_ = theme; }
    // The bindings are read again, and the filter starts empty.
    void OnShow() const override { stale_->store(true); }

private:
    Seams seams_;
    client::Theme theme_;
    std::shared_ptr<std::atomic<bool>> stale_ = std::make_shared<std::atomic<bool>>(true);
};

class CommandsView final : public plugins::View {
public:
    explicit CommandsView(Seams seams) : seams_(seams) {}
    const plugins::ViewSpec& spec() const override;
    ftxui::Component component() override;
    void OnTheme(const client::Theme& theme) override { theme_ = theme; }
    // The focused tab's catalog is read again, and the filter starts empty.
    void OnShow() const override { stale_->store(true); }

private:
    Seams seams_;
    client::Theme theme_;
    std::shared_ptr<std::atomic<bool>> stale_ = std::make_shared<std::atomic<bool>>(true);
};

// `/keymap`: the key bindings.
class KeymapCommand final : public plugins::ClientCommand {
public:
    explicit KeymapCommand(plugins::HostUi* ui) : ui_(ui) {}
    std::string_view name() const override { return "keymap"; }
    std::string_view summary() const override { return "show all configured key bindings"; }
    bool Run(const std::vector<std::string>&, const std::string&) override;

private:
    plugins::HostUi* ui_;
};

// A bare `/help`: the commands overlay. `/help <command>` stays the
// server's: one command's usage is a short notice.
class HelpCommand final : public plugins::ClientCommand {
public:
    explicit HelpCommand(plugins::HostUi* ui) : ui_(ui) {}
    std::string_view name() const override { return "help"; }
    std::string_view summary() const override { return ""; }
    bool listed() const override { return false; }  // the server lists /help
    bool Run(const std::vector<std::string>& positional, const std::string&) override;

private:
    plugins::HostUi* ui_;
};

}  // namespace minimal::help_view
