#include "minimal/common/model_ref.hpp"

#include <cctype>

namespace minimal::common {
namespace {

std::string_view Trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end &&
           std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

}  // namespace

std::optional<ModelRef> ParseModelRef(std::string_view text) {
    // Trimmed BEFORE splitting: a ref copied from a doc or config with
    // trailing whitespace (" gpt-4o ") produced provider " gpt-4o" (or a
    // trailing-space model id) that matched no catalog entry.
    text = Trim(text);
    const auto slash = text.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    // Exactly one separator: a second means the split point is a guess.
    if (text.find('/', slash + 1) != std::string_view::npos) return std::nullopt;
    if (slash == 0 || slash + 1 == text.size()) return std::nullopt;
    return ModelRef{std::string(Trim(text.substr(0, slash))),
                    std::string(Trim(text.substr(slash + 1)))};
}

std::optional<ModelRef> SplitModelRef(std::string_view text) {
    text = Trim(text);
    const auto slash = text.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 == text.size()) {
        return std::nullopt;
    }
    return ModelRef{std::string(Trim(text.substr(0, slash))),
                    std::string(Trim(text.substr(slash + 1)))};
}

}  // namespace minimal::common
