#include "minimal/client/render/theme.hpp"

#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <tuple>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <nlohmann/json.hpp>

#include "minimal/common/logger.hpp"
#include "minimal/common/read_file.hpp"
#include "minimal/common/units.hpp"

namespace minimal::client {

std::optional<std::tuple<uint8_t, uint8_t, uint8_t>> TryParseHexColor(std::string_view value) {
    if ((value.size() != 7 && value.size() != 4) || value[0] != '#') return std::nullopt;
    // Each digit is validated on its own: combining first could turn a valid
    // high nibble and an invalid low one into a plausible small value.
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int d[6] = {};
    const std::size_t n = value.size() - 1;
    for (std::size_t i = 0; i < n; ++i) {
        d[i] = digit(value[i + 1]);
        if (d[i] < 0) return std::nullopt;
    }
    if (n == 3) {
        return std::make_tuple(static_cast<uint8_t>(d[0] * 17), static_cast<uint8_t>(d[1] * 17),
                               static_cast<uint8_t>(d[2] * 17));
    }
    return std::make_tuple(static_cast<uint8_t>(d[0] * 16 + d[1]),
                           static_cast<uint8_t>(d[2] * 16 + d[3]),
                           static_cast<uint8_t>(d[4] * 16 + d[5]));
}

namespace {

// "#rrggbb" or "#rgb". Anything else is a malformed theme, and a malformed
// theme is a load error naming the key -- silently defaulting one role would
// leave the user hunting for which of their colors is off.
std::tuple<uint8_t, uint8_t, uint8_t> ParseHexColor(std::string_view value,
                                                    std::string_view role) {
    if (const auto rgb = TryParseHexColor(value)) return *rgb;
    throw std::runtime_error("theme role '" + std::string(role) + "': color '" +
                             std::string(value) + "' is not #rrggbb or #rgb");
}

// FillBackground's node: lays out exactly like its child, then back-fills the
// cells still at the default background once the child has drawn.
class BackgroundFill : public ftxui::Node {
public:
    BackgroundFill(ftxui::Element child, ftxui::Color bg)
        : Node(ftxui::Elements{std::move(child)}), bg_(bg) {}

    void ComputeRequirement() override {
        Node::ComputeRequirement();
        requirement_ = children_[0]->requirement();
    }

    void SetBox(ftxui::Box box) override {
        Node::SetBox(box);
        children_[0]->SetBox(box);
    }

    void Render(ftxui::Screen& screen) override {
        Node::Render(screen);
        const int x_max = std::min(box_.x_max, screen.dimx() - 1);
        const int y_max = std::min(box_.y_max, screen.dimy() - 1);
        for (int y = std::max(box_.y_min, 0); y <= y_max; ++y) {
            for (int x = std::max(box_.x_min, 0); x <= x_max; ++x) {
                auto& cell = screen.CellAt(x, y);
                if (cell.background_color == ftxui::Color::Default) cell.background_color = bg_;
            }
        }
    }

private:
    ftxui::Color bg_;
};

}  // namespace

ftxui::Decorator SelectionStyle(const Theme& theme) {
    // `cursor`, not `attention`-valued row_focused: a cursor that looks like
    // "act on this" misreads every picker row it lands on.
    const ftxui::Color fg = theme.ColorForOr("cursor", "row_focused");
    const ftxui::Color bg = theme.ColorFor("status_bar_bg");
    if (fg == ftxui::Color::Default || bg == ftxui::Color::Default) return ftxui::inverted;
    return ftxui::color(fg) | ftxui::bgcolor(bg);
}

ftxui::Color MutedColor(const Theme& theme) {
    return theme.ColorForOr("muted", "system_message");
}

ftxui::Decorator Muted(const Theme& theme) {
    const ftxui::Color c = MutedColor(theme);
    // Not color(Default): that would reset an enclosing tint (a modal's
    // attention color) back to the terminal's foreground.
    if (c == ftxui::Color::Default) return [](ftxui::Element e) { return e; };
    return ftxui::color(c);
}

ftxui::Decorator FillBackground(ftxui::Color bg) {
    return [bg](ftxui::Element child) -> ftxui::Element {
        if (bg == ftxui::Color::Default) return child;
        return std::make_shared<BackgroundFill>(std::move(child), bg);
    };
}

Theme Theme::Parse(std::string_view json, ColorDepth depth,
                   const std::vector<std::string>* known_plugins) {
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(std::string("theme: malformed JSON: ") + e.what());
    }

    Theme theme;
    // Fresh generation per built theme (see Theme::epoch).
    static std::atomic<uint64_t> next_epoch{1};
    theme.epoch_ = next_epoch.fetch_add(1, std::memory_order_relaxed);
    theme.depth_ = depth;
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
                // Only a FULL-key collision skips: the bare-role half of the
                // old test dropped a plugin role whose NAME matched a core
                // role -- even though it is stored under section.role and
                // could never shadow the core entry it "collided" with.
                if (theme.colors_.contains(full)) continue;
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
    const common::ReadFileResult read = common::ReadFile(file, common::kMiB);
    if (!read.ok()) throw std::runtime_error("theme: " + read.error);
    try {
        return Parse(read.contents, depth, known_plugins);
    } catch (const std::runtime_error& e) {
        throw std::runtime_error(file.string() + ": " + e.what());
    }
}

ftxui::Color Theme::ColorFor(std::string_view role) const {
    const auto it = colors_.find(role);
    return it != colors_.end() ? it->second : ftxui::Color::Default;
}

ftxui::Color Theme::ColorForOr(std::string_view role, std::string_view fallback) const {
    const auto it = colors_.find(role);
    return it != colors_.end() ? it->second : ColorFor(fallback);
}

}  // namespace minimal::client
