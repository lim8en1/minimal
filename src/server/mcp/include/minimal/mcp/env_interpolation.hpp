#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace minimal::mcp {

// Lookup used for ${ENV_VAR} interpolation. A thin indirection rather than
// ::getenv directly so tests can inject an environment.
using EnvLookup = std::function<std::optional<std::string>(std::string_view name)>;

// Resolves `${NAME}` markers against `lookup`. Everything else -- `$` not
// followed by `{`, empty variables, text around markers -- passes through
// literally.
//
// std::nullopt means a marker could not be resolved: the variable is unset,
// the name is empty, or a `${` never closes. When `failed_marker` is given it
// receives the offending NAME, so a diagnostic can name the variable without
// ever printing a resolved value. The caller treats failure as a connection
// error naming the entry -- silently sending an empty string (or the raw
// marker) to a server would leak the mistake into a place the user cannot
// see it. Resolved values are secrets in practice (API keys); the caller
// must never log them.
std::optional<std::string> InterpolateEnv(std::string_view raw, const EnvLookup& lookup,
                                          std::string* failed_marker = nullptr);

}  // namespace minimal::mcp
