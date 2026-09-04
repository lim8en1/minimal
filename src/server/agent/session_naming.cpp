#include "minimal/agent/session_naming.hpp"

#include <cctype>

namespace minimal::agent {
namespace {

// The c < 0x80 guard is what keeps this from splitting a UTF-8 sequence:
// continuation bytes are all > 127, and isspace() is locale-dependent above
// ASCII -- under a Latin-1 locale 0xA0 is a space, so an unguarded call would
// delete bytes out of the middle of a character. The server never calls
// setlocale today; this makes that not matter.
std::string CollapseWhitespace(std::string_view text) {
    std::string out;
    bool pending_space = false;
    for (const unsigned char c : text) {
        if (c < 0x80 && std::isspace(c)) {
            pending_space = true;
        } else {
            if (!out.empty() && pending_space) out += ' ';
            pending_space = false;
            out += static_cast<char>(c);
        }
    }
    return out;
}

// Cuts at a code-point boundary, never mid-sequence. A stray continuation byte
// counts as one character rather than desynchronizing the scan.
std::string TruncateToCodePoints(std::string_view s, int max_chars) {
    std::size_t chars = 0;
    std::size_t i = 0;
    while (i < s.size() && chars < static_cast<std::size_t>(max_chars)) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len;
        if (c < 0x80) len = 1;
        else if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else len = 1;
        if (i + len > s.size()) len = s.size() - i;
        i += len;
        ++chars;
    }
    return std::string(s.substr(0, i));
}

constexpr int kDefaultNameMaxChars = 60;

}  // namespace

std::string DefaultSessionName(std::string_view text) {
    return TruncateToCodePoints(CollapseWhitespace(text), kDefaultNameMaxChars);
}

}  // namespace minimal::agent
