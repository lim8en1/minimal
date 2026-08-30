#pragma once

#include <cstdint>
#include <optional>

#include "minimal/config/settings_config.hpp"

namespace minimal::agent {

// Resolves the configured compaction trigger strategy to the concrete occupancy
// threshold it means for this session's window. Returns 0 ("never fires") when
// the strategy is Never, when a window-relative strategy meets a provider entry
// that declared no context_window_tokens, or when the computed threshold is
// non-positive (a buffer at least as large as the window itself).
std::int64_t resolve_compaction_trigger(const config::CompactionTriggerSettings& trigger,
                                        std::optional<std::int64_t> context_window_tokens);

}  // namespace minimal::agent
