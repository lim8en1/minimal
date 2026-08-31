#include "minimal/mcp/env_interpolation.hpp"

namespace minimal::mcp {

std::optional<std::string> InterpolateEnv(std::string_view raw, const EnvLookup& lookup,
                                          std::string* failed_marker) {
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
            out.append(raw.substr(pos));
            break;
        }
        out.append(raw.substr(pos, marker - pos));

        const std::size_t close = raw.find('}', marker + 2);
        if (close == std::string_view::npos) {
            return fail(raw.substr(marker + 2));  // unterminated marker: never sent as-is
        }
        const std::string_view name = raw.substr(marker + 2, close - marker - 2);
        if (name.empty()) {
            return fail(name);  // ${} -- a typo, not an empty substitution
        }
        const std::optional<std::string> value = lookup(name);
        if (!value) {
            return fail(name);  // unset variable: the caller surfaces which one
        }
        out.append(*value);
        pos = close + 1;
    }
    return out;
}

}  // namespace minimal::mcp
