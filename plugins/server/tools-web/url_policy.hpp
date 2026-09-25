#pragma once

#include <optional>
#include <string>
#include <vector>

namespace minimal::tools_web {

struct UrlCheck {
    bool ok = false;
    std::string reason;   // set when !ok, human-readable
    std::string host;     // the extracted host, for logging/tests (set even when !ok, when parseable)
    // Empty unless ok AND the check actually resolved the host (the
    // allow_private_hosts short-circuit skips resolution). Each entry is a
    // libcurl CURLOPT_RESOLVE "host:port:addr" string for one address this
    // check vetted: the caller passes them through to HttpClient so the
    // connection goes to exactly those addresses -- without the pin, libcurl
    // re-resolves the host at connect time, and an attacker controlling DNS
    // could answer the two lookups differently (the classic rebinding TOCTOU:
    // vetted address for check time, 169.254.169.254 for connect time).
    std::vector<std::string> resolve_entries = {};
};

// Parses `url`'s scheme and host, and -- unless `allow_private_hosts` --
// RESOLVES the host (getaddrinfo) and rejects it if any resolved address is
// loopback, private, link-local, CGNAT/shared, or unspecified. Resolving
// rather than string-matching the literal host catches "https://evil.example/"
// that resolves to 169.254.169.254 (the cloud-metadata address) exactly the
// way the literal IP would be. On its own that is only half the rebinding
// story: the addresses returned here are also handed back as pin entries so
// the later fetch CONNECTS to them instead of re-resolving -- check-time and
// connect-time resolution are therefore the same lookup, and cannot disagree.
// Only http/https are accepted regardless of allow_private_hosts.
//
// This tool takes a URL from the MODEL, so the check runs before every fetch
// AND before every redirect hop -- a same-origin-looking redirect chain that
// ends inside a private network must not slip through after the first hop
// passed.
//
// Known limitation: getaddrinfo is a blocking call with no timeout
// parameter of its own. A wedged resolver blocks the calling thread (the
// tool pool, not the session worker -- see ARCHITECTURE.md "Tool
// dispatch") for as long as the OS's own resolver timeout takes. Accepted
// for cut 1, the same class of tradeoff the tree already makes for other
// library calls with no cancellable variant (cpp-mcp's blocking client).
UrlCheck CheckUrl(const std::string& url, bool allow_private_hosts);

// The host a URL names, lowercased, parsed exactly the way CheckUrl parses it
// (userinfo stripped, IPv6 brackets removed) -- WITHOUT resolving anything,
// so it is safe on the permission path. nullopt when there is none.
std::optional<std::string> HostOf(const std::string& url);

}  // namespace minimal::tools_web
