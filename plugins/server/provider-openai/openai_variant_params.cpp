#include "openai_variant_params.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace minimal::provider_openai {

namespace {

using nlohmann::json;

// A flag of the /model/info row: nullopt when absent or not a boolean.
std::optional<bool> Flag(const json& info, const std::string& name) {
    const auto it = info.find(name);
    if (it == info.end() || !it->is_boolean()) return std::nullopt;
    return it->get<bool>();
}

}  // namespace

std::optional<std::vector<std::string>> OpenAiVariantParams::AdvertisedEfforts(
    const json& metadata) const {
    if (!metadata.is_object() || !metadata.contains("model_info") ||
        !metadata.at("model_info").is_object()) {
        return std::nullopt;  // not ours: core's catalog shapes
    }
    const json& info = metadata.at("model_info");
    std::vector<std::string> out;
    if (Flag(info, "supports_reasoning") != true) return out;
    // In ladder order; the bool is whether the proxy maps the rung for any
    // reasoning model (the standard three) or only where a flag says so.
    static const std::pair<const char*, bool> kLadder[] = {
        {"none", false}, {"minimal", false}, {"low", true},  {"medium", true},
        {"high", true},  {"xhigh", false},   {"max", false},
    };
    for (const auto& [word, standard] : kLadder) {
        if (Flag(info, std::string("supports_") + word + "_reasoning_effort").value_or(standard)) {
            out.push_back(word);
        }
    }
    return out;
}

std::optional<std::pair<json, bool>> OpenAiVariantParams::DeriveParameters(
    const std::string& effort_word, std::optional<int> max_tokens, const json& metadata) const {
    (void)max_tokens;
    const auto advertised = AdvertisedEfforts(metadata);
    if (!advertised ||
        std::find(advertised->begin(), advertised->end(), effort_word) == advertised->end()) {
        return std::nullopt;  // core's generic reasoning_effort mapping
    }
    return std::pair{json{{"reasoning_effort", effort_word}}, false};
}

}  // namespace minimal::provider_openai
