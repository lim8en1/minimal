#include "minimal/mcp/env_interpolation.hpp"

#include <cctype>

namespace minimal::mcp {
namespace {

bool IsIdentChar(char c, bool first) {
    if (c == '_' || std::isalpha(static_cast<unsigned char>(c))) return true;
    return !first && std::isdigit(static_cast<unsigned char>(c));
}

// Reports every `$NAME`-shaped run in `segment` (a plain-text stretch that
// contains no `${` marker) to `on_bare_var`. Bounded to the segment itself --
// a `$` at the very end of a segment with its identifier spilling into the
// next one (rare, and only possible right before a `${` marker starts) is not
// chased across the boundary; this is a diagnostic, not a parser.
void ScanBareVars(std::string_view segment, const BareVarWarning& on_bare_var) {
    if (!on_bare_var) return;
    for (std::size_t i = 0; i < segment.size(); ++i) {
        if (segment[i] != '$') continue;
        std::size_t start = i + 1;
        if (start >= segment.size() || !IsIdentChar(segment[start], /*first=*/true)) continue;
        std::size_t end = start + 1;
        while (end < segment.size() && IsIdentChar(segment[end], /*first=*/false)) ++end;
        on_bare_var(segment.substr(start, end - start));
        i = end - 1;
    }
}

}  // namespace

std::optional<std::string> InterpolateEnv(std::string_view raw, const EnvLookup& lookup,
                                          std::string* failed_marker,
                                          const BareVarWarning& on_bare_var) {
    auto fail = [failed_marker](std::string_view name) {
        if (failed_marker != nullptr) *failed_marker = std::string(name);
        return std::nullopt;
    };
    std::string out;
    out.reserve(raw.size());

    std::size_t pos = 0;
    while (pos < raw.size()) {
        const std::size_t marker = raw.find("${", pos);
        if (marker == std::string_view::npos) {
            const std::string_view tail = raw.substr(pos);
            ScanBareVars(tail, on_bare_var);
            out.append(tail);
            break;
        }
        const std::string_view literal = raw.substr(pos, marker - pos);
        ScanBareVars(literal, on_bare_var);
        out.append(literal);

        const std::size_t close = raw.find('}', marker + 2);
        if (close == std::string_view::npos) {
            return fail(raw.substr(marker + 2));  // unterminated marker: never sent as-is
        }
        const std::string_view body = raw.substr(marker + 2, close - marker - 2);

        // `NAME:-default`: an unset NAME resolves to the literal default text
        // instead of failing. Only the FIRST `:-` splits -- a default value
        // containing `:-` itself is taken verbatim, never re-split.
        std::string_view name = body;
        std::optional<std::string_view> fallback;
        if (const std::size_t sep = body.find(":-"); sep != std::string_view::npos) {
            name = body.substr(0, sep);
            fallback = body.substr(sep + 2);
        }
        if (name.empty()) {
            return fail(body);  // ${} or ${:-x} -- a typo, not an empty substitution
        }
        const std::optional<std::string> value = lookup(name);
        if (!value) {
            if (fallback) {
                out.append(*fallback);
                pos = close + 1;
                continue;
            }
            return fail(name);  // unset variable: the caller surfaces which one
        }
        out.append(*value);
        pos = close + 1;
    }
    return out;
}

}  // namespace minimal::mcp
