// minimal-server entry point.
//
// Startup ordering is load-bearing and must not be rearranged casually:
//   config load -> plugin load -> sanity check -> serve
// Providers and tools are plugin-registered, so "is there at least one usable
// provider?" cannot be answered before plugins are in. See ARCHITECTURE.md.

#include <cstdio>

#include "minimal/common/abi_tag.hpp"
#include "minimal/common/config_paths.hpp"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    // TODO(agent-loop): config load, plugin load, sanity check, then serve
    // SessionService over a unix socket. Skeleton only -- see ARCHITECTURE.md.
    std::printf("minimal-server %s\n", minimal::common::Version());
    std::printf("  abi tag:    %s\n", minimal::common::AbiTag());
    std::printf("  config dir: %s\n", minimal::common::ConfigDir().c_str());
    std::printf("  data dir:   %s\n", minimal::common::DataDir().c_str());
    return 0;
}
