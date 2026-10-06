// A well-formed CLIENT plugin: registers one of every additive slot -- a
// status segment, a tool-result renderer, a message-kind renderer, a syntax
// grammar, a start view, and two views (panel + modal). The exclusive slots
// are left alone so the core defaults stand (fixture_client_exclusive covers
// those).
//
// The status segment echoes this plugin's own config ("echo_value") through
// Render(), so loader tests can observe per-plugin config delivery through a
// real dlopen.
#include "minimal/client/plugins/client_plugin_abi.h"

namespace {

using minimal::client::ChatRow;
using minimal::client::Theme;
namespace plugins = minimal::client::plugins;

class FixtureSegment final : public plugins::StatusSegment {
public:
    FixtureSegment(std::string echo, plugins::AttentionSink* attention, std::string raise_tab)
        : echo_(std::move(echo)), attention_(attention), raise_tab_(std::move(raise_tab)) {}
    std::string_view id() const override { return "fixture_seg"; }
    plugins::StatusValue Render(const plugins::TabStatus&) const override {
        // POST-LOAD seam use: the sink pointer was captured from the
        // registration context; Render is where a real plugin would use it
        // (attention raises happen while the TUI runs, long after LoadAll).
        // If the host frees the sink at the end of its loader-constructor
        // expression, this call is a use-after-free (ASan) and the raise
        // never lands. Opt-in via the "raise_tab" config key so the loader
        // tests using a default config stay unaffected.
        if (attention_ != nullptr && !raise_tab_.empty()) attention_->RaiseTab(raise_tab_);
        return {echo_, ""};
    }

private:
    std::string echo_;
    plugins::AttentionSink* attention_;
    std::string raise_tab_;
};

class FixtureToolRenderer final : public plugins::ToolResultRenderer {
public:
    std::string_view tool_name() const override { return "fixture_tool"; }
    ftxui::Element Render(const plugins::ToolResultView& result, const Theme&,
                          const plugins::RowRenderContext&) const override {
        return ftxui::text("fixture rendered: " + std::string(result.result_text));
    }
};

class FixtureDisplayRenderer final : public plugins::DisplayRenderer {
public:
    std::string_view kind() const override { return "fixture_kind"; }
    ftxui::Element Render(const plugins::ToolResultView& result, const Theme&,
                          const plugins::RowRenderContext&) const override {
        return ftxui::text("fixture kind: " + result.display.value("label", std::string{}));
    }
};

// Replaces the core Thinking default: the loader test asserts the claim via
// the registry's claims() and the render via the registry dispatch.
class FixtureThinkingRenderer final : public plugins::MessageRenderer {
public:
    ftxui::Element Render(const ChatRow& row, const Theme&,
                          const plugins::RowRenderContext&) const override {
        return ftxui::text("fixture thinking: " + std::to_string(row.text.size()));
    }
};

class FixtureGrammar final : public plugins::SyntaxGrammar {
public:
    std::string_view Language() const override { return "minilang"; }
    minimal::client::LanguageFnPtr LanguageFn() const override { return nullptr; }
    std::span<const plugins::SyntaxRule> Rules() const override { return rules_; }

private:
    std::vector<plugins::SyntaxRule> rules_{
        {"shout", minimal::client::HighlightCategory::Keyword},
    };
};

class FixtureStartView final : public plugins::StartView {
public:
    const plugins::ViewSpec& spec() const override { return spec_; }
    // Echoes the session id INTO the art so the loader test can observe the
    // per-render context delivery (empty = app-level empty state).
    ftxui::Element Render(const std::string& session_id, const Theme&) const override {
        return ftxui::text("fixture start:" + session_id);
    }

private:
    plugins::ViewSpec spec_{"fixture.start", "Fixture Start"};
};

class FixturePanelView final : public plugins::View {
public:
    const plugins::ViewSpec& spec() const override { return spec_; }
    ftxui::Component component() override {
        return ftxui::Renderer([] { return ftxui::text("fixture panel"); });
    }

private:
    plugins::ViewSpec spec_{"fixture.panel", "Fixture Panel", plugins::ViewPlacement::Panel,
                            "ctrl+p"};
};

class FixtureModalView final : public plugins::View {
public:
    const plugins::ViewSpec& spec() const override { return spec_; }
    ftxui::Component component() override {
        return ftxui::Renderer([] { return ftxui::text("fixture modal"); });
    }

private:
    plugins::ViewSpec spec_{"fixture.modal", "Fixture Modal", plugins::ViewPlacement::Modal,
                            "ctrl+m"};
};

}  // namespace

// A client-side /-command: echoes its raw arguments into a counter the test
// can read back through the registry.
class FixtureClientCommand final : public plugins::ClientCommand {
public:
    std::string_view name() const override { return "fixture-cmd"; }
    std::string_view summary() const override { return "fixture client command"; }
    bool Run(const std::vector<std::string>&, const std::string& raw) override {
        last_raw = raw;
        return true;
    }
    std::string last_raw;
};

extern "C" {
// The compile-time tag, not AbiTag(): see client_plugin_abi.h.
const char* minimal_client_plugin_abi_tag() { return MINIMAL_ABI_TAG; }
const char* minimal_client_plugin_name() { return "fixture-client"; }
const char* minimal_client_plugin_version() { return "1.0.0"; }

void minimal_client_plugin_register_status_segments(
    plugins::StatusSegmentRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    std::string echo = "fixture-seg-default";
    std::string raise_tab;
    if (context.config.is_object()) {
        if (auto it = context.config.find("echo_value");
            it != context.config.end() && it->is_string()) {
            echo = it->get<std::string>();
        }
        if (auto it = context.config.find("raise_tab");
            it != context.config.end() && it->is_string()) {
            raise_tab = it->get<std::string>();
        }
    }
    registry.Register(std::make_unique<FixtureSegment>(echo, context.attention, raise_tab));
}

void minimal_client_plugin_register_renderers(
    plugins::ToolResultRendererRegistry& registry,
    const plugins::ClientPluginRegistrationContext&) {
    registry.Register(std::make_unique<FixtureToolRenderer>());
}

void minimal_client_plugin_register_display_renderers(
    plugins::DisplayRendererRegistry& registry,
    const plugins::ClientPluginRegistrationContext&) {
    registry.Register(std::make_unique<FixtureDisplayRenderer>());
}

void minimal_client_plugin_register_message_renderers(
    plugins::MessageRendererRegistry& registry,
    const plugins::ClientPluginRegistrationContext&) {
    registry.Register(minimal::client::ChatRowKind::Thinking,
                      std::make_unique<FixtureThinkingRenderer>());
}

void minimal_client_plugin_register_syntax_grammars(
    plugins::SyntaxGrammarRegistry& registry, const plugins::ClientPluginRegistrationContext&) {
    registry.Register(std::make_unique<FixtureGrammar>());
}

void minimal_client_plugin_register_start_views(
    plugins::StartViewRegistry& registry, const plugins::ClientPluginRegistrationContext&) {
    registry.Register(std::make_unique<FixtureStartView>());
}

void minimal_client_plugin_register_client_commands(
    plugins::ClientCommandRegistry& registry, const plugins::ClientPluginRegistrationContext&) {
    registry.Register(std::make_unique<FixtureClientCommand>());
}

void minimal_client_plugin_register_views(plugins::ViewRegistry& registry,
                                          const plugins::ClientPluginRegistrationContext&) {
    registry.Register(std::make_unique<FixturePanelView>());
    registry.Register(std::make_unique<FixtureModalView>());
}
}
