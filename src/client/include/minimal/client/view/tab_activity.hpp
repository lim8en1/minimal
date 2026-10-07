#pragma once

// HostUi::Activity's source: every tab and its tool calls, as plugins see them
// (registries.hpp TabActivitySnapshot), built from AppState's tab list.

#include <cstdint>
#include <memory>

#include "minimal/client/plugins/attention_state.hpp"
#include "minimal/client/plugins/plugin_view_entry.hpp"
#include "minimal/client/sdk/client_registries.hpp"
#include "minimal/client/state.hpp"

namespace minimal::client {

// The status vocabulary plugins read ("idle", "thinking", ...).
const char* StateWord(SessionStateKind state);

// One tool call as plugins see it.
plugins::ToolActivity ToolActivityOf(const ToolExecutionState& tool);

// One tab as plugins see it; its tool rows only if `tab` carries them.
// `with_tools` false leaves `tools` empty: a view that never reads them (the
// tab bar, every render) skips a JSON summary per running tool per tab.
plugins::TabActivity ActivityOf(const AppState::TabSnapshot& tab, bool with_tools = true);

// What `filter` keeps out of the bar: the tabs of `all` that `visible` lacks,
// and how many of those carry attention (the glyph, not the state: Waiting
// and a plugin raise are the same signal). Inactive for TabFilter::All.
plugins::TabBarBadge ComputeTabBarBadge(const std::vector<AppState::TabSnapshot>& all,
                                        const std::vector<AppState::TabSnapshot>& visible,
                                        TabFilter filter);

// Rebuilds the snapshot only when something in it changed -- any tab's tool
// rows or output (AppState::ToolsVersion), or a tab's name, state, attention,
// focus or kind -- and otherwise hands back the last one, so a view keyed on
// its version redraws on change, not per frame. UI thread.
class TabActivityFeed {
public:
    std::shared_ptr<const plugins::TabActivitySnapshot> Get(const AppState& app);

private:
    std::uint64_t key_ = 0;
    // Nothing moved at all since the last call (a mouse move's render): the
    // key's own light snapshot and hashing are skipped too. Not the key
    // itself -- UiVersion moves on every text delta, which changes nothing
    // here.
    std::uint64_t seen_ui_version_ = 0;
    std::uint64_t seen_tools_version_ = 0;
    std::shared_ptr<const plugins::TabActivitySnapshot> last_;
};

}  // namespace minimal::client
