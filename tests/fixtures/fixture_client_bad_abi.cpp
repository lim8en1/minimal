// Reports a deliberately wrong ABI tag. Must be skipped, never executed --
// a plugin built against a different toolchain must not reach our types.
//
// The name/version functions ABORT if the loader ever calls them: the bug
// this fixture pins (the client loader ran them before the tag check, which
// the server loader had already fixed) is exactly "plugin code executed
// against an incompatible ABI", and a hard stop is the only observable a
// separate-process fixture can leave when the wrong thing happened.
#include <cstdlib>

#include "minimal/client/plugins/client_plugin_abi.h"

extern "C" {
const char* minimal_client_plugin_abi_tag() { return "definitely-not-this-toolchain"; }
const char* minimal_client_plugin_name() {
    std::abort();  // if this ever runs, the ABI gate failed open
}
const char* minimal_client_plugin_version() {
    std::abort();
}

void minimal_client_plugin_register_views(
    minimal::client::plugins::ViewRegistry&,
    const minimal::client::plugins::ClientPluginRegistrationContext&) {
    std::abort();
}
}
