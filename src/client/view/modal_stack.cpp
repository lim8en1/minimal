#include "minimal/client/view/modal_stack.hpp"

#include <algorithm>

namespace minimal::client {

bool ModalStack::AnyOpen() const {
    return std::any_of(flags_.begin(), flags_.end(), [](bool open) { return open; });
}

ftxui::Component ModalStack::Build(ftxui::Component base) {
    for (std::size_t i = 0; i < kSize; ++i) {
        if (components_[i]) base = ftxui::Modal(std::move(base), components_[i], &flags_[i]);
    }
    return base;
}

}  // namespace minimal::client
