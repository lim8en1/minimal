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

    config::Reader fetch = root.Object("fetch");
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
