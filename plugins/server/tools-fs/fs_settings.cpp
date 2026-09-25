#include "fs_settings.hpp"

#include <algorithm>
#include <limits>

#include "minimal/config/parse_util.hpp"

namespace minimal::tools_fs {
namespace {

// config::ReadClampedInt64 rejects a non-integer number (`2000.7` for a line
// limit would be silently truncated by get<T>()) and clamps into T's range,
// so a huge configured value narrows safely instead of wrapping -- the same
// reader tools-search uses. The floor of 0 is harmless: every field below is
// a positive count, and the `<= 0` fallbacks in ParseFsSettings treat 0 the
// same as a negative.
template <typename T>
T ValueOr(const nlohmann::json& parent, const char* section, const char* key, T fallback) {
    auto section_it = parent.find(section);
    if (section_it == parent.end() || !section_it->is_object()) return fallback;
    return static_cast<T>(config::ReadClampedInt64(
        *section_it, key, static_cast<std::int64_t>(fallback), 0,
        static_cast<std::int64_t>(std::numeric_limits<T>::max())));
}

}  // namespace

FsSettings ParseFsSettings(const nlohmann::json& config) {
    FsSettings settings;
    if (!config.is_object()) return settings;

    settings.read_default_limit_lines =
        ValueOr(config, "read", "default_limit_lines", settings.read_default_limit_lines);
    settings.read_max_line_length =
        ValueOr(config, "read", "max_line_length", settings.read_max_line_length);
    settings.read_max_lines =
        ValueOr(config, "read", "max_lines", settings.read_max_lines);
    settings.edit_max_editable_file_bytes = ValueOr(
        config, "edit", "max_editable_file_bytes", settings.edit_max_editable_file_bytes);

    // A non-positive limit would mean "read nothing", which is never what a user
    // meant to configure; fall back rather than silently returning empty windows.
    if (settings.read_default_limit_lines <= 0) settings.read_default_limit_lines = 2000;
    if (settings.read_max_line_length <= 0) settings.read_max_line_length = 2000;
    if (settings.read_max_lines <= 0) settings.read_max_lines = 2000;
    if (settings.edit_max_editable_file_bytes <= 0) {
        settings.edit_max_editable_file_bytes = 5 * 1024 * 1024;
    }
    // The default window lives under the ceiling: a default above it would
    // be clipped on every call, which is the configuration contradicting
    // itself, not something to report to the model each time.
    settings.read_default_limit_lines =
        std::min(settings.read_default_limit_lines, settings.read_max_lines);
    return settings;
}

}  // namespace minimal::tools_fs
