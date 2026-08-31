#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <ftxui/screen/color.hpp>

#include "minimal/client/render/color_downsample.hpp"

namespace minimal::client {

// A loaded theme: role/category name -> color, downsampled at load to what the
// terminal can display. Roles are open-ended -- the syntax highlighter and the
// views name them, and a theme may carry roles nothing in this build reads yet
// (kept, not rejected: a theme shared with a newer build must stay loadable).
class Theme {
public:
    // Loads <themes_dir>/<name>.json. Throws std::runtime_error naming the file
    // and the offending key on unreadable or malformed content.
    static Theme Load(const std::filesystem::path& themes_dir, const std::string& name,
                      ColorDepth depth, const std::vector<std::string>* known_plugins = nullptr);

    // Parses a theme document directly. The test seam; Load() is this over a file.
    // `known_plugins`, when given, feeds the naming_model rule for plugin
    // sections: a `sections` entry naming no loaded plugin is WARNED about
    // (its roles are unreachable), while section roles themselves merge AFTER
    // the core roles -- a plugin section can never shadow a core role.
    static Theme Parse(std::string_view json, ColorDepth depth,
                       const std::vector<std::string>* known_plugins = nullptr);

    // The color for a role, or Color::Default when the theme does not set it.
    // Missing roles fall back rather than failing: a partially-written theme is
    // usable, and every call site would otherwise duplicate the fallback.
    ftxui::Color ColorFor(std::string_view role) const;

    const std::string& name() const { return name_; }
    // Number of roles the theme defines. Visible for tests.
    std::size_t size() const { return colors_.size(); }

private:
    std::string name_;
    std::unordered_map<std::string, ftxui::Color> colors_;
};

}  // namespace minimal::client
