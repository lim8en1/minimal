#include "line_diff.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>

namespace minimal::tools_fs {
namespace {

constexpr int kContextLines = 3;
// Myers keeps one V slice per step, ~D^2 ints in total: 1000 bounds that at
// ~4 MB, and a change this large is not reviewable as a diff anyway.
constexpr int kMaxEditDistance = 1000;
constexpr std::size_t kMaxHunkLines = 2000;

enum class Op : char { kEqual = ' ', kDelete = '-', kInsert = '+' };

struct Edit {
    Op op;
    std::string_view text;
};

// A line plus whether it carried a '\n'. Only a file's last line can lack
// one, and comparing the flag too is what makes a change to the final
// newline alone -- invisible in the text -- still a changed line in the
// diff, instead of an empty hunk list for an edit that did change bytes.
struct Line {
    std::string_view text;
    bool terminated = true;
    bool operator==(const Line& other) const {
        return text == other.text && terminated == other.terminated;
    }
    bool operator!=(const Line& other) const { return !(*this == other); }
};

using Lines = std::vector<Line>;

Lines ToLines(std::string_view text) {
    Lines lines;
    for (const auto line : SplitLines(text)) lines.push_back({line, true});
    if (!lines.empty() && text.back() != '\n') lines.back().terminated = false;
    return lines;
}

bool LooksBinary(std::string_view text) { return text.find('\0') != std::string_view::npos; }

// trace[d] is V before step d, sliced to k in [-(d+1), d+1].
std::vector<Edit> Backtrack(const std::vector<std::vector<int>>& trace, const Lines& a,
                            const Lines& b) {
    std::vector<Edit> reversed;
    int x = static_cast<int>(a.size());
    int y = static_cast<int>(b.size());
    for (int d = static_cast<int>(trace.size()) - 1; d >= 0; --d) {
        const auto& v = trace[d];
        const auto at = [&v, d](int k) { return v[k + d + 1]; };
        const int k = x - y;
        const int prev_k = (k == -d || (k != d && at(k - 1) < at(k + 1))) ? k + 1 : k - 1;
        const int prev_x = at(prev_k);
        const int prev_y = prev_x - prev_k;
        while (x > prev_x && y > prev_y) {
            reversed.push_back({Op::kEqual, a[x - 1].text});
            --x;
            --y;
        }
        if (d > 0) {
            if (x == prev_x) {
                reversed.push_back({Op::kInsert, b[y - 1].text});
            } else {
                reversed.push_back({Op::kDelete, a[x - 1].text});
            }
        }
        x = prev_x;
        y = prev_y;
    }
    return {reversed.rbegin(), reversed.rend()};
}

// Myers' O(ND) shortest edit script; nullopt past kMaxEditDistance.
std::optional<std::vector<Edit>> Myers(const Lines& a, const Lines& b) {
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());
    const int max_d = std::min(n + m, kMaxEditDistance);
    const int offset = max_d + 1;
    std::vector<int> v(static_cast<std::size_t>(2 * max_d + 3), 0);
    std::vector<std::vector<int>> trace;
    for (int d = 0; d <= max_d; ++d) {
        trace.emplace_back(v.begin() + (offset - d - 1), v.begin() + (offset + d + 2));
        for (int k = -d; k <= d; k += 2) {
            int x = (k == -d || (k != d && v[offset + k - 1] < v[offset + k + 1]))
                        ? v[offset + k + 1]
                        : v[offset + k - 1] + 1;
            int y = x - k;
            while (x < n && y < m && a[x] == b[y]) {
                ++x;
                ++y;
            }
            v[offset + k] = x;
            if (x >= n && y >= m) return Backtrack(trace, a, b);
        }
    }
    return std::nullopt;
}

std::optional<std::vector<Edit>> Diff(const Lines& a, const Lines& b) {
    // Trimming the common prefix/suffix first keeps D (and Myers' cost) to
    // the size of the actual change, which for an edit is a few lines.
    std::size_t prefix = 0;
    while (prefix < a.size() && prefix < b.size() && a[prefix] == b[prefix]) ++prefix;
    std::size_t suffix = 0;
    while (suffix < a.size() - prefix && suffix < b.size() - prefix &&
           a[a.size() - 1 - suffix] == b[b.size() - 1 - suffix]) {
        ++suffix;
    }
    const Lines a_mid(a.begin() + prefix, a.end() - suffix);
    const Lines b_mid(b.begin() + prefix, b.end() - suffix);

    std::vector<Edit> edits;
    for (std::size_t i = 0; i < prefix; ++i) edits.push_back({Op::kEqual, a[i].text});
    if (a_mid.empty() || b_mid.empty()) {
        // Pure insertion or deletion (a created file included): no search needed.
        for (const auto& line : a_mid) edits.push_back({Op::kDelete, line.text});
        for (const auto& line : b_mid) edits.push_back({Op::kInsert, line.text});
    } else {
        auto middle = Myers(a_mid, b_mid);
        if (!middle) return std::nullopt;
        edits.insert(edits.end(), middle->begin(), middle->end());
    }
    for (std::size_t i = a.size() - suffix; i < a.size(); ++i) {
        edits.push_back({Op::kEqual, a[i].text});
    }
    return edits;
}

nlohmann::json Hunks(const std::vector<Edit>& edits) {
    const int total = static_cast<int>(edits.size());
    std::vector<int> old_before(static_cast<std::size_t>(total) + 1, 0);
    std::vector<int> new_before(static_cast<std::size_t>(total) + 1, 0);
    for (int i = 0; i < total; ++i) {
        old_before[i + 1] = old_before[i] + (edits[i].op != Op::kInsert ? 1 : 0);
        new_before[i + 1] = new_before[i] + (edits[i].op != Op::kDelete ? 1 : 0);
    }

    nlohmann::json hunks = nlohmann::json::array();
    std::size_t emitted = 0;
    int idx = 0;
    while (idx < total) {
        while (idx < total && edits[idx].op == Op::kEqual) ++idx;
        if (idx >= total) break;
        int last_change = idx;
        for (int j = idx; j < total; ++j) {
            if (edits[j].op != Op::kEqual) {
                last_change = j;
            } else if (j - last_change > 2 * kContextLines) {
                break;   // the next change's context no longer touches this one's
            }
        }
        const int start = std::max(0, idx - kContextLines);
        const int end = std::min(total, last_change + kContextLines + 1);

        const int old_count = old_before[end] - old_before[start];
        const int new_count = new_before[end] - new_before[start];
        // Capped BEFORE this hunk's lines are materialized: past the cap the
        // whole payload is dropped anyway, so building it first was waste.
        emitted += static_cast<std::size_t>(end - start);
        if (emitted > kMaxHunkLines) return nullptr;
        nlohmann::json lines = nlohmann::json::array();
        for (int i = start; i < end; ++i) {
            lines.push_back({{"op", std::string(1, static_cast<char>(edits[i].op))},
                             {"text", std::string(edits[i].text)}});
        }
        // Unified-diff convention: an empty side's start is the line BEFORE it.
        hunks.push_back({{"old_start", old_before[start] + (old_count > 0 ? 1 : 0)},
                         {"old_count", old_count},
                         {"new_start", new_before[start] + (new_count > 0 ? 1 : 0)},
                         {"new_count", new_count},
                         {"lines", std::move(lines)}});
        idx = end;
    }
    return hunks;
}

}  // namespace

std::vector<std::string_view> SplitLines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t newline = text.find('\n', pos);
        if (newline == std::string_view::npos) {
            lines.push_back(text.substr(pos));
            break;
        }
        lines.push_back(text.substr(pos, newline - pos));
        pos = newline + 1;
    }
    return lines;
}

nlohmann::json DiffDisplay(std::string_view path, std::string_view old_text,
                           std::string_view new_text, bool created) {
    if (LooksBinary(old_text) || LooksBinary(new_text)) return nullptr;
    const auto edits = Diff(ToLines(old_text), ToLines(new_text));
    if (!edits) return nullptr;
    auto hunks = Hunks(*edits);
    if (hunks.is_null()) return nullptr;
    return {{"kind", "diff"},
            {"path", std::string(path)},
            {"created", created},
            {"hunks", std::move(hunks)}};
}

}  // namespace minimal::tools_fs
