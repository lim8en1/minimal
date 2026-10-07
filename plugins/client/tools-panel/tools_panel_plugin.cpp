// tools-panel -- every tab's tool calls and their live output (tools_panel.hpp).

#include <memory>

#include "minimal/client/sdk/client_plugin_abi.h"
#include "minimal/common/config_reader.hpp"
#include "minimal/common/plugin_config_label.hpp"
#include "policy.hpp"
#include "tools_panel.hpp"

namespace plugins = minimal::client::plugins;

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("tools-panel")

void minimal_client_plugin_register_views(plugins::ViewRegistry& registry,
                                          const plugins::ClientPluginRegistrationContext& context) {
    if (context.ui == nullptr) return;  // nothing to read the tabs through
    minimal::config::Reader reader(
        context.config,
        minimal::common::PluginConfigLabel(minimal::common::kClientPluginConfigDirName,
                                           "tools-panel"));
    const auto output_lines = static_cast<int>(
        reader.Int("output_lines", minimal::tools_panel::kDefaultOutputLines, 1));
    reader.Finish();
    registry.Register(std::make_unique<minimal::tools_panel::ToolsPanelView>(context.ui, output_lines));
}

}  // extern "C"
