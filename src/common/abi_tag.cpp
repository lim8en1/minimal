#include "minimal/common/abi_tag.hpp"

#ifndef MINIMAL_ABI_TAG
#error "MINIMAL_ABI_TAG must be defined by the build system"
#endif
#ifndef MINIMAL_VERSION
#error "MINIMAL_VERSION must be defined by the build system"
#endif

namespace minimal::common {

// The #ifndef guards above can never fire: src/common/CMakeLists.txt defines
// both macros unconditionally, so they are always DEFINED -- including as ""
// if the cache variable feeding them is ever cleared, or this target's compile
// definitions are ever evaluated before the root CMakeLists.txt computes
// MINIMAL_ABI_TAG. An empty tag is the case that actually matters: it would
// compare equal across every such build, silently defeating the "you forgot
// to rebuild" guard the mismatch check exists to enforce. sizeof() on a string
// literal macro is 1 (the null terminator alone) exactly when the string is
// empty, so this catches it at compile time instead.
static_assert(sizeof(MINIMAL_ABI_TAG) > 1, "MINIMAL_ABI_TAG must not be empty");
static_assert(sizeof(MINIMAL_VERSION) > 1, "MINIMAL_VERSION must not be empty");

const char* AbiTag() { return MINIMAL_ABI_TAG; }
const char* Version() { return MINIMAL_VERSION; }

}  // namespace minimal::common
