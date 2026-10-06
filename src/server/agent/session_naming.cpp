#include "minimal/agent/session_naming.hpp"
#include "minimal/common/utf8.hpp"

#include <cctype>

namespace minimal::agent {
namespace {

// ASCII-only whitespace is what keeps this from splitting a UTF-8 sequence:
// isspace() is locale-dependent above ASCII -- under a Latin-1 locale 0xA0 is
// a space, so an unguarded call would delete bytes out of the middle of a
// character. The server never calls setlocale today; this makes that not
// matter.
std::string CollapseWhitespace(std::string_view text) {
    std::string out;
    bool pending_space = false;
    for (const char c : text) {
        if (common::IsAsciiSpace(c)) {
            pending_space = true;
        } else {
            if (!out.empty() && pending_space) out += ' ';
            pending_space = false;
            out += c;
        }
    }
    return out;
}

constexpr int kDefaultNameMaxChars = 60;

}  // namespace

std::string DefaultSessionName(std::string_view text) {
    return common::TruncateCodePoints(CollapseWhitespace(text), kDefaultNameMaxChars);
}

}  // namespace minimal::agent
