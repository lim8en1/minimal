#pragma once

// plugins policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>

#include "minimal/common/units.hpp"

namespace minimal::plugins {

// One PluginStorage value: the KV seam is a plugin's persistence, not scratch
// space, and one buggy or hostile plugin must not grow the host's database
// without limit. Real state blobs are kilobytes.
inline constexpr std::size_t kMaxPluginStorageValueBytes = common::kMiB;

// Idle heartbeat on a plugin's push subscription (the checklist and cost
// hubs). Load-bearing: a client can only end a subscription from its payload
// callback, so an idle stream would pin the subscriber's teardown until the
// next real push; the heartbeat bounds that to a blink.
inline constexpr std::chrono::milliseconds kSubscriptionHeartbeat{500};

// A plugin's config check (minimal_plugin_check_config), all its probes
// together: room for a slow model listing or a cold local server, short
// enough that "checking..." never looks hung.
inline constexpr std::chrono::milliseconds kConfigCheckBudget = std::chrono::seconds(20);

}  // namespace minimal::plugins
