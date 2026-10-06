#pragma once

// tool-ask policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <cstddef>

namespace minimal::tool_ask {

// One ask call: a few questions, a few options each -- a decision, not a
// survey. Past these the model is asked to narrow it down.
inline constexpr std::size_t kMaxQuestions = 4;
inline constexpr std::size_t kMaxOptions = 5;

// Byte caps on the model-authored strings. The header renders as a title and
// labels come back verbatim as answer keys, so both stay short; the question
// and descriptions wrap and only need a bound.
inline constexpr std::size_t kMaxHeaderBytes = 40;
inline constexpr std::size_t kMaxQuestionBytes = 1000;
inline constexpr std::size_t kMaxLabelBytes = 200;
inline constexpr std::size_t kMaxDescriptionBytes = 500;
// How much of the question stands in for a missing header in the result.
inline constexpr std::size_t kMaxQuestionEcho = 60;

}  // namespace minimal::tool_ask
