// opencode-compat -- reads opencode's config so an existing opencode setup
// works without copying anything. Core knows only its own config shapes; this
// plugin is the one place that knows opencode's:
//
//   register_provider_config_extensions
//     - a providers.json entry translator: opencode's provider block
//       (npm, options.baseURL/apiKey/headers, models.<id>.limit/options/name)
//       becomes core's shape before core parses it;
//     - a credential source: opencode's auth.json, asked after core's own
//       <data dir>/auth.json for entries that opted into
//       "credentials": "auth-file".
//
// Disabled, opencode-shaped providers.json entries fail core's parse with a
// named error, and opencode's auth.json is never read.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "minimal/common/config_paths.hpp"
#include "minimal/common/logger.hpp"
#include "minimal/common/read_file.hpp"
#include "minimal/config/provider_config_extensions.hpp"
#include "minimal/plugins/plugin_abi.h"

#include "opencode_providers.hpp"

namespace {

class OpencodeProviderTranslator final : public minimal::config::ProviderEntryTranslator {
public:
    void Translate(nlohmann::json& entry, const std::string& where) const override {
        entry = minimal::opencode_compat::TranslateOpencodeProvider(entry, where);
    }
};

// opencode's own credentials store: Windows uses APPDATA, POSIX the
// XDG-ish path opencode documents. Missing is the normal state; malformed
// warns and yields nothing.
class OpencodeAuthSource final : public minimal::config::CredentialSource {
public:
    std::string Describe() const override { return Path().string(); }

    std::optional<std::string> Lookup(const std::string& provider_name) const override {
        const std::filesystem::path path = Path();
        if (path.empty()) return std::nullopt;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec) return std::nullopt;
        const auto file = minimal::common::ReadFile(path);
        if (!file.ok()) return std::nullopt;
        try {
            return minimal::opencode_compat::LookupOpencodeKey(nlohmann::json::parse(file.contents),
                                                               provider_name);
        } catch (const std::exception& error) {
            minimal::common::DefaultLogger().warn(
                "opencode-compat: {} is malformed and was ignored: {}", path.string(),
                error.what());
            return std::nullopt;
        }
    }

private:
    static std::filesystem::path Path() {
        const auto home = minimal::common::LookupEnv("HOME");
        if (!home) return {};
        return std::filesystem::path(*home) / ".local" / "share" / "opencode" / "auth.json";
    }
};

}  // namespace

extern "C" {

MINIMAL_PLUGIN_ABI_SYMBOLS("opencode-compat")

void minimal_plugin_register_provider_config_extensions(
    minimal::config::ProviderConfigExtensions& extensions,
    const minimal::plugins::PluginRegistrationContext&) {
    extensions.AddTranslator(std::make_shared<OpencodeProviderTranslator>());
    extensions.AddCredentialSource(std::make_shared<OpencodeAuthSource>());
}

}  // extern "C"
