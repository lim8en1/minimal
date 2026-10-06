#include "search_settings.hpp"

#include <limits>

#include "minimal/common/config_reader.hpp"

namespace minimal::tools_search {

SearchSettings ParseSearchSettings(const nlohmann::json& config) {
    SearchSettings settings;
    constexpr std::int64_t kIntMax = std::numeric_limits<int>::max();
    config::Reader root(config, "plugins.json", "tools-search.config");
    const auto positive_int = [&](config::Reader& section, const char* key, int& target) {
        target = static_cast<int>(section.Int(key, target, 1, kIntMax));
    };

    config::Reader grep = root.Object("grep");
    positive_int(grep, "default_head_limit", settings.grep_default_head_limit);
    positive_int(grep, "max_results", settings.grep_max_results);
    settings.grep_max_file_bytes = grep.Int("max_file_bytes", settings.grep_max_file_bytes, 1);
    positive_int(grep, "timeout_ms", settings.grep_timeout_ms);
    positive_int(grep, "max_context_lines", settings.grep_max_context_lines);
    positive_int(grep, "max_line_length", settings.grep_max_line_length);
    grep.Finish();

    config::Reader glob = root.Object("glob");
    positive_int(glob, "default_limit", settings.glob_default_limit);
    positive_int(glob, "max_results", settings.glob_max_results);
    positive_int(glob, "timeout_ms", settings.glob_timeout_ms);
    glob.Finish();
    root.Finish();
    return settings;
}

}  // namespace minimal::tools_search
