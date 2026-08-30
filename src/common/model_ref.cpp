#include "minimal/common/model_ref.hpp"

namespace minimal::common {

std::optional<ModelRef> ParseModelRef(std::string_view text) {
    const auto slash = text.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    // Exactly one separator: a second means the split point is a guess.
    if (text.find('/', slash + 1) != std::string_view::npos) return std::nullopt;
    if (slash == 0 || slash + 1 == text.size()) return std::nullopt;
    return ModelRef{std::string(text.substr(0, slash)), std::string(text.substr(slash + 1))};
}

}  // namespace minimal::common
