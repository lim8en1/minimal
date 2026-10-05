#pragma once

// Named secrets: one place an operator keeps credentials, one reference syntax
// every config field that carries a key accepts, one resolver.
//
//   "env:NAME"              an environment variable (fixed for the process)
//   "secret:NAME"           the operator's store, <data dir>/secrets.json
//   "secret:<plugin>:NAME"  a plugin's SecretSource; the namespace is the
//                           plugin's own name, so no plugin answers for another
//   anything else           a literal key
//
// There is no implicit lookup: a config entry gets a stored secret only by
// naming it. The store keeps names, not provider names -- an embedding
// endpoint's key is a secret like any other, with no providers.json entry.
//
// secrets.json:
//   {"anthropic":  {"value": "sk-...", "hosts": ["api.anthropic.com"]},
//    "embeddings": {"value": "..."}}
// `hosts` (optional) binds where the secret may be sent: host or host:port,
// matched against the destination URL. Without it the explicit reference is
// the consent.
//
// Stored secrets are read at use time (SecretHandle::Get), so /secret set
// reaches the next request with no restart. The file is tiny and replaced by
// atomic rename, so each use reads it whole.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "minimal/common/secret_handle.hpp"

namespace minimal::config {

class ProviderConfigExtensions;

inline constexpr const char* kSecretsServiceName = "core.secrets";

struct SecretEntry {
    std::string value;
    std::vector<std::string> hosts;  // empty = unrestricted
};

// A plugin's secrets, reachable as "secret:<plugin>:NAME". Registered through
// ProviderConfigExtensions::AddSecretSource during plugin load.
class SecretSource {
public:
    virtual ~SecretSource() = default;
    // Where the values come from, for logs ("~/.local/share/foo/auth.json").
    virtual std::string Describe() const = 0;
    // The secret called `name`, or nullopt. Called per use; must not throw.
    virtual std::optional<SecretEntry> Lookup(const std::string& name) const = 0;
};

// <data dir>/secrets.json -- the file /secret writes and the resolver reads.
std::filesystem::path SecretStorePath();

// The store's document parsed into entries. Malformed entries are skipped; a
// malformed document throws std::runtime_error.
std::vector<std::pair<std::string, SecretEntry>> ParseSecretStore(const std::string& contents);

// Whether `url`'s host (and port, when the entry names one) matches one of
// `hosts`. An empty list matches everything.
bool HostAllowed(const std::vector<std::string>& hosts, const std::string& url);

// Binds config references to SecretHandles. Provided to plugins as
// kSecretsServiceName; providers.json gets it directly.
class SecretResolver {
public:
    // `sources` (nullable): plugin secret namespaces. Read at Get time, so a
    // resolver constructed before plugin load sees every source.
    explicit SecretResolver(std::filesystem::path store_path = SecretStorePath(),
                            const ProviderConfigExtensions* sources = nullptr);

    // `ref` as configured; `destination_url` is where the value will be sent
    // ("" when the consumer has no URL to check). Throws std::runtime_error
    // for a reference that can never resolve: malformed syntax, an unset env
    // variable. A stored secret that is merely absent right now binds fine
    // and fails at Get, naming the /secret set that fixes it. An empty `ref`
    // binds the unconfigured handle.
    common::SecretHandle Bind(const std::string& ref, const std::string& destination_url) const;

    const std::filesystem::path& store_path() const { return store_path_; }

private:
    std::filesystem::path store_path_;
    const ProviderConfigExtensions* sources_;
};

}  // namespace minimal::config
