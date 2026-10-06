#include "cost_settings.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>

#include "minimal/common/clock.hpp"
#include "minimal/common/config_reader.hpp"
#include "minimal/common/plugin_config_label.hpp"

namespace minimal::cost_tracker {
namespace {

constexpr RateFormat kFormats[] = {RateFormat::kLiteLlm, RateFormat::kOpenRouter,
                                   RateFormat::kLiteLlmProxy};

// A name is a storage key suffix and a log label: keep it to a portable,
// unambiguous alphabet.
bool ValidSourceName(const std::string& name) {
    return !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.';
    });
}

nlohmann::json SourcesAsJson(const std::vector<RateSourceSettings>& sources) {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& source : sources) {
        list.push_back({{"name", source.name},
                        {"format", RateFormatName(source.format)},
                        {"url", source.url}});
    }
    return list;
}

std::vector<RateSourceSettings> ParseRateSources(config::Reader& root,
                                                 const nlohmann::json& list,
                                                 const config::SecretResolver& secrets) {
    if (!list.is_array()) root.Fail("rate_sources", "must be a list of rate sources");
    std::vector<RateSourceSettings> sources;
    std::set<std::string> names;
    for (std::size_t index = 0; index < list.size(); ++index) {
        const std::string segment = "rate_sources[" + std::to_string(index) + "]";
        const auto& node = list[index];
        if (!node.is_object()) root.Fail(segment, "must be an object");
        config::Reader entry = root.Child(node, segment);
        RateSourceSettings source;
        const auto format =
            entry.MaybeEnum("format", {"litellm", "openrouter", "litellm-proxy"});
        if (!format) entry.Fail("format", "is required");
        for (const RateFormat candidate : kFormats) {
            if (*format == RateFormatName(candidate)) source.format = candidate;
        }
        source.name = entry.String("name", *format, /*allow_empty=*/false);
        if (!ValidSourceName(source.name)) {
            entry.Fail("name", "may contain only letters, digits, '-', '_' and '.'");
        }
        if (!names.insert(source.name).second) {
            entry.Fail("name", "\"" + source.name + "\" names an earlier source too; " +
                                   "give one of them a distinct name");
        }
        source.url = entry.RequireString("url");
        if (source.url.empty()) entry.Fail("url", "must not be empty");
        source.api_key_ref = entry.String("api_key", {});
        entry.Finish();
        try {
            source.api_key = secrets.Bind(source.api_key_ref, source.url);
        } catch (const std::exception& error) {
            entry.Fail("api_key", error.what());
        }
        sources.push_back(std::move(source));
    }
    return sources;
}

}  // namespace

std::vector<RateSourceSettings> DefaultRateSources() {
    std::vector<RateSourceSettings> sources(2);
    sources[0].name = "openrouter";
    sources[0].format = RateFormat::kOpenRouter;
    sources[0].url = "https://openrouter.ai/api/v1/models";
    sources[1].name = "litellm";
    sources[1].format = RateFormat::kLiteLlm;
    sources[1].url =
        "https://raw.githubusercontent.com/BerriAI/litellm/main/"
        "model_prices_and_context_window.json";
    return sources;
}

std::vector<std::string> CostSettings::RateSourceNames() const {
    std::vector<std::string> names;
    names.reserve(rate_sources.size());
    for (const auto& source : rate_sources) names.push_back(source.name);
    return names;
}

CostSettings ParseCostSettings(const nlohmann::json& config,
                               const config::SecretResolver* secrets) {
    CostSettings settings;
    config::Reader reader(config, common::PluginConfigLabel(common::kPluginConfigDirName, "cost-tracker"));
    reader.Describe("rate_sources",
                    {"Pricing snapshots to fetch, highest priority first: [{\"format\": "
                     "\"litellm\" | \"openrouter\" | \"litellm-proxy\", \"url\", \"name\" "
                     "(defaults to format; unique), \"api_key\" (a key reference such as "
                     "\"secret:NAME\", sent as Authorization: Bearer)}]. [] fetches nothing."});
    reader.Default("rate_sources", SourcesAsJson(settings.rate_sources));
    reader.Describe("rates_fetch_timeout_ms", {"Bound on one pricing fetch."});
    reader.Describe("rates_refresh_days",
                    {"Snapshot age that triggers a refetch at startup; 0 = every start."});
    // Live: rates (the price lookup is per report). Restart: the fetch
    // settings (held by the refresher and the RPC service; the source list
    // also fixes the rate table's tiers) and rates_refresh_days, which only a
    // startup consults.
    reader.Describe("rates", {"Per-model rate overrides, keyed by model id: {\"input\", "
                              "\"output\", \"cache_read\"} per million tokens. Costs are priced "
                              "when reported, so a change reprices past usage too.",
                              config::ApplyPolicy::kLive});
    reader.Default("rates", nlohmann::json::object());
    static const config::SecretResolver kDefaultSecrets;
    if (const auto* list = reader.Raw("rate_sources")) {
        settings.rate_sources =
            ParseRateSources(reader, *list, secrets != nullptr ? *secrets : kDefaultSecrets);
    }
    settings.rates_fetch_timeout_ms = static_cast<int>(reader.Int(
        "rates_fetch_timeout_ms", settings.rates_fetch_timeout_ms, 1,
        std::min<std::int64_t>(std::numeric_limits<int>::max(), common::kMaxDeadline.count())));
    // 0 is meaningful ("always refresh").
    settings.rates_refresh_days = static_cast<int>(
        reader.Int("rates_refresh_days", settings.rates_refresh_days, 0, std::numeric_limits<int>::max()));
    if (auto rates = reader.MaybeObject("rates")) settings.rates = rates->node();
    reader.Finish();
    return settings;
}

}  // namespace minimal::cost_tracker
