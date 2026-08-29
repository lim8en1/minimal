#pragma once

namespace minimal::common {

// Identifies the toolchain this binary was built with.
//
// Plugins are source-compatible, not binary-compatible: they are rebuilt with the
// harness. This tag is therefore a "you forgot to rebuild" guard, not a stable-ABI
// promise. Both plugin loaders (server and client) compare their own tag against
// the one a candidate plugin reports and refuse to load on mismatch.
const char* AbiTag();

const char* Version();

}  // namespace minimal::common
