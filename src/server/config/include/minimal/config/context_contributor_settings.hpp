#pragma once

// Split out of settings_config.hpp: the SDK's context_contributor.hpp needs
// this struct, not the whole settings model.

#include <cstdint>
#include <string>
#include <vector>

#include "minimal/config/policy.hpp"

namespace minimal::config {

struct ContextContributorSettings {
    // Plugin names whose contributed sections are skipped -- the operator's off
    // switch that does not require disabling the whole plugin.
    std::vector<std::string> disabled;
    // Per-section body cap in bytes. 0 = uncapped.
    std::int64_t max_bytes_per_section = kDefaultContextSectionMaxBytes;
};

}  // namespace minimal::config
