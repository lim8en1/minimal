#pragma once

// THE SERVICE CONTRACT tools-bash ships for its per-session shell state.
//
// This header is the "typed at the edges" half of the cross-plugin service
// channel (docs/EXTENSIBILITY.md §2.2): a consumer includes it, passes
// kBashStateServiceName to ServiceRegistry::Find, and static_casts the result
// here. The name is the only runtime coupling; this header is the contract,
// under the same source-compatibility model as every other plugin surface.
//
// The interface is ABSTRACT deliberately. The concrete store's symbols live
// inside tools-bash.so, which is loaded RTLD_LOCAL -- a consumer calling them
// by symbol would fail to resolve at dlopen(RTLD_NOW). Virtual dispatch needs
// no symbols: the call goes through the object's own vtable, which points back
// into the provider. A consumer must therefore NEVER link tools-bash or cast
// to BashSessionStateStore -- only to this interface.
//
// Narrow by design: Erase/Reset are what an outside consumer may ask. Anything
// finer (Snapshot/SetState) would hand out write access to state whose
// wholesale-adoption rules belong to the owning plugin.
//
// ABSENT PROVIDER: Find returns null when tools-bash is disabled or the
// service is missing (e.g. a stale third-party build predating
// register_services). That is a runtime condition, not an error -- degrade
// with a clear message, never crash and never fail startup.

#include <filesystem>
#include <string>

namespace minimal::tools_bash {

// Qualified by the providing plugin, matching the "plugin.original_name"
// convention everywhere else. The name is addressable by code only -- services
// are not user-aliasable and never surface in permission keys.
inline constexpr char kBashStateServiceName[] = "tools-bash.session_state";

class BashSessionStateService {
public:
    virtual ~BashSessionStateService() = default;

    // Forget a session's cwd and env entirely; the next bash command re-seeds
    // from project_root and a fresh process environment. This is exactly what
    // `restart: true` does and what OnSessionEnded does at teardown.
    virtual void Erase(const std::string& session_id) = 0;

    // Re-seed NOW from `project_root` + the process environment, forgetting any
    // adopted state without dropping the entry.
    virtual void Reset(const std::string& session_id,
                       const std::filesystem::path& project_root) = 0;
};

}  // namespace minimal::tools_bash
