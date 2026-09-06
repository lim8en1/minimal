#include "url_policy.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <optional>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace minimal::tools_web {

namespace {

bool IsPrivateOrLocalIPv4(const in_addr& addr) {
    const std::uint32_t ip = ntohl(addr.s_addr);
    if ((ip & 0xFF000000u) == 0x7F000000u) return true;  // 127.0.0.0/8 loopback
    if ((ip & 0xFF000000u) == 0x0A000000u) return true;  // 10.0.0.0/8
    if ((ip & 0xFFF00000u) == 0xAC100000u) return true;  // 172.16.0.0/12
    if ((ip & 0xFFFF0000u) == 0xC0A80000u) return true;  // 192.168.0.0/16
    // 169.254.0.0/16 link-local -- includes the cloud-metadata address
    // 169.254.169.254, the single most common SSRF target in practice.
    if ((ip & 0xFFFF0000u) == 0xA9FE0000u) return true;
    // 100.64.0.0/10 CGNAT / shared address space (RFC 6598) -- carrier and
    // cloud internal networks route on it; a fetch to it targets
    // infrastructure the user was never meant to reach.
    if ((ip & 0xFFC00000u) == 0x64400000u) return true;
    if ((ip & 0xFF000000u) == 0x00000000u) return true;  // 0.0.0.0/8 "this network"
    return false;
}

bool IsPrivateOrLocalIPv6(const in6_addr& addr) {
    if (IN6_IS_ADDR_LOOPBACK(&addr)) return true;
    if (IN6_IS_ADDR_LINKLOCAL(&addr)) return true;
    if (IN6_IS_ADDR_SITELOCAL(&addr)) return true;  // deprecated, still worth refusing
    if (IN6_IS_ADDR_UNSPECIFIED(&addr)) return true;
    if (IN6_IS_ADDR_V4MAPPED(&addr)) {
        in_addr v4{};
        std::memcpy(&v4, &addr.s6_addr[12], sizeof(v4));
        return IsPrivateOrLocalIPv4(v4);
    }
    if ((addr.s6_addr[0] & 0xFE) == 0xFC) return true;  // fc00::/7 unique local
    // 64:ff9b::/96 -- the well-known NAT64 prefix (RFC 6052): the last 32
    // bits are an embedded IPv4 address a NAT64 gateway translates
    // literally, so a public-looking v6 literal can smuggle a private v4
    // target past a v4-only denylist.
    if (addr.s6_addr[0] == 0x00 && addr.s6_addr[1] == 0x64 && addr.s6_addr[2] == 0xff &&
        addr.s6_addr[3] == 0x9b &&
        std::all_of(addr.s6_addr + 4, addr.s6_addr + 12,
                    [](std::uint8_t b) { return b == 0; })) {
        return true;
    }
    return false;
}

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string port;  // explicit ":port" digits, empty when the URL has none
};

// Deliberately minimal: enough to split scheme/host/port out of a URL the
// tool itself only ever forwards verbatim to HttpClient (which does its own,
// complete parsing). Not a general-purpose URL parser.
std::optional<ParsedUrl> ParseUrl(const std::string& url) {
    const auto scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return std::nullopt;
    ParsedUrl parsed;
    parsed.scheme = url.substr(0, scheme_end);

    std::string rest = url.substr(scheme_end + 3);
    // Strip userinfo ("user:pass@host") before the host, if present.
    const auto slash = rest.find('/');
    if (const auto at = rest.find('@'); at != std::string::npos && (slash == std::string::npos || at < slash)) {
        rest = rest.substr(at + 1);
    }

    if (!rest.empty() && rest.front() == '[') {  // IPv6 literal, e.g. [::1]:8080
        const auto close = rest.find(']');
        if (close == std::string::npos) return std::nullopt;
        parsed.host = rest.substr(1, close - 1);
        if (close + 1 < rest.size() && rest[close + 1] == ':') {
            const auto port_end = rest.find_first_of("/?#", close + 2);
            parsed.port = rest.substr(close + 2, port_end - (close + 2));
        }
    } else {
        const auto host_end = rest.find_first_of(":/?#");
        parsed.host = rest.substr(0, host_end);
        if (host_end != std::string::npos && rest[host_end] == ':') {
            const auto port_end = rest.find_first_of("/?#", host_end + 1);
            parsed.port = rest.substr(host_end + 1, port_end - (host_end + 1));
        }
    }
    if (parsed.host.empty()) return std::nullopt;
    return parsed;
}

std::string ToLower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

}  // namespace

UrlCheck CheckUrl(const std::string& url, bool allow_private_hosts) {
    const auto parsed = ParseUrl(url);
    if (!parsed) return UrlCheck{false, "could not parse a host from the URL", ""};

    const std::string scheme = ToLower(parsed->scheme);
    if (scheme != "http" && scheme != "https") {
        return UrlCheck{false, "unsupported scheme '" + parsed->scheme + "' (only http/https)",
                        parsed->host};
    }

    if (allow_private_hosts) return UrlCheck{true, "", parsed->host};

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const int rc = getaddrinfo(parsed->host.c_str(), nullptr, &hints, &result);
    if (rc != 0) {
        return UrlCheck{false, "could not resolve host '" + parsed->host + "'", parsed->host};
    }

    bool private_hit = false;
    std::vector<std::string> resolved_addresses;
    for (const addrinfo* it = result; it != nullptr; it = it->ai_next) {
        if (it->ai_family == AF_INET) {
            const auto* sin = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
            if (IsPrivateOrLocalIPv4(sin->sin_addr)) {
                private_hit = true;
                break;
            }
            char buf[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)) != nullptr) {
                resolved_addresses.emplace_back(buf);
            }
        } else if (it->ai_family == AF_INET6) {
            const auto* sin6 = reinterpret_cast<const sockaddr_in6*>(it->ai_addr);
            if (IsPrivateOrLocalIPv6(sin6->sin6_addr)) {
                private_hit = true;
                break;
            }
            char buf[INET6_ADDRSTRLEN];
            if (inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf)) != nullptr) {
                resolved_addresses.emplace_back(buf);
            }
        }
    }
    freeaddrinfo(result);

    if (private_hit) {
        return UrlCheck{false,
                        "host '" + parsed->host +
                            "' resolves to a private, loopback, or link-local address",
                        parsed->host};
    }

    // Pin the connection to the addresses just vetted (see UrlCheck's comment
    // for the why). Entries need a numeric port: an explicit one only counts
    // if it is all digits, otherwise the scheme default -- and if the URL's
    // port is garbage, no pinning (libcurl will reject the URL itself).
    std::string port = parsed->port;
    if (port.empty()) {
        port = (scheme == "https") ? "443" : "80";
    } else if (port.find_first_not_of("0123456789") != std::string::npos) {
        return UrlCheck{true, "", parsed->host};
    }
    UrlCheck out{true, "", parsed->host};
    // A host that is itself an IPv6 literal needs the same bracketing in the
    // entry's HOST field as in its ADDR field, or the colon-split entry is
    // ambiguous.
    const std::string host_part =
        parsed->host.find(':') != std::string::npos ? "[" + parsed->host + "]" : parsed->host;
    out.resolve_entries.reserve(resolved_addresses.size());
    for (const auto& address : resolved_addresses) {
        // CURLOPT_RESOLVE wants IPv6 addresses bracketed.
        const std::string addr_part =
            address.find(':') != std::string::npos ? "[" + address + "]" : address;
        out.resolve_entries.push_back(host_part + ":" + port + ":" + addr_part);
    }
    return out;
}

}  // namespace minimal::tools_web
