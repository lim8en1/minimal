// A well-formed plugin: correct ABI tag, registers one tool and one provider kind.
#include "minimal/plugins/plugin_abi.h"

namespace {

class FixtureTool : public minimal::tools::Tool {
public:
    explicit FixtureTool(std::string echo) : echo_(std::move(echo)) {
        spec_.name = "fixture_tool";
        spec_.category = minimal::tools::ToolCategory::ReadOnly;
        spec_.definition.name = spec_.name;
        spec_.definition.description = "a tool contributed by a test fixture plugin";
        spec_.definition.input_schema = nlohmann::json::object();
    }
    const minimal::tools::ToolSpec& spec() const override { return spec_; }
    minimal::tools::ToolResult execute(const nlohmann::json&,
                                       minimal::tools::ToolContext&) override {
        return {false, echo_};
    }
private:
    minimal::tools::ToolSpec spec_;
    std::string echo_;
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

// Track N: the plugin also registers a model catalog under the SAME kind
// string, so the loader tests can observe the seam through a real dlopen.
class FixtureCatalog : public minimal::llm::ModelCatalog {
public:
    std::vector<minimal::llm::DiscoveredModel> List(
        const minimal::llm::ProviderConfig&) const override {
        return {{"fixture-model", 4321, nlohmann::json::object(), std::nullopt}};
    }
};

// The variant-params relocation seam (ARCHITECTURE.md, "Core vs. plugin
// surface"): the plugin also registers one under the SAME kind string, so
// the loader tests can observe this seam through a real dlopen too.
class FixtureVariantParams : public minimal::llm::VariantParamsProvider {
public:
    std::pair<nlohmann::json, bool> DeriveParameters(const std::string& effort_word,
                                                     std::optional<int>) const override {
        return {nlohmann::json{{"fixture_effort", effort_word}}, false};
    }
};

}  // namespace

extern "C" {
const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "fixture-good"; }
const char* minimal_plugin_version() { return "1.0.0"; }

void minimal_plugin_register_tools(
    minimal::tools::ToolRegistry& registry,
    const minimal::plugins::PluginRegistrationContext& context) {
    // The plugin's own config slice, echoed back through the tool so the
    // loader tests can observe per-plugin config delivery through a real
    // dlopen -- including that a context carries ONLY this plugin's slice.
    std::string echo = "fixture ok";
    if (context.config.is_object()) {
        if (auto it = context.config.find("echo_value");
            it != context.config.end() && it->is_string()) {
            echo = it->get<std::string>();
        }
    }
    registry.Register(std::make_unique<FixtureTool>(echo));
}
void minimal_plugin_register_provider_factories(
    minimal::llm::ProviderFactoryRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Register("fixture", [](const minimal::llm::ProviderConfig&) {
        return std::make_unique<FixtureProvider>();
    });
}
void minimal_plugin_register_mention_resolver(
    minimal::agent::MentionResolverRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.TrySetOverride(std::make_shared<FixtureResolver>());
}
void minimal_plugin_register_model_catalogs(
    minimal::llm::ModelCatalogRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Register("fixture", std::make_shared<FixtureCatalog>());
}
void minimal_plugin_register_variant_params(
    minimal::llm::VariantParamsRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Register("fixture", std::make_shared<FixtureVariantParams>());
}
}
