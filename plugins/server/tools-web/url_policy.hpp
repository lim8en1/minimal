#pragma once

#include <string>

namespace minimal::tools_web {

struct UrlCheck {
    bool ok = false;
    std::string reason;   // set when !ok, human-readable
    std::string host;     // the extracted host, for logging/tests (set even when !ok, when parseable)
};

// Parses `url`'s scheme and host, and -- unless `allow_private_hosts` -- RESOLVES
// the host (getaddrinfo) and rejects it if any resolved address is loopback,
// private, link-local, or unspecified. Resolving rather than string-matching
// the literal host is what defeats DNS rebinding: "https://evil.example/"
// that resolves to 169.254.169.254 (the cloud-metadata address) is caught
// exactly the way the literal IP would be. Only http/https are accepted
// regardless of allow_private_hosts.
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

}  // namespace minimal::tools_web
