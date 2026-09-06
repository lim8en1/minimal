#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace minimal::mcp {

// Lookup used for ${ENV_VAR} interpolation. A thin indirection rather than
// ::getenv directly so tests can inject an environment.
using EnvLookup = std::function<std::optional<std::string>(std::string_view name)>;

// Fired for a bare `$NAME` reference (not followed by `{`) that looks like a
// variable name (`[A-Za-z_][A-Za-z0-9_]*`) -- never for an incidental `$` in
// unrelated text (a price, a shell-unrelated symbol). Diagnostic only: the
// passthrough behavior below is unchanged either way.
using BareVarWarning = std::function<void(std::string_view name)>;

// Resolves `${NAME}` and `${NAME:-default}` markers against `lookup`.
// Everything else -- `$` not followed by `{`, text around markers -- passes
// through literally (a bare `$NAME`-shaped reference fires `on_bare_var` when
// given, since that shape is a plausible copy-paste of shell syntax this
// interpolator does not expand).
//
// std::nullopt means a marker could not be resolved: the variable (the part
// before `:-`, if present) is unset with no default, the name is empty (a
// bare `${}` or `${:-x}` is a typo, not an empty substitution), a `${` never
// closes, or the variable is set-but-EMPTY -- common::LookupEnv treats
// set-but-empty as unset, so `${VAR:-default}` is the way to spell "empty is
// legitimate". When `failed_marker` is given it receives the offending
// NAME, so a diagnostic can name the variable without ever printing a
// resolved value. The caller treats failure as a connection error naming the
// entry -- silently sending an empty string (or the raw marker) to a server
// would leak the mistake into a place the user cannot see it. Resolved
// values are secrets in practice (API keys); the caller must never log them.
std::optional<std::string> InterpolateEnv(std::string_view raw, const EnvLookup& lookup,
                                          std::string* failed_marker = nullptr,
                                          const BareVarWarning& on_bare_var = nullptr);

}  // namespace minimal::mcp
