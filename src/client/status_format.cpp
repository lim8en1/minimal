#include "minimal/client/status_format.hpp"

namespace minimal::client {

std::vector<StatusSegment> ParseStatusFormat(std::string_view format) {
    std::vector<StatusSegment> out;
    std::string literal;
    auto flush = [&] {
        if (!literal.empty()) {
            out.push_back(StatusSegment{false, std::move(literal)});
            literal.clear();
        }
    };

    std::size_t i = 0;
    while (i < format.size()) {
        if (format[i] != '{') {
            literal.push_back(format[i]);
            ++i;
            continue;
        }
        // Scan for the closing brace, but a NESTED '{' closes the candidacy of
        // this one first: in "{version | {tokens}" the opening brace is
        // decoration and only "tokens" is a segment. Left-to-right, no
        // brace-matching across a nested open.
        std::size_t close = i + 1;
        while (close < format.size() && format[close] != '}' && format[close] != '{') ++close;
        if (close == format.size() || format[close] == '{') {
            // Unterminated (or nested): this '{' is literal text.
            literal.push_back(format[i]);
            ++i;
            continue;
        }
        flush();
        std::string id(format.substr(i + 1, close - i - 1));
        const bool optional = !id.empty() && id.back() == '?';
        if (optional) id.pop_back();
        out.push_back(StatusSegment{true, std::move(id), optional});
        i = close + 1;
    }
    flush();
    return out;
}

}  // namespace minimal::client
