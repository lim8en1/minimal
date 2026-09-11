# Bundled plugin IMPLEMENTATIONS -- things the harness loads.
#
# Deliberately a top-level peer of src/, not a subdirectory of it: these are not
# part of the server or client binary. They are separate .so artifacts that happen
# to ship with us, categorically identical to a third-party plugin and differing
# only in who publishes them. The plugin HOST (loader, registries, ABI header)
# lives in src/server/plugins and src/client/plugins -- that machinery *is* harness
# code. Two different things, hence two different places.
#
# Provenance matters at load time: bundled plugins default to enabled, user plugins
# (dropped into the config dir) default to disabled. A failed *bundled* plugin is a
# fatal sanity-check result -- losing tools-fs or provider-openai is a useless
# harness, not a degraded one.
#
# LINKAGE:
# Plugins link minimal::server_core normally, because it is a SHARED library --
# one copy in memory, one set of globals, ordinary symbol resolution.
#
# This was not always so, and the history is worth keeping: while core was STATIC,
# plugins deliberately did NOT link it, because a static library linked into both
# the host and each plugin would give every plugin its own copy of every global --
# separate registries, separate logger. That reasoning was correct about static
# libraries, but it left a latent failure that only appears once something actually
# calls dlopen: an archive member is pulled into an executable only if referenced,
# so a plugin calling a core function the server itself never calls would find that
# symbol missing from the host image and fail at dlopen(RTLD_NOW). Making core
# shared dissolves both problems at once rather than working around either.
#
# The add_* functions (bundled pair here, user twin with its deploy-on-build
# semantics) live in cmake/MinimalPlugins.cmake, included from the top-level
# CMakeLists, so user-plugin trees registered under user-plugins/{server,client}
# can reach them too.

add_subdirectory(server)

# plugins/client/ -- the logo plugin is the first bundled client plugin (the
# ASCII-banner start view; deliberately the thinnest possible real plugin).
# E5 proved the host with test fixtures (tests/fixtures/client-*), which use
# minimal_add_client_fixture_plugin. The subtree exists so a bundled client
# plugin lands here the same way a server plugin lands in plugins/server/.
add_subdirectory(client)

# The aggregate both host binaries and the tests hang off: every target the
# add_* functions registered above. Bundled plugins are loaded at RUNTIME by
# directory, so without this target nothing orders their builds after (or at
# all alongside) the host binaries -- a partial build (`--target minimal_tests`,
# say) then pairs a fresh host with stale plugins, the exact state the ABI tag
# was baked to catch: the client loader refuses the plugin; the server's
# startup sanity check aborts outright. Consumers that load or dlopen plugins
# (minimal-server, minimal_tests) depend on this target, so "rebuilt the host"
# always means "rebuilt the plugins it will load".
get_property(_bundled_plugin_targets GLOBAL PROPERTY MINIMAL_BUNDLED_PLUGIN_TARGETS)
add_custom_target(bundled_plugins DEPENDS ${_bundled_plugin_targets})
