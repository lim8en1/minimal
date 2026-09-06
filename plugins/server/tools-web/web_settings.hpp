#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

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
    // Same api_key_env convention as providers.json (providers_config.cpp):
    // the key is resolved from the environment once, at parse time, and sent
    // as `Authorization: Bearer <key>` when non-empty.
    std::string search_api_key_env;
    int search_default_max_results = 5;
    int search_max_results = 20;
    int search_timeout_ms = 10000;

    // Resolved from search_api_key_env at parse time -- see ParseWebSettings.
    // Empty when search_api_key_env is unset; that is not an error (a local
    // SearXNG instance needs no key, the same "a local server needs none"
    // case providers.json's ResolveApiKey makes).
    std::string search_api_key;
};

// Throws std::runtime_error if search_api_key_env names an environment
// variable that is unset -- the same fail-loud-at-startup posture
// providers.json's ResolveApiKey takes, and for the same reason: a silently
// empty key fails later as a cryptic 401 instead of a clear startup error.
// Never throws for search being simply unconfigured (search_api_url empty).
WebSettings ParseWebSettings(const nlohmann::json& config);

}  // namespace minimal::tools_web
