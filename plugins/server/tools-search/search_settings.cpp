#include "search_settings.hpp"

#include <limits>

#include "minimal/config/parse_util.hpp"

namespace minimal::tools_search {
namespace {

template <typename T>
T ValueOr(const nlohmann::json& parent, const char* section, const char* key, T fallback) {
    auto section_it = parent.find(section);
    if (section_it == parent.end() || !section_it->is_object()) return fallback;
    // config::ReadClampedInt64 rejects a non-integer number (`is_number()`
    // accepted 250.9 and get<T>() silently truncated it) and clamps into T's
    // own range, so a huge configured value narrows safely instead of
    // wrapping. The floor of 0 is harmless here: every field below is a
    // non-negative count/timeout, and PositiveOr's own `value > 0` check
    // already rejected a negative the same way a not-positive one is.
    const auto value =
        config::ReadClampedInt64(*section_it, key, static_cast<std::int64_t>(fallback), 0,
                                 static_cast<std::int64_t>(std::numeric_limits<T>::max()));
    return static_cast<T>(value);
}

template <typename T>
T PositiveOr(const nlohmann::json& parent, const char* section, const char* key, T fallback) {
    const auto value = ValueOr(parent, section, key, fallback);
    return value > 0 ? value : fallback;
}

}  // namespace

SearchSettings ParseSearchSettings(const nlohmann::json& config) {
    SearchSettings settings;
    if (!config.is_object()) return settings;

    settings.grep_default_head_limit =
        PositiveOr(config, "grep", "default_head_limit", settings.grep_default_head_limit);
    settings.grep_max_results =
        PositiveOr(config, "grep", "max_results", settings.grep_max_results);
    settings.grep_max_file_bytes =
        PositiveOr(config, "grep", "max_file_bytes", settings.grep_max_file_bytes);
    settings.grep_timeout_ms =
        PositiveOr(config, "grep", "timeout_ms", settings.grep_timeout_ms);
    settings.grep_max_context_lines =
        PositiveOr(config, "grep", "max_context_lines", settings.grep_max_context_lines);
    settings.grep_max_line_length =
        PositiveOr(config, "grep", "max_line_length", settings.grep_max_line_length);

    settings.glob_default_limit =
        PositiveOr(config, "glob", "default_limit", settings.glob_default_limit);
    settings.glob_max_results =
        PositiveOr(config, "glob", "max_results", settings.glob_max_results);
    settings.glob_timeout_ms =
        PositiveOr(config, "glob", "timeout_ms", settings.glob_timeout_ms);

    return settings;
}

}  // namespace minimal::tools_search
