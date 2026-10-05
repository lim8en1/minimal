#include "minimal/config/secrets.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

#include "minimal/common/config_paths.hpp"
#include "minimal/common/env.hpp"
#include "minimal/common/read_file.hpp"
#include "minimal/config/provider_config_extensions.hpp"

namespace minimal::config {
namespace {

constexpr std::string_view kEnvPrefix = "env:";
constexpr std::string_view kSecretPrefix = "secret:";

std::string ToLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

struct HostPort {
    std::string host;  // lowercased, IPv6 brackets removed
    std::string port;  // "" when neither named nor implied by the scheme
};

// "[::1]:8080", "host:443", "host", "::1" (bare IPv6: no port).
HostPort ParseHostSpec(const std::string& spec) {
    if (!spec.empty() && spec.front() == '[') {
        const auto close = spec.find(']');
        if (close == std::string::npos) return {ToLower(spec), ""};
        HostPort out{ToLower(spec.substr(1, close - 1)), ""};
        if (close + 1 < spec.size() && spec[close + 1] == ':') out.port = spec.substr(close + 2);
        return out;
    }
    const auto colon = spec.find(':');
    if (colon == std::string::npos || spec.find(':', colon + 1) != std::string::npos) {
        return {ToLower(spec), ""};
    }
    return {ToLower(spec.substr(0, colon)), spec.substr(colon + 1)};
}

// The destination's host and port; the port defaults from an http(s) scheme.
std::optional<HostPort> ParseUrlHost(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return std::nullopt;
    const std::string scheme = ToLower(url.substr(0, scheme_end));
    std::string authority = url.substr(scheme_end + 3);
    authority = authority.substr(0, authority.find_first_of("/?#"));
    if (const auto at = authority.rfind('@'); at != std::string::npos) {
        authority = authority.substr(at + 1);
    }
    HostPort out = ParseHostSpec(authority);
    if (out.host.empty()) return std::nullopt;
    if (out.port.empty()) {
        if (scheme == "https") out.port = "443";
        if (scheme == "http") out.port = "80";
    }
    return out;
}

std::string DescribeHost(const std::string& url) {
    const auto parsed = ParseUrlHost(url);
    if (!parsed) return url.empty() ? "an unchecked destination" : "'" + url + "'";
    return parsed->host + (parsed->port.empty() ? "" : ":" + parsed->port);
}

std::optional<SecretEntry> ReadStoreEntry(const std::filesystem::path& path,
                                          const std::string& name) {
    const common::ReadFileResult file = common::ReadFile(path);
    if (file.status == common::ReadFileResult::Status::kNotFound) return std::nullopt;
    if (!file.ok()) throw std::runtime_error(file.error);
    for (auto& [key, entry] : ParseSecretStore(file.contents)) {
        if (key == name) return std::move(entry);
    }
    return std::nullopt;
}

}  // namespace

std::filesystem::path SecretStorePath() { return common::DataDir() / "secrets.json"; }

std::vector<std::pair<std::string, SecretEntry>> ParseSecretStore(const std::string& contents) {
    std::vector<std::pair<std::string, SecretEntry>> out;
    if (contents.empty()) return out;
    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(contents);
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("secrets.json is malformed: ") + error.what());
    }
    if (!doc.is_object()) throw std::runtime_error("secrets.json must be an object keyed by name");
    for (const auto& [name, node] : doc.items()) {
        if (!node.is_object() || !node.contains("value") || !node.at("value").is_string()) continue;
        SecretEntry entry;
        entry.value = node.at("value").get<std::string>();
        if (node.contains("hosts") && node.at("hosts").is_array()) {
            for (const auto& host : node.at("hosts")) {
                if (host.is_string() && !host.get<std::string>().empty()) {
                    entry.hosts.push_back(host.get<std::string>());
                }
            }
        }
        out.emplace_back(name, std::move(entry));
    }
    return out;
}

bool HostAllowed(const std::vector<std::string>& hosts, const std::string& url) {
    if (hosts.empty()) return true;
    const auto destination = ParseUrlHost(url);
    if (!destination) return false;
    return std::any_of(hosts.begin(), hosts.end(), [&](const std::string& spec) {
        const HostPort allowed = ParseHostSpec(spec);
        return allowed.host == destination->host &&
               (allowed.port.empty() || allowed.port == destination->port);
    });
}

SecretResolver::SecretResolver(std::filesystem::path store_path,
                               const ProviderConfigExtensions* sources)
    : store_path_(std::move(store_path)), sources_(sources) {}

common::SecretHandle SecretResolver::Bind(const std::string& ref,
                                          const std::string& destination_url) const {
    if (ref.empty()) return {};

    if (ref.rfind(kEnvPrefix, 0) == 0) {
        const std::string name = ref.substr(kEnvPrefix.size());
        if (name.empty()) throw std::runtime_error("'" + ref + "' names no environment variable");
        const auto value = common::LookupEnv(name);
        if (!value) {
            throw std::runtime_error("'" + ref + "' names an environment variable that is unset");
        }
        common::SecretValue fixed{*value, "env " + name};
        return common::SecretHandle(ref, "env " + name, [fixed] { return fixed; });
    }

    if (ref.rfind(kSecretPrefix, 0) != 0) {
        // "word:..." reads as a reference, not a key: a typo'd scheme
        // ("secrets:openai") would otherwise go out verbatim as the key and
        // come back as a 401. A real key shaped like that is stored with
        // /secret set and named as secret:NAME.
        const auto colon = ref.find(':');
        if (colon != std::string::npos && colon > 0 &&
            std::all_of(ref.begin(), ref.begin() + static_cast<std::ptrdiff_t>(colon),
                        [](unsigned char c) { return std::isalpha(c) != 0; })) {
            // The scheme only, never the whole value: it may be a real key.
            throw std::runtime_error("unknown key reference scheme '" + ref.substr(0, colon) +
                                     ":' (expected env:NAME or secret:NAME; store a literal of "
                                     "this shape with /secret set)");
        }
        return common::SecretHandle::Literal(ref);
    }

    const std::string rest = ref.substr(kSecretPrefix.size());
    const auto colon = rest.find(':');
    const std::string plugin = colon == std::string::npos ? "" : rest.substr(0, colon);
    const std::string name = colon == std::string::npos ? rest : rest.substr(colon + 1);
    if (name.empty() || (colon != std::string::npos && plugin.empty())) {
        throw std::runtime_error("'" + ref +
                                 "' is malformed: expected secret:NAME or secret:<plugin>:NAME");
    }

    auto get = [store_path = store_path_, sources = sources_, plugin, name, ref,
                destination_url]() -> common::SecretValue {
        std::optional<SecretEntry> entry;
        std::string source;
        if (plugin.empty()) {
            entry = ReadStoreEntry(store_path, name);
            source = store_path.string();
            if (!entry) {
                throw std::runtime_error("secret '" + name + "' is not set (/secret set " + name +
                                         " <value>)");
            }
        } else {
            const SecretSource* provider =
                sources != nullptr ? sources->FindSecretSource(plugin) : nullptr;
            if (provider == nullptr) {
                throw std::runtime_error("'" + ref + "': no loaded plugin '" + plugin +
                                         "' provides secrets");
            }
            entry = provider->Lookup(name);
            source = plugin + ": " + provider->Describe();
            if (!entry) {
                throw std::runtime_error("'" + ref + "': plugin '" + plugin +
                                         "' has no secret '" + name + "'");
            }
        }
        if (entry->value.empty()) throw std::runtime_error("'" + ref + "' is empty");
        if (!HostAllowed(entry->hosts, destination_url)) {
            std::string bound;
            for (const auto& host : entry->hosts) bound += (bound.empty() ? "" : ", ") + host;
            throw std::runtime_error("'" + ref + "' is bound to hosts [" + bound +
                                     "] and may not be sent to " + DescribeHost(destination_url));
        }
        return {std::move(entry->value), std::move(source)};
    };
    return common::SecretHandle(ref, ref, std::move(get));
}

}  // namespace minimal::config
