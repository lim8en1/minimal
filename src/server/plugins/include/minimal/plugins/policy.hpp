#pragma once

// plugins policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <cstddef>

#include "minimal/common/units.hpp"

namespace minimal::plugins {

// One PluginStorage value: the KV seam is a plugin's persistence, not scratch
// space, and one buggy or hostile plugin must not grow the host's database
// without limit. Real state blobs are kilobytes.
inline constexpr std::size_t kMaxPluginStorageValueBytes = common::kMiB;

}  // namespace minimal::plugins
