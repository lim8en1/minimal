#pragma once

// Reads a whole session back through the sanctioned data path
// (ClientApi::GetMessages), newest page first, into one ascending Transcript.
// Blocking: call it from the plugin's worker, never the UI thread.

#include <optional>
#include <string>

#include "export_render.hpp"
#include "minimal/client/sdk/host.hpp"

namespace minimal::session_export {

// nullopt: a page failed or timed out (the caller says so and offers a retry;
// a partial transcript exported as if whole would be a silent lie).
std::optional<Transcript> FetchTranscript(client::plugins::ClientApi& api,
                                          const std::string& session_id);

}  // namespace minimal::session_export
