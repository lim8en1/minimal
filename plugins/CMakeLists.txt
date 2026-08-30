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

function(minimal_add_plugin name)
    add_library(${name} MODULE ${ARGN})
    target_link_libraries(${name} PRIVATE minimal::server_core)
    target_compile_options(${name} PRIVATE ${MINIMAL_WARNING_FLAGS})
    set_target_properties(${name} PROPERTIES PREFIX "" OUTPUT_NAME "${name}")

    # Plugins are installed one directory deeper than the server, so the hop up to
    # the library dir is correspondingly longer.
    if(APPLE)
        set_target_properties(${name} PROPERTIES
            INSTALL_RPATH "@loader_path/../../../${CMAKE_INSTALL_LIBDIR}")
    else()
        set_target_properties(${name} PROPERTIES
            INSTALL_RPATH "$ORIGIN/../../../${CMAKE_INSTALL_LIBDIR}")
    endif()

    install(TARGETS ${name} LIBRARY DESTINATION "${MINIMAL_PLUGIN_INSTALL_DIR}")
endfunction()

add_subdirectory(server)

# plugins/client/ -- none bundled yet. Client plugins are loaded by a different
# loader, export different ABI symbols, and install to a different directory, so
# they get their own subtree and their own add-function when the first one lands.
