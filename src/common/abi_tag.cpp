#include "minimal/common/abi_tag.hpp"

#ifndef MINIMAL_ABI_TAG
#error "MINIMAL_ABI_TAG must be defined by the build system"
#endif
#ifndef MINIMAL_VERSION
#error "MINIMAL_VERSION must be defined by the build system"
#endif

namespace minimal::common {

const char* AbiTag() { return MINIMAL_ABI_TAG; }
const char* Version() { return MINIMAL_VERSION; }

}  // namespace minimal::common
