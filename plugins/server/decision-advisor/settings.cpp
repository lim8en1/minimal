#include "settings.hpp"

#include <cstdint>
#include <limits>
#include <map>
#include <string>

#include "decision.hpp"
#include "minimal/common/config_reader.hpp"
#include "minimal/common/json_config_loader.hpp"
#include "minimal/common/plugin_config_label.hpp"

namespace minimal::decision_advisor {
namespace {

// The only upper bounds are representational: Settings stores int, and
// max_retries + 1 attempts must still fit. Budget, not a cap here, is what
// bounds a call; the host's advisor_timeout_ms is the outer ceiling.
constexpr std::int64_t kIntMax = std::numeric_limits<int>::max();
// The operator layer's file, for error messages only.
const std::string kPluginsFile =
    common::PluginConfigLabel(common::kPluginConfigDirName, "decision-advisor");

// Reads one layer's settings keys through the strict reader -- type and range
// checked, unknown keys refused by the caller's Finish() -- and returns the
// ones present, normalized. Absent keys stay absent: whether a value is
// missing is only known once every layer is merged.
nlohmann::json ReadLayer(config::Reader& reader) {
    nlohmann::json out = nlohmann::json::object();
    if (auto v = reader.MaybeString("api_key")) out["api_key"] = *v;
    if (auto v = reader.MaybeString("endpoint", /*allow_empty=*/false)) out["endpoint"] = *v;
    if (auto v = reader.MaybeString("model", /*allow_empty=*/false)) out["model"] = *v;
    if (auto v = reader.MaybeString("deny_message", /*allow_empty=*/false)) out["deny_message"] = *v;
    if (auto v = reader.MaybeInt("request_timeout_ms", 1, kIntMax)) out["request_timeout_ms"] = *v;
    if (auto v = reader.MaybeInt("budget_ms", 1, kIntMax)) out["budget_ms"] = *v;
    if (auto retry = reader.MaybeObject("retry")) {
        nlohmann::json r = nlohmann::json::object();
        if (auto v = retry->MaybeInt("max_retries", 0, kIntMax - 1)) r["max_retries"] = *v;
        if (auto v = retry->MaybeInt("backoff_initial_ms", 1, kIntMax)) r["backoff_initial_ms"] = *v;
        if (auto v = retry->MaybeInt("backoff_max_ms", 1, kIntMax)) r["backoff_max_ms"] = *v;
        if (auto v = retry->MaybeDouble("jitter", 0.0, 1.0)) r["jitter"] = *v;
        if (auto v = retry->MaybeInt("retry_after_cap_ms", 1, kIntMax)) r["retry_after_cap_ms"] = *v;
        retry->Finish();
        out["retry"] = std::move(r);
    }
    for (const char* key : {"min_probability_allow", "min_probability_deny", "min_margin"}) {
        if (auto v = reader.MaybeDouble(key, 0.0, 1.0)) out[key] = *v;
    }
    if (auto v = reader.MaybeBool("debug_decision_logging")) out["debug_decision_logging"] = *v;
    // Sent verbatim, so only their shape is the advisor's business.
    for (const char* key : {"questions", "extra_questions"}) {
        if (const auto* v = reader.Raw(key)) {
            if (!v->is_object()) reader.Fail(key, "must be an object");
            out[key] = *v;
        }
    }
    return out;
}

// Later layer wins; "retry" merges field by field, everything else replaces.
void Overlay(nlohmann::json& merged, const nlohmann::json& layer) {
    for (auto it = layer.begin(); it != layer.end(); ++it) {
        if (it.key() == "retry") {
            for (auto field = it->begin(); field != it->end(); ++field) {
                merged["retry"][field.key()] = field.value();
            }
        } else {
            merged[it.key()] = it.value();
        }
    }
}

}  // namespace

Settings ParseSettings(const nlohmann::json& bundled, const nlohmann::json& config) {
    // Layer 1: the bundled file's top level, and every profile in it -- all
    // validated, so a typo in a profile not selected today is still caught.
    config::Reader file(bundled, kBundledSettingsFile);
    const std::string file_backend = file.String("backend", {}, /*allow_empty=*/false);
    const nlohmann::json top = ReadLayer(file);
    std::map<std::string, nlohmann::json> profiles;
    std::string known;
    for (const auto& [name, node] : file.Entries("backends")) {
        config::Reader profile = file.Child(*node, "backends." + name);
        profiles[name] = ReadLayer(profile);
        profile.Finish();
        known += (known.empty() ? "\"" : ", \"") + name + "\"";
    }
    file.Finish();

    // Layer 3: the operator's plugins.d file.
    const nlohmann::json empty = nlohmann::json::object();
    config::Reader entry(config.is_null() ? empty : config, kPluginsFile);
    const auto entry_backend = entry.MaybeString("backend", /*allow_empty=*/false);
    const nlohmann::json overrides = ReadLayer(entry);
    entry.Finish();

    Settings settings;
    settings.backend = entry_backend.value_or(file_backend);
    if (settings.backend.empty()) {
        file.Fail("backend", "is required: the profile used when " + kPluginsFile + " names none");
    }
    const auto profile = profiles.find(settings.backend);
    if (profile == profiles.end()) {
        const std::string message = "\"" + settings.backend + "\" is not a profile in " +
                                    kBundledSettingsFile + " (backends: " + known + ")";
        if (entry_backend) entry.Fail("backend", message);
        file.Fail("backend", message);
    }

    // Layer 2 between them, then one resolved object, read once more: a value
    // no layer sets is an error -- the code has nothing to fall back to.
    nlohmann::json merged = nlohmann::json::object();
    Overlay(merged, top);
    Overlay(merged, profile->second);
    Overlay(merged, overrides);
    if (!merged.contains("retry")) merged["retry"] = nlohmann::json::object();

    // Describe mode only (no-ops otherwise): the operator file's defaults are
    // what the bundled layers resolve to for the selected backend.
    entry.Default("backend", settings.backend);
    entry.Describe("backend", {"Profile under backends in decision-advisor.json."});
    entry.Describe("api_key", {"Key reference for the selected backend.",
                               config::ApplyPolicy::kRestart, /*secret=*/true});
    for (const auto& [key, value] : merged.items()) {
        if (key == "retry" && value.is_object()) {
            for (const auto& [field, inner] : value.items()) entry.Default("retry." + field, inner);
        } else {
            entry.Default(key, value);
        }
    }

    config::Reader resolved(merged, std::string(kBundledSettingsFile) + " + " + kPluginsFile);
    const std::string unset = "is not set for backend \"" + settings.backend + "\" (set it in " +
                              kBundledSettingsFile + " or " + kPluginsFile + ")";
    const auto require_string = [&](config::Reader& reader, std::string_view key) {
        auto value = reader.MaybeString(key);  // "" is meaningful: api_key "" = no header
        if (!value) reader.Fail(key, unset);
        return *value;
    };
    const auto require_int = [&](config::Reader& reader, std::string_view key) {
        const auto value = reader.MaybeInt(key);
        if (!value) reader.Fail(key, unset);
        return static_cast<int>(*value);
    };
    const auto require_double = [&](config::Reader& reader, std::string_view key) {
        const auto value = reader.MaybeDouble(key);
        if (!value) reader.Fail(key, unset);
        return *value;
    };
    settings.api_key = require_string(resolved, "api_key");
    settings.endpoint = require_string(resolved, "endpoint");
    settings.model = require_string(resolved, "model");
    settings.request_timeout_ms = require_int(resolved, "request_timeout_ms");
    settings.budget_ms = require_int(resolved, "budget_ms");
    // A per-attempt cap above the total budget can only mean "budget wins";
    // the first attempt must not be able to outlive the loop.
    if (settings.request_timeout_ms > settings.budget_ms) {
        resolved.Fail("request_timeout_ms", "must not exceed budget_ms");
    }

    config::Reader retry = resolved.Object("retry");
    settings.max_retries = require_int(retry, "max_retries");
    settings.backoff_initial_ms = require_int(retry, "backoff_initial_ms");
    settings.backoff_max_ms = require_int(retry, "backoff_max_ms");
    settings.jitter = require_double(retry, "jitter");
    settings.retry_after_cap_ms = require_int(retry, "retry_after_cap_ms");
    retry.Finish();
    if (settings.backoff_max_ms < settings.backoff_initial_ms) {
        resolved.Fail("retry.backoff_max_ms", "must not be below retry.backoff_initial_ms");
    }

    settings.min_probability_allow = require_double(resolved, "min_probability_allow");
    settings.min_probability_deny = require_double(resolved, "min_probability_deny");
    settings.min_margin = require_double(resolved, "min_margin");

    settings.deny_message = require_string(resolved, "deny_message");
    const auto logging = resolved.MaybeBool("debug_decision_logging");
    if (!logging) resolved.Fail("debug_decision_logging", unset);
    settings.debug_decision_logging = *logging;

    // The rubric is sent verbatim, but the code acts on one answer of it: the
    // "action" choice over a fixed option set (decision.hpp). Check that
    // coupling here, so a renamed option fails at load rather than turning
    // every call into an abstain.
    const auto* questions = resolved.Raw("questions");
    if (questions == nullptr) resolved.Fail("questions", unset);
    const std::string action_path = "questions." + std::string(kActionQuestion);
    const auto action = questions->find(kActionQuestion);
    if (action == questions->end() || !action->is_object()) {
        resolved.Fail("questions", "must define \"" + std::string(kActionQuestion) +
                                       "\" -- the answer the advisor acts on");
    }
    if (action->value("type", std::string()) != "choice") {
        resolved.Fail(action_path + ".type", "must be \"choice\"");
    }
    const auto criteria = action->find("criteria");
    bool options_match = criteria != action->end() && criteria->is_object() &&
                         criteria->size() == kActionOptions.size();
    for (const std::string_view option : kActionOptions) {
        options_match = options_match && criteria->contains(option);
    }
    if (!options_match) {
        resolved.Fail(action_path + ".criteria",
                      "must have exactly the options \"allow\", \"deny\", \"review\" -- the ones "
                      "the advisor acts on");
    }
    settings.questions = *questions;
    const auto* extra = resolved.Raw("extra_questions");
    if (extra == nullptr) resolved.Fail("extra_questions", unset);
    settings.extra_questions = *extra;
    resolved.Finish();
    return settings;
}

nlohmann::json LoadBundledSettings(const std::filesystem::path& path) {
    return config::LoadJsonConfig<nlohmann::json>(
        path, kBundledSettingsFile, config::AbsencePolicy::kThrow,
        config::EmptyFilePolicy::kTreatAsMalformed,
        [](const nlohmann::json& doc) { return doc; });
}

}  // namespace minimal::decision_advisor
