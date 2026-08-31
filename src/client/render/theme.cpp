#include "minimal/client/render/theme.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include <nlohmann/json.hpp>

#include "minimal/common/logger.hpp"

namespace minimal::client {

namespace {

// "#rrggbb" or "#rgb". Anything else is a malformed theme, and a malformed
// theme is a load error naming the key -- silently defaulting one role would
// leave the user hunting for which of their colors is off.
std::tuple<uint8_t, uint8_t, uint8_t> ParseHexColor(std::string_view value,
                                                    std::string_view role) {
    const auto bad = [&]() -> std::runtime_error {
        return std::runtime_error("theme role '" + std::string(role) +
                                  "': color '" + std::string(value) +
                                  "' is not #rrggbb or #rgb");
    };
    if (value.size() != 7 && value.size() != 4) throw bad();
    if (value[0] != '#') throw bad();

    const auto digit = [&](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw bad();
    };

    if (value.size() == 4) {
        const int r = digit(value[1]), g = digit(value[2]), b = digit(value[3]);
        return {static_cast<uint8_t>(r * 17), static_cast<uint8_t>(g * 17),
                static_cast<uint8_t>(b * 17)};
    }
    const int r = digit(value[1]) * 16 + digit(value[2]);
    const int g = digit(value[3]) * 16 + digit(value[4]);
    const int b = digit(value[5]) * 16 + digit(value[6]);
    return {static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
}

}  // namespace

Theme Theme::Parse(std::string_view json, ColorDepth depth,
                   const std::vector<std::string>* known_plugins) {
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(std::string("theme: malformed JSON: ") + e.what());
    }

    Theme theme;
    theme.name_ = doc.value("name", std::string{});
    if (!doc.contains("colors") || !doc.at("colors").is_object()) {
        throw std::runtime_error("theme '" + theme.name_ + "': no \"colors\" object");
    }
    for (const auto& [role, value] : doc.at("colors").items()) {
        if (!value.is_string()) {
            throw std::runtime_error("theme role '" + role + "': color must be a string");
        }
        const auto [r, g, b] = ParseHexColor(value.get_ref<const std::string&>(), role);
        theme.colors_.emplace(role, Downsample(r, g, b, depth));
    }

    // Plugin sections (docs/EXTENSIBILITY.md 4.2): optional namespaced roles,
    // addressable only as `section.role` and merged AFTER the core roles, so a
    // plugin can never shadow one -- emplace keeps the first (core) entry, and
    // a dotted key colliding with an earlier section's is likewise first-wins.
    // Duplicate section NAMES cannot be detected here: a JSON object keeps one
    // entry per key, so a repeated name in one file collapses before we see it.
    if (auto sections = doc.find("sections");
        sections != doc.end() && sections->is_object()) {
        for (const auto& [section, roles] : sections->items()) {
            if (known_plugins != nullptr &&
                std::find(known_plugins->begin(), known_plugins->end(), section) ==
                    known_plugins->end()) {
                // The naming_model rule: a section naming no loaded plugin is
                // reported, not fatal -- it may be this theme predates the
                // plugin's removal, and colors are decoration.
                common::DefaultLogger().warn(
                    "theme '{}': section '{}' names no loaded client plugin; its roles are "
                    "unreachable",
                    theme.name_, section);
            }
            if (!roles.is_object()) continue;
            for (const auto& [role, value] : roles.items()) {
                if (!value.is_string()) continue;  // tolerate: plugin sections are not core config
                const std::string full = section + "." + role;
                if (theme.colors_.contains(role) || theme.colors_.contains(full)) continue;
                const auto [r, g, b] = ParseHexColor(value.get_ref<const std::string&>(), full);
                theme.colors_.emplace(full, Downsample(r, g, b, depth));
            }
        }
    }
    return theme;
}

Theme Theme::Load(const std::filesystem::path& themes_dir, const std::string& name,
                  ColorDepth depth, const std::vector<std::string>* known_plugins) {
    const std::filesystem::path file = themes_dir / (name + ".json");
    std::ifstream in(file);
    if (!in) {
        throw std::runtime_error("theme: cannot read " + file.string());
    }
    std::ostringstream contents;
    contents << in.rdbuf();
    try {
        return Parse(contents.str(), depth, known_plugins);
    } catch (const std::runtime_error& e) {
        throw std::runtime_error(file.string() + ": " + e.what());
    }
}

ftxui::Color Theme::ColorFor(std::string_view role) const {
    const auto it = colors_.find(std::string(role));
    return it != colors_.end() ? it->second : ftxui::Color::Default;
}

}  // namespace minimal::client
