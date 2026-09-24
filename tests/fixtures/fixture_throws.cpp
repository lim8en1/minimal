// A plugin whose registration callback THROWS.
//
// Not a mock, because the failure being tested is what happens to the process:
// the callback used to be invoked bare, so the exception propagated out of
// LoadOne, out of LoadAll, into main() -- which has no handler -- and killed the
// server with no diagnostic naming the plugin. A fake loader cannot demonstrate
// that; only dlopen'ing something that really throws can.
//
// It registers into several registries (a tool, a provider kind, a model
// catalog, a mention resolver, a context contributor, a service) and only
// THEN throws, from the last hook the loader runs -- so the tests can see
// that the rollback strips every one of them, and that each attribution
// window was closed on the way out (a window left open would mis-attribute
// every later plugin's registrations to this one).
#include <stdexcept>

#include "minimal/plugins/plugin_abi.h"

namespace {

class BeforeTheThrow : public minimal::tools::Tool {
public:
    BeforeTheThrow() {
        spec_.name = "registered_before_throw";
        spec_.category = minimal::tools::ToolCategory::ReadOnly;
        spec_.definition.name = spec_.name;
        spec_.definition.description = "registered just before the callback throws";
        spec_.definition.input_schema = nlohmann::json::object();
    }
    const minimal::tools::ToolSpec& spec() const override { return spec_; }
    minimal::tools::ToolResult execute(const nlohmann::json&,
                                       minimal::tools::ToolContext&) override {
        return {false, "ok"};
    }
private:
    minimal::tools::ToolSpec spec_;
};

class ThrowsProvider : public minimal::llm::LlmProvider {
public:
    minimal::llm::ChatResponse Complete(const minimal::llm::ChatRequest&) override { return {}; }
    minimal::llm::ChatResponse StreamComplete(const minimal::llm::ChatRequest&,
                                              const minimal::llm::StreamEventSink&) override {
        return {};
    }
};

class ThrowsCatalog : public minimal::llm::ModelCatalog {
public:
    std::vector<minimal::llm::DiscoveredModel> List(
        const minimal::llm::ProviderConfig&) const override {
        return {};
    }
};

class ThrowsResolver : public minimal::agent::MentionResolver {
public:
    std::vector<std::string> Suggest(std::string_view, const std::filesystem::path&,
                                     int) const override {
        return {};
    }
};

class ThrowsContributor : public minimal::agent::ContextContributor {
public:
    std::optional<minimal::agent::ContextSection> Contribute(
        const minimal::agent::ContextRequest&) override {
        return std::nullopt;
    }
};

int g_service = 0;

}  // namespace

extern "C" {
const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "fixture-throws"; }
const char* minimal_plugin_version() { return "1.0.0"; }

void minimal_plugin_register_tools(minimal::tools::ToolRegistry& registry,
                                    const minimal::plugins::PluginRegistrationContext&) {
    registry.Register(std::make_unique<BeforeTheThrow>());
}
void minimal_plugin_register_provider_factories(
    minimal::llm::ProviderFactoryRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Register("throws-kind", [](const minimal::llm::ProviderConfig&) {
        return std::make_unique<ThrowsProvider>();
    });
}
void minimal_plugin_register_mention_resolver(
    minimal::agent::MentionResolverRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.TrySetOverride(std::make_shared<ThrowsResolver>());
}
void minimal_plugin_register_context_contributors(
    minimal::agent::ContextContributorRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Register(std::make_shared<ThrowsContributor>());
}
void minimal_plugin_register_model_catalogs(
    minimal::llm::ModelCatalogRegistry& registry,
    const minimal::plugins::PluginRegistrationContext&) {
    registry.Register("throws-kind", std::make_shared<ThrowsCatalog>());
}
// The last hook the loader runs: everything above has already landed.
void minimal_plugin_register_services(minimal::plugins::ServiceRegistry& registry,
                                       const minimal::plugins::PluginRegistrationContext&) {
    registry.Provide("fixture-throws.service", &g_service);
    throw std::runtime_error("this plugin's registration is broken");
}
}
