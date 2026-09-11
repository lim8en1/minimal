#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <ftxui/dom/elements.hpp>
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
    // The depth the roles were downsampled to, for colors that arrive from
    // outside the theme (agent preset colors) and must be degraded the same way.
    ColorDepth depth() const { return depth_; }

private:
    std::string name_;
    ColorDepth depth_ = ColorDepth::Truecolor;
    // Transparent hash + equality: ColorFor(string_view) looks up WITHOUT
    // constructing a std::string key per call (called per frame by the bars
    // and per highlight span stream).
    struct TransparentHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view v) const {
            return std::hash<std::string_view>{}(v);
        }
    };
    struct TransparentEqual {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const { return a == b; }
    };
    std::unordered_map<std::string, ftxui::Color, TransparentHash, TransparentEqual> colors_;
};

// The focused-row style for pickers and option lists: row_focused on
// status_bar_bg, so the cursor follows the theme instead of swapping the
// terminal's own fg/bg. Falls back to `inverted` when either role is unset (a
// partial theme, or ColorDepth::None) -- a cursor must never vanish.
ftxui::Decorator SelectionStyle(const Theme& theme);

// Secondary text -- hints, footers, gutters, collapse affordances: the `muted`
// role, falling back to system_message. This replaces SGR `dim`, which the
// terminal renders however it likes (unreadable in practice). With neither
// role set (NO_COLOR) it is a no-op: plain text, never dim.
ftxui::Decorator Muted(const Theme& theme);

// Paints `bg` into every cell the element leaves at the terminal-default
// background, AFTER the element renders -- so it also reaches cells a Modal's
// clear_under reset. Default `bg` is a no-op: transparent terminals keep
// showing through (the mid-tone themes rely on that).
ftxui::Decorator FillBackground(ftxui::Color bg);

}  // namespace minimal::client
