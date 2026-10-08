#include "transcript_fetch.hpp"

#include <algorithm>
#include <tuple>
#include <utility>

#include "policy.hpp"

namespace minimal::session_export {

std::optional<Transcript> FetchTranscript(client::plugins::ClientApi& api,
                                          const std::string& session_id) {
    Transcript transcript;
    std::optional<std::int64_t> before;
    for (;;) {
        auto page = api.GetMessages(session_id, before, kPageSize);
        if (!page) return std::nullopt;
        // Pages come newest first; collected descending, reversed once below.
        for (auto& stored : page->messages) transcript.messages.push_back(std::move(stored));
        for (auto& compaction : page->compactions) {
            transcript.compactions.push_back(std::move(compaction));
        }
        // An empty page cannot advance the cursor: stop rather than loop.
        if (!page->has_more || page->messages.empty()) break;
        before = transcript.messages.back().seq();
    }
    std::reverse(transcript.messages.begin(), transcript.messages.end());
    auto& compactions = transcript.compactions;
    std::sort(compactions.begin(), compactions.end(), [](const auto& a, const auto& b) {
        return std::tuple(a.at_seq(), a.created_at_unix_ms()) <
               std::tuple(b.at_seq(), b.created_at_unix_ms());
    });
    return transcript;
}

}  // namespace minimal::session_export
