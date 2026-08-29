#pragma once

// The server plugin ABI. This is the header a plugin author includes.
//
// COMPATIBILITY MODEL: source, not binary. Plugins are compiled from the same
// tree with the same pinned toolchain as the harness, so an interface change is
// a compile error you fix by rebuilding -- not a silent break. The ABI tag below
// is therefore a "you forgot to rebuild" guard, not a stability promise, which
// is what makes it safe to ship core functionality as plugins while these
// interfaces are still evolving.
//
// The header has two halves, and both are frozen surface:
//
//   1. EXTENSION POINTS a plugin implements -- the extern "C" symbols below.
//      Symbol presence *is* the capability declaration; the loader dlsym's each
//      optional name and calls it only if found, so there is no manifest to
//      drift out of sync with the code.
//
//   2. UTILITIES a plugin consumes -- included below. A plugin tool must call
//      path_confinement::confine rather than reimplementing path safety, which
//      is precisely why these are ABI surface too, just inbound.

#include "minimal/common/abi_tag.hpp"

// --- Half 2: utilities plugins consume -------------------------------------
#include "minimal/llm/http_client.hpp"
#include "minimal/llm/provider.hpp"
#include "minimal/llm/provider_factory_registry.hpp"
#include "minimal/llm/sse_parser.hpp"
#include "minimal/tools/path_confinement.hpp"
#include "minimal/tools/tool.hpp"
#include "minimal/tools/tool_registry.hpp"

// --- Half 1: extension points plugins implement -----------------------------

extern "C" {

// REQUIRED. A plugin missing any of these is skipped with MissingRequiredSymbol.
//
// Implement abi_tag by returning minimal::common::AbiTag() -- returning a
// literal defeats the guard, since the point is to report the toolchain this
// binary was actually built with.
const char* minimal_plugin_abi_tag();
const char* minimal_plugin_name();
const char* minimal_plugin_version();

// OPTIONAL, additive: many plugins may each contribute tools.
void minimal_plugin_register_tools(minimal::tools::ToolRegistry&);

// OPTIONAL, additive: contributes provider kinds addressable from providers.json.
void minimal_plugin_register_provider_factories(minimal::llm::ProviderFactoryRegistry&);

// Not yet wired -- declared nowhere until the subsystems they register into
// exist, so that this ABI is not frozen before it has consumers:
//   minimal_plugin_register_commands(CommandRegistry&)
//   minimal_plugin_register_session_observer(SessionObserverRegistry&)
//   minimal_plugin_register_mention_resolver(MentionResolverRegistry&)
//   minimal_plugin_register_auth_provider(AuthProviderRegistry&)

}  // extern "C"
