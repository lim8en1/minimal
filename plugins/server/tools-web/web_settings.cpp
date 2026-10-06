#include "web_settings.hpp"

#include <limits>
#include <stdexcept>
#include <utility>

#include "minimal/common/config_reader.hpp"
#include "minimal/common/plugin_config_label.hpp"

namespace minimal::tools_web {

WebSettings ParseWebSettings(const nlohmann::json& config,
                             const config::SecretResolver* secrets) {
    WebSettings settings;
    constexpr std::int64_t kIntMax = std::numeric_limits<int>::max();
    config::Reader root(config, common::PluginConfigLabel(common::kPluginConfigDirName, "tools-web"));
    const auto positive_int = [&](config::Reader& section, const char* key, int& target) {
        target = static_cast<int>(section.Int(key, target, 1, kIntMax));
    };

    // Live: the limits, read per call; the tool definitions quote none of
    // them (a result states the limit it applied). Restart: the URL and its
    // key (the permission model is keyed by the backend's host) and the SSRF
    // switch.
    config::Reader fetch = root.Object("fetch");
    fetch.Describe("timeout_ms", {"Hard wall-clock bound on one fetch.", config::ApplyPolicy::kLive});
    fetch.Describe("max_redirects", {"Redirect follow limit.", config::ApplyPolicy::kLive});
    fetch.Describe("default_max_bytes", {"Text returned when the model supplies no max_bytes.",
                                         config::ApplyPolicy::kLive});
    fetch.Describe("max_bytes", {"Ceiling on the model-supplied max_bytes.",
                                 config::ApplyPolicy::kLive});
    fetch.Describe("allow_private_hosts",
                   {"Allow loopback/private/link-local hosts (incl. cloud metadata) -- the URL "
                    "comes from the model. Only for a trusted internal network."});
    positive_int(fetch, "timeout_ms", settings.fetch_timeout_ms);
    positive_int(fetch, "max_redirects", settings.fetch_max_redirects);
    positive_int(fetch, "default_max_bytes", settings.fetch_default_max_bytes);
    positive_int(fetch, "max_bytes", settings.fetch_max_bytes);
    settings.fetch_allow_private_hosts =
        fetch.Bool("allow_private_hosts", settings.fetch_allow_private_hosts);
    fetch.Finish();
    // A ceiling below the default would clamp every fetch that supplied no
    // max_bytes -- the configuration contradicting itself.
    if (settings.fetch_max_bytes < settings.fetch_default_max_bytes) {
        root.Fail("fetch.max_bytes", "must not be below fetch.default_max_bytes");
    }

    config::Reader search = root.Object("search");
    search.Describe("api_url", {"Search backend, queried as GET <url>?q=...&count=...; empty = "
                                "web_search unconfigured. SearXNG: "
                                "http://localhost:8888/search?format=json"});
    search.Describe("api_key", {"Key reference sent as Authorization: Bearer; unset = no header.",
                                config::ApplyPolicy::kRestart, /*secret=*/true});
    search.Describe("default_max_results", {"Result count when the model supplies none.",
                                            config::ApplyPolicy::kLive});
    search.Describe("max_results", {"Ceiling on the result count.", config::ApplyPolicy::kLive});
    search.Describe("timeout_ms", {"Hard wall-clock bound on one search.",
                                   config::ApplyPolicy::kLive});
    // Without a backend, web_search answers "not configured": nothing else
    // under search does anything.
    for (const char* key : {"api_key", "default_max_results", "max_results", "timeout_ms"}) {
        search.RelevantWhenSet(key, "api_url");
    }
    settings.search_api_url = search.String("api_url", settings.search_api_url);
    settings.search_api_key_ref = search.String("api_key", settings.search_api_key_ref);
    positive_int(search, "default_max_results", settings.search_default_max_results);
    positive_int(search, "max_results", settings.search_max_results);
    positive_int(search, "timeout_ms", settings.search_timeout_ms);
    search.Finish();
    root.Finish();

    static const config::SecretResolver kDefaultSecrets;
    try {
        settings.search_api_key = (secrets != nullptr ? *secrets : kDefaultSecrets)
                                      .Bind(settings.search_api_key_ref, settings.search_api_url);
    } catch (const std::exception& error) {
        search.Fail("api_key", error.what());
    }
    return settings;
}

}  // namespace minimal::tools_web
