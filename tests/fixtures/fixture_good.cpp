// A well-formed plugin: correct ABI tag, registers one tool and one provider kind.
#include "minimal/plugins/plugin_abi.h"

namespace {

class FixtureTool : public minimal::tools::Tool {
public:
    FixtureTool() {
        spec_.name = "fixture_tool";
        spec_.category = minimal::tools::ToolCategory::ReadOnly;
        spec_.definition.name = spec_.name;
        spec_.definition.description = "a tool contributed by a test fixture plugin";
        spec_.definition.input_schema = nlohmann::json::object();
    }
    const minimal::tools::ToolSpec& spec() const override { return spec_; }
    minimal::tools::ToolResult execute(const nlohmann::json&,
                                       minimal::tools::ToolContext&) override {
        return {false, "fixture ok"};
    }
private:
    minimal::tools::ToolSpec spec_;
};

class FixtureProvider : public minimal::llm::LlmProvider {
public:
    minimal::llm::ChatResponse Complete(const minimal::llm::ChatRequest&) override { return {}; }
    minimal::llm::ChatResponse StreamComplete(const minimal::llm::ChatRequest&,
                                              const minimal::llm::StreamEventSink&) override {
        return {};
    }
};

class FixtureResolver : public minimal::agent::MentionResolver {
public:
    std::vector<std::string> Suggest(std::string_view, const std::filesystem::path&,
                                     int) const override {
        return {"fixture-resolved"};
    }
};

}  // namespace

extern "C" {
const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "fixture-good"; }
const char* minimal_plugin_version() { return "1.0.0"; }

void minimal_plugin_register_tools(minimal::tools::ToolRegistry& registry,
                                   const minimal::plugins::ToolRegistrationContext&) {
    registry.Register(std::make_unique<FixtureTool>());
}
void minimal_plugin_register_provider_factories(minimal::llm::ProviderFactoryRegistry& registry) {
    registry.Register("fixture", [](const minimal::llm::ProviderConfig&) {
        return std::make_unique<FixtureProvider>();
    });
}
void minimal_plugin_register_mention_resolver(
    minimal::agent::MentionResolverRegistry& registry) {
    registry.Register(std::make_shared<FixtureResolver>());
}
}
