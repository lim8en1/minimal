// @PLUGIN_NAME@ -- a user client plugin (scaffold).
//
// The three required symbols below are the capability floor; every
// minimal_client_plugin_register_* hook is OPTIONAL (dlsym'd, called only
// when present). This scaffold registers one status-bar segment; the other
// hooks (see src/client/plugins/include/minimal/client/plugins/
// client_plugin_abi.h):
//   register_views              -- self-contained panels/modals
//   register_start_views        -- the initial view (logo is the example)
//   register_renderers          -- per-tool result renderers
//   register_message_renderers  -- per-ChatRowKind presentation
//   register_syntax_grammars    -- tree-sitter grammars per language
//   register_chat_surface       -- replace the whole chat view (exclusive)
//   register_markdown_engine    -- replace the markdown pipeline (exclusive)
//
// Config reaches you as context.config: the verbatim "config" object on this
// plugin's entry in client-plugins.json.

#include <ctime>
#include <string>
#include <string_view>

#include "minimal/client/plugins/client_plugin_abi.h"

namespace {

namespace plugins = minimal::client::plugins;

// One status-bar segment. The id is referenced from ui.json's status_format
// ("{@PLUGIN_NAME@}"); an id that never appears in the format string renders
// nothing. Duplicate ids across plugins are fatal at startup, like every
// other name collision.
//
// CONTRACT: Render runs on the render thread each frame (~20 Hz). It must be
// cheap and must never block -- fetch data (plugin RPCs, sessions) on your
// own background thread and read a cached answer here.
class ClockSegment final : public plugins::StatusSegment {
public:
    std::string_view id() const override { return "@PLUGIN_NAME@"; }

    std::string Render(const std::string& focused_session_id) const override {
        (void)focused_session_id;  // the focused tab's session, empty if none
        std::time_t now = std::time(nullptr);
        std::tm utc{};
        gmtime_r(&now, &utc);
        char buf[9];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &utc);
        return buf;
    }
};

}  // namespace

extern "C" {

const char* minimal_client_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_client_plugin_name() { return "@PLUGIN_NAME@"; }
const char* minimal_client_plugin_version() { return minimal::common::Version(); }

void minimal_client_plugin_register_status_segments(
    plugins::StatusSegmentRegistry& registry,
    const plugins::ClientPluginRegistrationContext&) {
    registry.Register(std::make_unique<ClockSegment>());
}

}  // extern "C"
