#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "minimal/common/secret_handle.hpp"
#include "minimal/config/secrets.hpp"

namespace minimal::tools_web {

// This plugin's own config, the "config" object on its plugins.json entry --
// {"fetch": {...}, "search": {...}}. Same shape and rules as tools-search's
// SearchSettings: every field optional, missing values keep the struct's
// defaults, and the search half is legitimately absent (see below).
struct WebSettings {
    // web_fetch.
    int fetch_timeout_ms = 15000;
    int fetch_max_redirects = 5;
    // The text returned to the model is bounded: without a model-supplied
    // max_bytes, the first `fetch_default_max_bytes` bytes; a supplied value is
    // clamped to `fetch_max_bytes`. The overflow is spilled to a project-local
    // file the `read` tool can page through -- see fetch_tool.cpp.
    int fetch_default_max_bytes = 262144;  // 256 KiB
    int fetch_max_bytes = 2097152;         // 2 MiB
    // Loopback/private/link-local hosts (including the cloud-metadata address
    // 169.254.169.254) are refused by default -- this tool takes a URL from
    // the MODEL. An operator running against a trusted internal network sets
    // this to reach it.
    bool fetch_allow_private_hosts = false;

    // web_search: ONE configurable JSON search API, not a per-vendor
    // integration. `search_api_url` is queried as `GET <url>?q=<query>&count=<n>`
    // and must return `{"results": [{"title", "url", "snippet"}, ...]}` --
    // SearXNG's own JSON API is close to this shape; a vendor whose API
    // differs needs a thin adapter in front of it. Empty `search_api_url`
    // means search is simply not configured: the tool still registers (so it
    // is discoverable and its absence is not a surprise), but reports that
    // plainly rather than failing as if misconfigured.
    std::string search_api_url;
    // A key reference, the same syntax providers.json's api_key takes
    // ("env:NAME", "secret:NAME", "secret:<plugin>:NAME", or a literal --
    // config/secrets.hpp). Sent as `Authorization: Bearer <key>`.
    std::string search_api_key_ref;
    int search_default_max_results = 5;
    int search_max_results = 20;
    int search_timeout_ms = 10000;

    // search_api_key_ref bound at parse time, read per search (a secret
    // stored later reaches the next search). Unconfigured when no key is set;
    // that is not an error (a local SearXNG instance needs none).
    common::SecretHandle search_api_key;
};

// Throws std::runtime_error if search.api_key can never resolve (malformed,
// an unset "env:" variable) -- a silently empty key fails later as a cryptic
// 401 instead of a clear startup error. Never throws for search being simply
// unconfigured (search_api_url empty). `secrets` null = the default resolver
// (the operator's store, no plugin namespaces).
WebSettings ParseWebSettings(const nlohmann::json& config,
                             const config::SecretResolver* secrets = nullptr);

}  // namespace minimal::tools_web
