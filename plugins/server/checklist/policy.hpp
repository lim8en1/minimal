#pragma once

// checklist policy: every value here is a judgment call, not a fact or a
// derivation, and states why it is what it is. See ARCHITECTURE.md
// "Policy constants".

#include <chrono>
#include <cstddef>

namespace minimal::checklist {

// One item's text and one list's length: a task list is short lines the user
// scans at a glance; past these the model is writing a document, not a list.
inline constexpr std::size_t kMaxItemTextBytes = 4000;
inline constexpr std::size_t kMaxItems = 200;

// Pushes for a burst of edits coalesce over this window: a model rewriting a
// list item by item updates the view once, still faster than the eye.
inline constexpr std::chrono::milliseconds kPushDebounce{50};

}  // namespace minimal::checklist
