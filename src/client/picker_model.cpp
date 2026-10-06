#include "minimal/client/picker_model.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>

#include "minimal/client/utf8.hpp"
#include "minimal/common/clock.hpp"
#include "minimal/client/policy.hpp"

namespace minimal::client {

namespace {

std::string DisplayName(const v1::SessionSummary& session) {
    std::string name = StripControlChars(session.name());
    return name.empty() ? std::string("(unnamed)") : name;
}

std::string Label(const v1::SessionSummary& session, std::size_t name_width,
                  std::int64_t now_unix_ms) {
    // S5.4: "live" marks a session with a Session object still running
    // server-side (attached or detached). Selecting the row ATTACHES to the
    // running instance (PickerController::OpenPickedSession); resume from the
    // stored transcript is for the non-live rows, and a resume sent for a
    // live session is refused server-side ("already open") -- the two verbs
    // are not interchangeable. A leading dot, so it scans down the column.
    std::string name = DisplayName(session);
    if (Utf8ColumnWidth(name) > name_width) name = TruncateUtf8Columns(name, name_width);
    const std::size_t pad = name_width - std::min(name_width, Utf8ColumnWidth(name));
    std::string count = std::to_string(session.message_count()) + " msgs";
    if (count.size() < kMessageCountColumnWidth) {
        count.insert(0, kMessageCountColumnWidth - count.size(), ' ');
    }
    return std::string(LiveMarker(session.is_live())) + name + std::string(pad, ' ') + "  " +
           count + "  " + RelativeAge(session.updated_at_unix_ms(), now_unix_ms);
}

}  // namespace

std::string RelativeAge(std::int64_t unix_ms, std::int64_t now_unix_ms) {
    if (unix_ms <= 0) return "";
    using std::chrono::duration_cast;
    const std::chrono::milliseconds age(now_unix_ms - unix_ms);
    // Under a minute includes a clock slightly behind the server's.
    if (age < std::chrono::minutes(1)) return "just now";
    if (age < std::chrono::hours(1)) {
        return std::to_string(duration_cast<std::chrono::minutes>(age).count()) + "m ago";
    }
    if (age < std::chrono::days(1)) {
        return std::to_string(duration_cast<std::chrono::hours>(age).count()) + "h ago";
    }
    if (age < std::chrono::weeks(1)) {
        return std::to_string(duration_cast<std::chrono::days>(age).count()) + "d ago";
    }
    const std::time_t stamp = common::UnixMillisToTimeT(unix_ms);
    std::tm local{};
    localtime_r(&stamp, &local);
    char buffer[sizeof("YYYY-MM-DD")];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &local);
    return buffer;
}

std::vector<PickerRow> BuildPickerRows(const std::vector<v1::SessionSummary>& sessions,
                                       const std::string& filter, PickerSort sort,
                                       std::int64_t now_unix_ms) {
    now_unix_ms = NowUnixMsOr(now_unix_ms);
    const std::string lowered_filter = ToLower(filter);

    std::vector<const v1::SessionSummary*> matched;
    matched.reserve(sessions.size());
    for (const auto& session : sessions) {
        if (MatchesFilter(lowered_filter,
                          {session.name(), session.session_id(), session.project_root()})) {
            matched.push_back(&session);
        }
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
    // The name column is as wide as the widest name SHOWN (so a filter
    // narrows it too), capped.
    std::size_t name_width = 0;
    for (const auto* session : matched) {
        name_width = std::max(name_width, Utf8ColumnWidth(DisplayName(*session)));
    }
    name_width = std::min(name_width, kNameColumnCap);
    for (const auto* session : matched) {
        rows.push_back(PickerRow{Label(*session, name_width, now_unix_ms), session->session_id()});
    }
    return rows;
}

}  // namespace minimal::client
