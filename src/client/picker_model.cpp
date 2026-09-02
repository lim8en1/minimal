#include "minimal/client/picker_model.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>

namespace minimal::client {

namespace {

std::string ToLower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool MatchesFilter(const v1::SessionSummary& session, const std::string& lowered_filter) {
    if (lowered_filter.empty()) return true;
    return ToLower(session.name()).find(lowered_filter) != std::string::npos ||
           ToLower(session.session_id()).find(lowered_filter) != std::string::npos ||
           ToLower(session.project_root()).find(lowered_filter) != std::string::npos;
}

std::string HumanTime(std::int64_t unix_ms) {
    if (unix_ms <= 0) return "";
    const std::time_t seconds = static_cast<std::time_t>(unix_ms / 1000);
    std::tm local{};
    localtime_r(&seconds, &local);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M", &local);
    return buffer;
}

std::string Label(const v1::SessionSummary& session) {
    // S5.4: "live" marks a session with a Session object still running
    // server-side (attached or detached) -- selecting it attaches to the
    // live stream rather than resuming from the stored transcript, and its
    // turn may still be in flight.
    std::string label = session.name() + "  ·  " + std::to_string(session.message_count()) +
                        " msgs  ·  " + HumanTime(session.updated_at_unix_ms());
    if (session.is_live()) label += "  ·  live";
    return label;
}

}  // namespace

std::vector<PickerRow> BuildPickerRows(const std::vector<v1::SessionSummary>& sessions,
                                       const std::string& filter, PickerSort sort) {
    const std::string lowered_filter = ToLower(filter);

    std::vector<const v1::SessionSummary*> matched;
    matched.reserve(sessions.size());
    for (const auto& session : sessions) {
        if (MatchesFilter(session, lowered_filter)) matched.push_back(&session);
    }

    if (sort == PickerSort::kName) {
        std::stable_sort(matched.begin(), matched.end(),
                         [](const v1::SessionSummary* a, const v1::SessionSummary* b) {
                             const std::string name_a = ToLower(a->name());
                             const std::string name_b = ToLower(b->name());
                             if (name_a != name_b) return name_a < name_b;
                             return a->session_id() < b->session_id();
                         });
    } else {
        // kRecent: newest first. stable_sort preserves the server's own
        // (updated_at DESC, id ASC) tie-break for equal timestamps, since the
        // raw list already arrives in that order.
        std::stable_sort(matched.begin(), matched.end(),
                         [](const v1::SessionSummary* a, const v1::SessionSummary* b) {
                             return a->updated_at_unix_ms() > b->updated_at_unix_ms();
                         });
    }

    std::vector<PickerRow> rows;
    rows.reserve(matched.size() + 1);
    rows.push_back(PickerRow{"+ new session", ""});
    for (const auto* session : matched) {
        rows.push_back(PickerRow{Label(*session), session->session_id()});
    }
    return rows;
}

}  // namespace minimal::client
