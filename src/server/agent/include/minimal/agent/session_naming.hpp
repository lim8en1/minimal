#pragma once

// The free default-naming rule (Phase 5 step 9): what a session's tab label
// becomes when nobody named it explicitly. Pure and DB-free -- Session calls
// this from the raw text of a user turn, then hands the result to
// SessionDatabase::SetDefaultName; the database itself no longer knows what
// a name looks like.

#include <string>
#include <string_view>

namespace minimal::agent {

// Collapses whitespace runs to a single space, drops leading/trailing, and
// truncates at a code-point boundary (never mid-UTF-8-sequence) so a
// multi-line, arbitrarily long prompt becomes a usable tab label.
std::string DefaultSessionName(std::string_view text);

}  // namespace minimal::agent
