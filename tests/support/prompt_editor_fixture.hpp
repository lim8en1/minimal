#pragma once

// The bundled prompt editor (plugins/client/prompt-editor) over a fake
// PromptEditorHost, built from the pieces the tests vary: the submit paths,
// the @-mention answers, the command catalog and its session / subagent
// gates (applied here as core applies them), the paste flag, the timeline,
// and the keymap (null: the editor's declared default keys).

#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>

#include "prompt_editor.hpp"
#include "prompt_editor_factory.hpp"

#include "minimal/client/command_catalog.hpp"
#include "minimal/client/sdk/key_event_parser.hpp"
#include "minimal/client/plugins/attention_state.hpp"
#include "minimal/client/plugins/plugin_view_entry.hpp"
#include "minimal/client/sdk/client_registries.hpp"
#include "minimal/client/view/prompt_host.hpp"

namespace minimal_test {

using MentionAnswer = std::function<std::optional<std::vector<std::string>>(const std::string&)>;
using KeyMatches = std::function<bool(std::string_view, const ftxui::Event&)>;

// The editor's declared actions at their default keys.
inline bool DefaultEditorKeyMatches(std::string_view action, const ftxui::Event& event) {
    static const auto actions = minimal::client::prompt_editor::Factory().Actions();
    for (const auto& declared : actions) {
        if (declared.id != action) continue;
        for (const auto& key : minimal::client::ParseKeySpec(declared.default_key)) {
            if (key == event) return true;
        }
    }
    return false;
}

class FakeEditorHost final : public minimal::client::plugins::PromptEditorHost {
public:
    std::function<void(const std::string&)> on_text;
    std::function<void(const std::string&)> on_command;
    MentionAnswer mentions;
    minimal::client::CommandCatalogSource catalog;
    std::function<bool()> session_available;
    std::function<bool()> subagent_read_only;
    minimal::client::PromptHistory* history = nullptr;   // null: own_history
    minimal::client::PromptHistory own_history;
    KeyMatches key_matches;
    int redraws = 0;

    void SubmitChat(const std::string& text) override {
        if (on_text) on_text(text);
    }
    void SubmitCommand(const std::string& line) override {
        if (on_command) on_command(line);
    }
    std::shared_ptr<const std::vector<minimal::client::CommandCatalogEntry>> Commands() override {
        if (!catalog) return nullptr;
        const auto all = catalog();
        if (all == nullptr) return nullptr;
        const bool session_ok = !session_available || session_available();
        const bool subagent_only = subagent_read_only && subagent_read_only();
        if (all != source_ || session_ok != session_ok_ || subagent_only != subagent_only_ ||
            usable_ == nullptr) {
            std::vector<minimal::client::CommandCatalogEntry> usable;
            for (const auto& entry : *all) {
                if (!session_ok && !minimal::client::UsableWithoutSession(entry)) continue;
                if (subagent_only && !minimal::client::UsableOnSubagent(entry)) continue;
                usable.push_back(entry);
            }
            source_ = all;
            session_ok_ = session_ok;
            subagent_only_ = subagent_only;
            usable_ = std::make_shared<const std::vector<minimal::client::CommandCatalogEntry>>(
                std::move(usable));
        }
        return usable_;
    }
    std::optional<std::vector<std::string>> Mentions(const std::string& prefix) override {
        return mentions ? mentions(prefix) : std::vector<std::string>{};
    }
    minimal::client::plugins::PromptTimeline& History() override {
        return history != nullptr ? *history : own_history;
    }
    bool Matches(std::string_view action, const ftxui::Event& event) const override {
        return key_matches ? key_matches(action, event) : DefaultEditorKeyMatches(action, event);
    }
    void RequestRedraw() override { ++redraws; }

private:
    std::shared_ptr<const std::vector<minimal::client::CommandCatalogEntry>> source_;
    bool session_ok_ = true;
    bool subagent_only_ = false;
    std::shared_ptr<const std::vector<minimal::client::CommandCatalogEntry>> usable_;
};

// A host that lives as long as the test binary (an editor never outlives its
// host; tests build many and drop them freely).
inline FakeEditorHost& NewFakeEditorHost() {
    static std::deque<std::unique_ptr<FakeEditorHost>> hosts;
    return *hosts.emplace_back(std::make_unique<FakeEditorHost>());
}

// The editor over `state`, with the host built from the arguments.
inline ftxui::Component MakeTestPromptEditor(
    const minimal::client::Theme& theme, minimal::client::prompt_editor::PromptEditorState& state,
    std::function<void(const std::string&)> on_text,
    std::function<void(const std::string&)> on_command, MentionAnswer mentions,
    minimal::client::CommandCatalogSource catalog = nullptr,
    std::function<bool()> session_available = nullptr,
    std::function<bool()> subagent_read_only = nullptr, std::shared_ptr<bool> pasting = nullptr,
    minimal::client::PromptHistory* history = nullptr, KeyMatches key_matches = nullptr) {
    FakeEditorHost& host = NewFakeEditorHost();
    host.on_text = std::move(on_text);
    host.on_command = std::move(on_command);
    host.mentions = std::move(mentions);
    host.catalog = catalog;
    host.session_available = std::move(session_available);
    host.subagent_read_only = std::move(subagent_read_only);
    host.history = history;
    if (history != nullptr) history->SetCatalog(catalog);
    else host.own_history.SetCatalog(catalog);
    host.key_matches = std::move(key_matches);
    return minimal::client::prompt_editor::MakePromptEditor(theme, state, host,
                                                            std::move(pasting));
}

}  // namespace minimal_test
