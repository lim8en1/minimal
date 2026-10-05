#include "host_permission_model.hpp"

#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

#include "minimal/tools/pattern_match.hpp"
#include "url_policy.hpp"

namespace minimal::tools_web {
namespace {

using tools::PermissionMode;

struct Rule {
    PermissionMode mode;
    bool from_preset;
};

struct HostLayer {
    // glob (lowercased) -> rule, operator's and preset's merged: a preset
    // rule replaces the operator's for the same glob.
    std::map<std::string, Rule> rules;
    std::optional<PermissionMode> default_mode;
    // Untrusted preset: the operator's answer alone, to fold the preset's with.
    bool clamp = false;
    std::map<std::string, Rule> global_rules;
    std::optional<PermissionMode> global_default;
    bool default_from_preset = false;
};

PermissionMode ParseMode(const nlohmann::json& value, const std::string& where) {
    if (value.is_string()) {
        if (auto mode = tools::mode_from_name(value.get<std::string>())) return *mode;
    }
    throw std::runtime_error(where + " must be \"allow\", \"ask\" or \"deny\"");
}

std::string Lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

void ParseSlice(const nlohmann::json& slice, const char* where, bool from_preset,
                std::map<std::string, Rule>& rules,
                std::optional<PermissionMode>& default_mode) {
    if (slice.is_null()) return;
    if (!slice.is_object()) throw std::runtime_error(std::string(where) + " must be an object");
    for (const auto& [key, value] : slice.items()) {
        if (key == "default") {
            default_mode = ParseMode(value, std::string(where) + ".default");
        } else if (key == "hosts") {
            if (!value.is_object()) {
                throw std::runtime_error(std::string(where) + ".hosts must be {glob: mode}");
            }
            for (const auto& [glob, mode] : value.items()) {
                if (glob.empty()) throw std::runtime_error(std::string(where) + ": empty host glob");
                rules[Lower(glob)] = Rule{
                    ParseMode(mode, std::string(where) + ".hosts[\"" + glob + "\"]"), from_preset};
            }
        } else if (!key.empty() && key[0] != '_' && key[0] != '$') {
            throw std::runtime_error(std::string(where) + ": unknown key '" + key + "'");
        }
    }
}

// Literal characters: what "more specific" means.
std::size_t Specificity(const std::string& glob) {
    std::size_t literal = 0;
    for (const char c : glob) {
        if (c != '*' && c != '?') ++literal;
    }
    return literal;
}

struct Match {
    Rule rule;
    std::string glob;
};

std::optional<Match> BestMatch(const std::map<std::string, Rule>& rules, const std::string& host) {
    std::optional<Match> best;
    std::size_t best_specificity = 0;
    for (const auto& [glob, rule] : rules) {
        if (!tools::GlobMatch(glob, host, /*slash_terminates=*/false)) continue;
        const auto specificity = Specificity(glob);
        if (!best || specificity > best_specificity ||
            (specificity == best_specificity && rule.mode < best->rule.mode)) {
            best = Match{rule, glob};
            best_specificity = specificity;
        }
    }
    return best;
}

struct Answer {
    PermissionMode mode;
    std::string why;
    bool from_preset = false;  // a preset's own host rule or default decided
};

Answer Decide(const std::map<std::string, Rule>& rules,
              const std::optional<PermissionMode>& default_mode, bool default_from_preset,
              const std::optional<std::string>& host, PermissionMode core_network) {
    if (host) {
        if (const auto match = BestMatch(rules, *host)) {
            return {match->rule.mode,
                    std::string(match->rule.from_preset ? "preset: " : "") + "host " + *host +
                        " matches `" + match->glob +
                        "`=" + std::string(tools::mode_name(match->rule.mode)),
                    match->rule.from_preset};
        }
    }
    if (default_mode) {
        return {*default_mode,
                std::string(default_from_preset ? "preset: " : "") +
                    "tools-web default=" + std::string(tools::mode_name(*default_mode)),
                default_from_preset};
    }
    return {core_network, "network=" + std::string(tools::mode_name(core_network)), false};
}

class HostPermissionModel final : public tools::PermissionModel {
public:
    explicit HostPermissionModel(WebSettings settings) : settings_(std::move(settings)) {}

    unsigned Replaces() const override {
        return static_cast<unsigned>(tools::PermissionStage::kNetwork);
    }

    // The permission key is the tool's original name, or "tools-web.<name>"
    // when another plugin also provides that name (name resolution).
    bool Claims(std::string_view key) const override {
        return key == "web_fetch" || key == "web_search" || key == "tools-web.web_fetch" ||
               key == "tools-web.web_search";
    }

    tools::PermissionLayer BuildLayer(const nlohmann::json& global_slice,
                                      const nlohmann::json& preset_slice,
                                      bool preset_trusted) const override {
        HostLayer layer;
        ParseSlice(global_slice, "plugins.tools-web", /*from_preset=*/false, layer.global_rules,
                   layer.global_default);
        std::map<std::string, Rule> preset_rules;
        std::optional<PermissionMode> preset_default;
        ParseSlice(preset_slice, "plugin_permissions.tools-web", /*from_preset=*/true,
                   preset_rules, preset_default);
        layer.rules = layer.global_rules;
        for (const auto& [glob, rule] : preset_rules) layer.rules[glob] = rule;
        layer.default_mode = preset_default ? preset_default : layer.global_default;
        layer.default_from_preset = preset_default.has_value();
        layer.clamp = !preset_trusted && (!preset_rules.empty() || preset_default);
        return std::make_shared<const HostLayer>(std::move(layer));
    }

    tools::PermissionAnswer Resolve(tools::PermissionStage stage,
                                    const tools::PermissionQuery& query) const override {
        (void)stage;  // kNetwork is the only stage this model replaces
        static const HostLayer kEmpty;
        const auto& layer =
            query.layer != nullptr ? *static_cast<const HostLayer*>(query.layer) : kEmpty;
        const PermissionMode core_network =
            query.core_policy != nullptr ? query.core_policy->network : PermissionMode::Ask;
        const auto host = HostFor(query);

        Answer answer =
            Decide(layer.rules, layer.default_mode, layer.default_from_preset, host, core_network);
        if (layer.clamp) {
            // An untrusted preset only narrows the operator's own answer.
            const Answer global =
                Decide(layer.global_rules, layer.global_default, false, host, core_network);
            if (global.mode < answer.mode) answer = global;
        }
        // A preset that set `network` itself (plan's deny) is a blanket
        // statement the operator's host rules must not widen -- the same fold
        // core applies between a preset category and a global per-tool rule.
        // Only the preset's OWN host rules speak more specifically than it.
        if (!answer.from_preset && query.core_policy != nullptr && query.core_policy->layers) {
            if (const auto preset_network = query.core_policy->layers->preset.network;
                preset_network && *preset_network < answer.mode) {
                answer = {*preset_network,
                          "preset: network=" + std::string(tools::mode_name(*preset_network)),
                          true};
            }
        }
        return {answer.mode, answer.why};
    }

    std::string Describe(const void* layer) const override {
        if (layer == nullptr) return {};
        const auto& l = *static_cast<const HostLayer*>(layer);
        std::string out = "default=" + (l.default_mode ? std::string(tools::mode_name(*l.default_mode))
                                                        : std::string("network"));
        for (const auto& [glob, rule] : l.rules) {
            out += " " + glob + "=" + std::string(tools::mode_name(rule.mode));
        }
        return out;
    }

private:
    std::optional<std::string> HostFor(const tools::PermissionQuery& query) const {
        const std::string_view key = query.permission_key;
        const bool search = key == "web_search" || key == "tools-web.web_search";
        if (search) {
            if (settings_.search_api_url.empty()) return std::nullopt;
            return HostOf(settings_.search_api_url);
        }
        if (!query.input.is_object() || !query.input.contains("url") ||
            !query.input.at("url").is_string()) {
            return std::nullopt;
        }
        return HostOf(query.input.at("url").get<std::string>());
    }

    WebSettings settings_;
};

}  // namespace

std::shared_ptr<const tools::PermissionModel> MakeHostPermissionModel(WebSettings settings) {
    return std::make_shared<const HostPermissionModel>(std::move(settings));
}

}  // namespace minimal::tools_web
