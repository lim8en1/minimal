#include "fs_settings.hpp"

namespace minimal::tools_fs {
namespace {

template <typename T>
T ValueOr(const nlohmann::json& parent, const char* section, const char* key, T fallback) {
    auto section_it = parent.find(section);
    if (section_it == parent.end() || !section_it->is_object()) return fallback;
    auto key_it = section_it->find(key);
    if (key_it == section_it->end() || !key_it->is_number()) return fallback;
    return key_it->get<T>();
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
    return settings;
}

}  // namespace minimal::tools_fs
