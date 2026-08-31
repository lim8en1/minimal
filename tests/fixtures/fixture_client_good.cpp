// A well-formed CLIENT plugin: registers one of every additive slot -- a
// status segment, a tool-result renderer, a message-kind renderer, a syntax
// grammar, and two views (panel + modal). The exclusive slots are left alone
// so the core defaults stand (fixture_client_exclusive covers those).
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
    explicit FixtureSegment(std::string echo) : echo_(std::move(echo)) {}
    std::string_view id() const override { return "fixture_seg"; }
    std::string Render() const override { return echo_; }

private:
    std::string echo_;
};

class FixtureToolRenderer final : public plugins::ToolResultRenderer {
public:
    std::string_view tool_name() const override { return "fixture_tool"; }
    ftxui::Element Render(const nlohmann::json&, std::string_view result_text, bool,
                          const Theme&) const override {
        return ftxui::text("fixture rendered: " + std::string(result_text));
    }
};

// Replaces the core Thinking default: the loader test asserts the claim via
// the registry's claims() and the render via the registry dispatch.
class FixtureThinkingRenderer final : public plugins::MessageRenderer {
public:
    ftxui::Element Render(const ChatRow& row, const Theme&) const override {
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

extern "C" {
const char* minimal_client_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_client_plugin_name() { return "fixture-client"; }
const char* minimal_client_plugin_version() { return "1.0.0"; }

void minimal_client_plugin_register_status_segments(
    plugins::StatusSegmentRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    std::string echo = "fixture-seg-default";
    if (context.config.is_object()) {
        if (auto it = context.config.find("echo_value");
            it != context.config.end() && it->is_string()) {
            echo = it->get<std::string>();
        }
    }
    registry.Register(std::make_unique<FixtureSegment>(echo));
}

void minimal_client_plugin_register_renderers(
    plugins::ToolResultRendererRegistry& registry,
    const plugins::ClientPluginRegistrationContext&) {
    registry.Register(std::make_unique<FixtureToolRenderer>());
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

void minimal_client_plugin_register_views(plugins::ViewRegistry& registry,
                                          const plugins::ClientPluginRegistrationContext&) {
    registry.Register(std::make_unique<FixturePanelView>());
    registry.Register(std::make_unique<FixtureModalView>());
}
}
