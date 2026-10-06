#include "fs_settings.hpp"

#include <algorithm>
#include <limits>

#include "minimal/common/config_reader.hpp"
#include "minimal/common/plugin_config_label.hpp"

namespace minimal::tools_fs {

FsSettings ParseFsSettings(const nlohmann::json& config) {
    FsSettings settings;
    constexpr std::int64_t kIntMax = std::numeric_limits<int>::max();
    config::Reader root(config, common::PluginConfigLabel(common::kPluginConfigDirName, "tools-fs"));
    config::Reader read = root.Object("read");
    settings.read_default_limit_lines = static_cast<decltype(settings.read_default_limit_lines)>(
        read.Int("default_limit_lines", settings.read_default_limit_lines, 1, kIntMax));
    settings.read_max_line_length = static_cast<decltype(settings.read_max_line_length)>(
        read.Int("max_line_length", settings.read_max_line_length, 1, kIntMax));
    settings.read_max_lines = static_cast<decltype(settings.read_max_lines)>(
        read.Int("max_lines", settings.read_max_lines, 1, kIntMax));
    read.Finish();
    config::Reader edit = root.Object("edit");
    settings.edit_max_editable_file_bytes =
        static_cast<decltype(settings.edit_max_editable_file_bytes)>(edit.Int(
            "max_editable_file_bytes", settings.edit_max_editable_file_bytes, 1));
    edit.Finish();
    root.Finish();
    // The default window lives under the ceiling: a default above it would be
    // clipped on every call, which is the configuration contradicting itself.
    if (settings.read_default_limit_lines > settings.read_max_lines) {
        root.Fail("read.default_limit_lines", "must not exceed read.max_lines");
    }
    return settings;
}

}  // namespace minimal::tools_fs
