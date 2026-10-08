// @PLUGIN_NAME@ -- a user client plugin (scaffold).
//
// The three required symbols below are the capability floor; every
// minimal_client_plugin_register_* hook is OPTIONAL (dlsym'd, called only
// when present). This scaffold registers one status-bar segment; the other
// hooks (see src/client/plugins/include/minimal/client/sdk/
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
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "minimal/client/sdk/client_plugin_abi.h"

namespace {

namespace plugins = minimal::client::plugins;

// One status-bar segment. It places itself (placement(): right side, after
// the bundled segments); ui.json's status_segments can hide or move it by id
// ("@PLUGIN_NAME@"), and `minimal status-bar` shows where it lands. Duplicate
// ids across plugins are fatal at startup, like every other name collision.
//
// CONTRACT: Render runs on the render thread each frame (~20 Hz). It must be
// cheap and must never block -- fetch data (plugin RPCs, sessions) on your
// own background thread and read a cached answer here.
class ClockSegment final : public plugins::StatusSegment {
public:
    std::string_view id() const override { return "@PLUGIN_NAME@"; }

    std::optional<plugins::StatusPlacement> placement() const override {
        return plugins::StatusPlacement{plugins::StatusPlacement::Side::kRight, 100};
    }

    plugins::StatusValue Render(const plugins::TabStatus& focused) const override {
        (void)focused;  // the focused tab's state
        std::time_t now = std::time(nullptr);
        std::tm utc{};
        gmtime_r(&now, &utc);
        char buf[9];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &utc);
        return {buf, ""};
    }
};

}  // namespace

extern "C" {

// The tag and version baked into THIS .so at compile time -- not
// common::AbiTag(), which binds to the running host's copy at dlopen and
// would always match it, letting a stale plugin load.
MINIMAL_CLIENT_PLUGIN_ABI_SYMBOLS("@PLUGIN_NAME@")

void minimal_client_plugin_register_status_segments(
    plugins::StatusSegmentRegistry& registry,
    const plugins::ClientPluginRegistrationContext&) {
    registry.Register(std::make_unique<ClockSegment>());
}

}  // extern "C"
