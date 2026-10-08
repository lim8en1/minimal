// session-export -- `/export`: the focused session's transcript to a .md or
// .txt file, through a modal (export_view.hpp). Client-only: it reads the
// transcript over the same GetMessages path scrollback uses, and writes on
// the machine the user is at -- which a server command could not promise.

#include <filesystem>
#include <memory>
#include <system_error>

#include "export_view.hpp"
#include "minimal/client/sdk/client_plugin_abi.h"

namespace plugins = minimal::client::plugins;

namespace {

// The data path, the focused tab and the widget kit are all the modal
// stands on; a test host offering none gets neither the view nor a command
// that would open nothing.
bool Hosted(const plugins::ClientPluginRegistrationContext& context) {
    return context.client_api != nullptr && context.focus != nullptr && context.ui != nullptr &&
           context.widgets != nullptr;
}

// The directory the client was started from, read once at load: the default
// export location, which a later chdir anywhere in the process must not move.
const std::filesystem::path& StartDir() {
    static const std::filesystem::path dir = [] {
        std::error_code ec;
        auto cwd = std::filesystem::current_path(ec);
        return ec ? std::filesystem::path(".") : cwd;
    }();
    return dir;
}

}  // namespace

extern "C" {

MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("session-export")

void minimal_client_plugin_register_views(plugins::ViewRegistry& registry,
                                          const plugins::ClientPluginRegistrationContext& context) {
    if (!Hosted(context)) return;
    registry.Register(std::make_unique<minimal::session_export::ExportView>(
        context.client_api, context.focus, context.ui, context.widgets, StartDir()));
}

void minimal_client_plugin_register_client_commands(
    plugins::ClientCommandRegistry& registry,
    const plugins::ClientPluginRegistrationContext& context) {
    if (!Hosted(context)) return;
    registry.Register(std::make_unique<minimal::session_export::ExportCommand>(context.ui));
}

}  // extern "C"
