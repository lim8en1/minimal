#pragma once

// HTTP status codes and classes (RFC 9110 section 15), named once: every
// client in the tree -- providers, MCP, web tools, catalogs -- classifies
// responses through these instead of comparing against bare numbers.

namespace minimal::common {

// NOLINTBEGIN(readability-magic-numbers) -- RFC 9110 status codes.
inline constexpr int kHttpBadRequest = 400;
inline constexpr int kHttpUnauthorized = 401;
inline constexpr int kHttpForbidden = 403;
inline constexpr int kHttpRequestTimeout = 408;
inline constexpr int kHttpContentTooLarge = 413;
inline constexpr int kHttpTooManyRequests = 429;
inline constexpr int kHttpInternalServerError = 500;
inline constexpr int kHttpNotImplemented = 501;

constexpr bool IsSuccessStatus(int status) { return status >= 200 && status < 300; }
constexpr bool IsRedirectStatus(int status) { return status >= 300 && status < 400; }
constexpr bool IsServerErrorStatus(int status) { return status >= 500 && status < 600; }
// NOLINTEND(readability-magic-numbers)

}  // namespace minimal::common
