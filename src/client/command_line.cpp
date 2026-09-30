#include "minimal/client/command_line.hpp"

#include <cctype>
#include <filesystem>
#include <system_error>

#include "minimal/common/logger.hpp"

namespace minimal::client {

// Splits "/name arg1 arg2" into its parts. Client-side because this is the side
// holding the text a human typed; the wire carries the result, so the server
// never re-splits a command line. Declared in the header: the TUI's prompt
// editor sends commands through the same split the headless flag uses.
CommandInvocationText SplitCommand(std::string_view text) {
    CommandInvocationText out;
    if (text.empty() || text.front() != '/') return out;  // no-op: nothing to split
    std::size_t i = 1;  // past the '/'
    while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    out.name = std::string(text.substr(1, i - 1));

    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    out.raw = std::string(text.substr(i));

    // Whitespace-separated, no quoting. Every bundled command takes at most one
    // simple word; `raw` is there for anything that needs the rest untouched,
    // which is the honest split until a command actually needs quoting.
    std::size_t start = i;
    while (start < text.size()) {
        std::size_t end = start;
        while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
        if (end > start) out.positional.emplace_back(text.substr(start, end - start));
        start = end;
        while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
    }
    return out;
}

std::string CurrentProjectRootOrEmpty() {
    std::error_code ec;
    auto dir = std::filesystem::current_path(ec);
    if (ec) {
        common::DefaultLogger().warn(
            "current_path() failed: {} -- project scoping disabled for this call",
            ec.message());
        return {};
    }
    return dir.string();
}

}  // namespace minimal::client
