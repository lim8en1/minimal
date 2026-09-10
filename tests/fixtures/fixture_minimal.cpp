// Exports only the three required symbols and none of the optional seams.
// Symbol presence is the capability declaration, so this must load cleanly and
// simply contribute nothing. Also the one fixture that defines those three
// symbols through MINIMAL_PLUGIN_ABI_SYMBOLS instead of by hand, so the macro
// itself is compiled and exercised by the loader tests -- a bare "return
// minimal::common::AbiTag();" call compiles too, but binds to the host's own
// copy at dlopen time (see plugin_abi.h), so nothing else in this fixture
// tree would notice the macro's compile-time expansion silently regressing.
#include "minimal/plugins/plugin_abi.h"

MINIMAL_PLUGIN_ABI_SYMBOLS("fixture-minimal")
