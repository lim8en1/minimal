#include "settings.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace minimal::decision_advisor {
namespace {

constexpr const char* kWhere = "decision-advisor";

// The settings keys, valid at every layer (the bundled top level, a profile,
// the plugins.json entry). "backend" is valid at the top level and in the
// entry, never inside a profile; "backends" only at the bundled top level.
const std::vector<std::string> kSettingKeys = {
    "api_key", "endpoint", "model", "request_timeout_ms", "budget_ms", "retry",
    "min_probability_allow", "min_probability_deny", "min_margin", "questions",
    "extra_questions"};
const std::vector<std::string> kRetryKeys = {
    "max_retries", "backoff_initial_ms", "backoff_max_ms", "jitter", "retry_after_cap_ms"};

bool IsComment(const std::string& key) { return key.rfind("//", 0) == 0; }

std::vector<std::string> KeysPlus(std::vector<std::string> keys,
                                  std::initializer_list<const char*> extra) {
    keys.insert(keys.end(), extra.begin(), extra.end());
    return keys;
}

// Strict schema, the config::RejectUnknownKeys rule over a runtime key set: a
// misspelled field is a named error, never a silent ignore.
void RejectUnknown(const nlohmann::json& object, const std::vector<std::string>& known,
                   const std::string& where) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (IsComment(it.key()) ||
            std::find(known.begin(), known.end(), it.key()) != known.end()) {
            continue;
        }
        std::string list;
        for (const auto& key : known) list += (list.empty() ? "" : ", ") + key;
        throw std::runtime_error(where + " has unknown field \"" + it.key() + "\" (known: " +
                                 list + ")");
    }
}

void CheckLayer(const nlohmann::json& layer, const std::vector<std::string>& known,
                const std::string& where) {
    if (!layer.is_object()) throw std::runtime_error(where + " must be an object");
    RejectUnknown(layer, known, where);
    if (const auto retry = layer.find("retry"); retry != layer.end()) {
        if (!retry->is_object()) throw std::runtime_error(where + ".retry must be an object");
        RejectUnknown(*retry, kRetryKeys, where + ".retry");
    }
}

// Later layer wins; "retry" merges field by field, everything else replaces.
void Overlay(nlohmann::json& merged, const nlohmann::json& layer) {
    for (auto it = layer.begin(); it != layer.end(); ++it) {
        if (IsComment(it.key())) continue;
        if (it.key() == "retry") {
            for (auto field = it->begin(); field != it->end(); ++field) {
                if (!IsComment(field.key())) merged["retry"][field.key()] = field.value();
            }
        } else {
            merged[it.key()] = it.value();
        }
    }
}

const nlohmann::json& Require(const nlohmann::json& parent, const char* key,
                              const std::string& where) {
    const auto it = parent.find(key);
    if (it == parent.end() || it->is_null()) {
        throw std::runtime_error(where + ": \"" + key + "\" is not set (" +
                                 kBundledSettingsFile + " or the plugins.json entry)");
    }
    return *it;
}

std::string RequireString(const nlohmann::json& parent, const char* key, const std::string& where,
                          bool allow_empty) {
    const auto& value = Require(parent, key, where);
    if (!value.is_string()) throw std::runtime_error(where + ": \"" + key + "\" must be a string");
    auto out = value.get<std::string>();
    if (!allow_empty && out.empty()) {
        throw std::runtime_error(where + ": \"" + key + "\" must not be empty");
    }
    return out;
}

int RequireInt(const nlohmann::json& parent, const char* key, const std::string& where, int min) {
    const auto& value = Require(parent, key, where);
    if (!value.is_number_integer()) {
        throw std::runtime_error(where + ": \"" + key + "\" must be an integer");
    }
    const auto raw = value.get<std::int64_t>();
    if (raw < min || raw > std::numeric_limits<int>::max()) {
        throw std::runtime_error(where + ": \"" + key + "\" must be >= " + std::to_string(min));
    }
    return static_cast<int>(raw);
}

double RequireFraction(const nlohmann::json& parent, const char* key, const std::string& where) {
    const auto& value = Require(parent, key, where);
    if (!value.is_number()) throw std::runtime_error(where + ": \"" + key + "\" must be a number");
    const double out = value.get<double>();
    if (out < 0.0 || out > 1.0) {
        throw std::runtime_error(where + ": \"" + key + "\" must be within [0, 1]");
    }
    return out;
}

const nlohmann::json& RequireObject(const nlohmann::json& parent, const char* key,
                                    const std::string& where) {
    const auto& value = Require(parent, key, where);
    if (!value.is_object()) throw std::runtime_error(where + ": \"" + key + "\" must be an object");
    return value;
}

}  // namespace

Settings ParseSettings(const nlohmann::json& bundled, const nlohmann::json& config) {
    const std::string bundled_where = std::string(kWhere) + ": " + kBundledSettingsFile;
    const std::string config_where = std::string(kWhere) + ": config";
    CheckLayer(bundled, KeysPlus(kSettingKeys, {"backend", "backends"}), bundled_where);
    const bool has_config = !config.is_null() && !(config.is_object() && config.empty());
    if (has_config) CheckLayer(config, KeysPlus(kSettingKeys, {"backend"}), config_where);

    Settings settings;
    const bool backend_from_config = has_config && config.contains("backend");
    settings.backend = RequireString(backend_from_config ? config : bundled, "backend",
                                     backend_from_config ? config_where : bundled_where,
                                     /*allow_empty=*/false);

    const auto& backends = RequireObject(bundled, "backends", bundled_where);
    const auto profile = backends.find(settings.backend);
    if (profile == backends.end()) {
        std::string known;
        for (auto it = backends.begin(); it != backends.end(); ++it) {
            if (!IsComment(it.key())) known += (known.empty() ? "" : ", ") + it.key();
        }
        throw std::runtime_error(std::string(kWhere) + ": unknown backend \"" + settings.backend +
                                 "\" (" + kBundledSettingsFile + " defines: " + known + ")");
    }
    CheckLayer(*profile, kSettingKeys, bundled_where + ": backends." + settings.backend);

    nlohmann::json merged = nlohmann::json::object();
    Overlay(merged, bundled);
    Overlay(merged, *profile);
    if (has_config) Overlay(merged, config);
    merged.erase("backend");
    merged.erase("backends");

    // Validated as ONE resolved object: which layer a value came from does
    // not change what it must be.
    const std::string where = std::string(kWhere) + " (backend \"" + settings.backend + "\")";
    settings.api_key = RequireString(merged, "api_key", where, /*allow_empty=*/true);
    settings.endpoint = RequireString(merged, "endpoint", where, /*allow_empty=*/false);
    settings.model = RequireString(merged, "model", where, /*allow_empty=*/false);
    settings.request_timeout_ms = RequireInt(merged, "request_timeout_ms", where, 1);
    settings.budget_ms = RequireInt(merged, "budget_ms", where, 1);
    // A per-attempt cap above the total budget can only mean "budget wins";
    // clamp it so the first attempt cannot silently outlive the loop.
    settings.request_timeout_ms = std::min(settings.request_timeout_ms, settings.budget_ms);

    const auto& retry = RequireObject(merged, "retry", where);
    const std::string retry_where = where + ".retry";
    settings.max_retries = RequireInt(retry, "max_retries", retry_where, 0);
    settings.backoff_initial_ms = RequireInt(retry, "backoff_initial_ms", retry_where, 1);
    settings.backoff_max_ms =
        RequireInt(retry, "backoff_max_ms", retry_where, settings.backoff_initial_ms);
    settings.jitter = RequireFraction(retry, "jitter", retry_where);
    settings.retry_after_cap_ms = RequireInt(retry, "retry_after_cap_ms", retry_where, 1);

    settings.min_probability_allow = RequireFraction(merged, "min_probability_allow", where);
    settings.min_probability_deny = RequireFraction(merged, "min_probability_deny", where);
    settings.min_margin = RequireFraction(merged, "min_margin", where);

    settings.questions = RequireObject(merged, "questions", where);
    if (!settings.questions.contains("action")) {
        throw std::runtime_error(where + ": \"questions\" must define \"action\" -- the answer "
                                         "the advisor acts on");
    }
    settings.extra_questions = RequireObject(merged, "extra_questions", where);
    return settings;
}

nlohmann::json LoadBundledSettings(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        throw std::runtime_error(std::string(kWhere) + ": cannot read " + path.string() +
                                 " (seeded from bundled/ at install; restore it from there)");
    }
    auto parsed = nlohmann::json::parse(stream, nullptr, /*allow_exceptions=*/false,
                                        /*ignore_comments=*/true);
    if (parsed.is_discarded()) {
        throw std::runtime_error(std::string(kWhere) + ": " + path.string() + " is not valid JSON");
    }
    return parsed;
}

}  // namespace minimal::decision_advisor
