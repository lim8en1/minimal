#pragma once

// tools-web policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>

#include "minimal/common/units.hpp"

namespace minimal::tools_web {

// --- plugins.json tools-web.config defaults ------------------------------------------

// web_fetch: a page over a slow link fits; a dead host is reported in seconds.
// Redirects followed: a canonicalization hop or two and a login bounce, not a
// loop.
inline constexpr std::chrono::milliseconds kDefaultFetchTimeout = std::chrono::seconds(15);
inline constexpr int kDefaultFetchMaxRedirects = 5;
// Text returned to the model: a long article by default; a model-supplied
// max_bytes is clamped to the ceiling. The overflow spills to a file the read
// tool pages through.
inline constexpr int kDefaultFetchDefaultMaxBytes = 256 * static_cast<int>(common::kKiB);
inline constexpr int kDefaultFetchMaxBytes = 2 * static_cast<int>(common::kMiB);

// web_search: results shown by default and at most, and the API's budget.
inline constexpr int kDefaultSearchDefaultMaxResults = 5;
inline constexpr int kDefaultSearchMaxResults = 20;
inline constexpr std::chrono::milliseconds kDefaultSearchTimeout = std::chrono::seconds(10);

}  // namespace minimal::tools_web
