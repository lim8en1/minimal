// Exports only the three required symbols and none of the optional seams.
// Symbol presence is the capability declaration, so this must load cleanly and
// simply contribute nothing.
#include "minimal/plugins/plugin_abi.h"

extern "C" {
const char* minimal_plugin_abi_tag() { return minimal::common::AbiTag(); }
const char* minimal_plugin_name() { return "fixture-minimal"; }
const char* minimal_plugin_version() { return "1.0.0"; }
}
