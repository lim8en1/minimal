// Partial-registration-rollback fixture: register_views succeeds (claims
// "fixture.panel", the SAME id fixture-client's own good view uses), then
// register_renderers throws. LoadOne must roll the view claim back -- a
// LATER, healthy plugin reusing "fixture.panel" (fixture-client) must load
// clean, not hit a fatal collision against a plugin that never actually
// loaded.
#include <stdexcept>

#include "minimal/client/plugins/client_plugin_abi.h"

namespace {

namespace plugins = minimal::client::plugins;

class PartialThrowPanelView final : public plugins::View {
public:
    const plugins::ViewSpec& spec() const override { return spec_; }
    ftxui::Component component() override {
        return ftxui::Renderer([] { return ftxui::text("partial-throw panel"); });
    }

private:
    plugins::ViewSpec spec_{"fixture.panel", "Partial Throw Panel", plugins::ViewPlacement::Panel};
};

}  // namespace

extern "C" {
const char* minimal_client_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_client_plugin_name() { return "fixture-client-partial-throw"; }
const char* minimal_client_plugin_version() { return "1.0.0"; }

void minimal_client_plugin_register_views(plugins::ViewRegistry& registry,
                                          const plugins::ClientPluginRegistrationContext&) {
    registry.Register(std::make_unique<PartialThrowPanelView>());
}

void minimal_client_plugin_register_renderers(
    plugins::ToolResultRendererRegistry&, const plugins::ClientPluginRegistrationContext&) {
    throw std::runtime_error("register_renderers boom");
}
}
