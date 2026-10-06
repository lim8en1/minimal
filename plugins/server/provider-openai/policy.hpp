#pragma once

// provider-openai policy: every value here is a judgment call, not a fact or
// a derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>

namespace minimal::provider_openai {

// GET <base_url>/models for a discover entry: off the chat path, so a hung
// backend must not wedge the picker for a chat-length timeout. Local servers
// answer in milliseconds, remote catalogs in well under this.
inline constexpr std::chrono::milliseconds kListModelsTimeout = std::chrono::seconds(10);

}  // namespace minimal::provider_openai
