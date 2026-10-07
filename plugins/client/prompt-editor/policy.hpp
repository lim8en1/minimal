#pragma once

// prompt-editor policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>

namespace minimal::client::prompt_editor {

// @-mention suggestions are asked for after this pause in typing: one query
// per word, not per keystroke.
inline constexpr std::chrono::milliseconds kSuggestDebounce{120};
// Undo: steps kept, and keystrokes closer than the gap coalesce into one step.
inline constexpr std::size_t kMaxUndoSteps = 100;
inline constexpr std::chrono::milliseconds kUndoCoalesceGap = std::chrono::seconds(1);
// The prompt grows to half the terminal, but never below this many rows.
inline constexpr std::size_t kMinPromptRows = 4;
// The suggestion list under the prompt scrolls past this many rows.
inline constexpr int kSuggestionListMaxHeight = 8;

}  // namespace minimal::client::prompt_editor
