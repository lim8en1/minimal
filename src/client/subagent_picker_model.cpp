#include "minimal/client/subagent_picker_model.hpp"

#include <algorithm>
#include <chrono>

#include "minimal/client/picker_model.hpp"
#include "minimal/client/utf8.hpp"

namespace minimal::client {

namespace {

bool MatchesFilter(const v1::SessionSummary& session, const std::string& lowered_filter) {
    if (lowered_filter.empty()) return true;
    return ToLower(session.name()).find(lowered_filter) != std::string::npos ||
           ToLower(session.session_id()).find(lowered_filter) != std::string::npos;
}

// A subagent row is created nameless server-side (subagent_manager.cpp mints
// the row with name=""); a titler or /rename may have named it since. The raw
// id as the fallback label (truncated) beats a row of empty strings.
std::string DisplayName(const v1::SessionSummary& session) {
    if (!session.name().empty()) return session.name();
    std::string id = session.session_id();
    if (id.size() > 12) id = id.substr(0, 12);
    return id;
}

std::string Label(const v1::SessionSummary& session, std::int64_t now_unix_ms) {
    // Same leading-glyph convention as the session picker's Label
    // (picker_model.cpp): the live dot scans down the column; ended rows
    // indent to align. State vocabulary is deliberately coarse -- the wire
    // summary carries is_live only (see the header).
    const std::string state = session.is_live() ? "running" : "ended";
    return std::string(session.is_live() ? "● " : "  ") + DisplayName(session) + "  ·  " + state +
           "  ·  " + RelativeAge(session.updated_at_unix_ms(), now_unix_ms);
}

}  // namespace

std::vector<SubagentPickerRow> BuildSubagentPickerRows(
    const std::vector<v1::SessionSummary>& sessions, bool include_ended,
    const std::string& filter) {
    const std::int64_t now_unix_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    const std::string lowered_filter = ToLower(filter);

    std::vector<const v1::SessionSummary*> matched;
    matched.reserve(sessions.size());
    for (const auto& session : sessions) {
        if (!include_ended && !session.is_live()) continue;
        if (!MatchesFilter(session, lowered_filter)) continue;
        matched.push_back(&session);
    }

    // Live first, newest activity within each group (stable_sort preserves
    // the server's updated_at DESC delivery order for ties).
    std::stable_sort(matched.begin(), matched.end(),
                     [](const v1::SessionSummary* a, const v1::SessionSummary* b) {
                         if (a->is_live() != b->is_live()) return a->is_live();
                         return false;
                     });

    std::vector<SubagentPickerRow> rows;
    rows.reserve(matched.size());
    for (const auto* session : matched) {
        rows.push_back(SubagentPickerRow{Label(*session, now_unix_ms), session->session_id(),
                                         session->is_live()});
    }
    return rows;
}

}  // namespace minimal::client
