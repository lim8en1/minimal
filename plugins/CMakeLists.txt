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
# LINKAGE, and why it is not the obvious thing:
# A plugin must NOT link minimal_server_core. It is a static library, so linking it
# into both the host and each plugin would give every plugin its own copy of every
# global -- separate registries, separate logger, separate everything. Plugins take
# the host's headers only, and resolve host symbols at dlopen time from the
# already-loaded server image. That requires the host to export its symbols
# (ENABLE_EXPORTS on minimal-server) and the plugin to tolerate undefined symbols
# at link time. Verified with nm: AbiTag/Version are undefined in each .so and
# exported from minimal-server.

function(minimal_add_plugin name)
    add_library(${name} MODULE ${ARGN})

    # Headers only -- deliberately not target_link_libraries(minimal::server_core).
    target_include_directories(${name} PRIVATE
        $<TARGET_PROPERTY:minimal_server_core,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(${name} PRIVATE
        $<TARGET_PROPERTY:minimal_common,INTERFACE_COMPILE_DEFINITIONS>)

    if(APPLE)
        target_link_options(${name} PRIVATE -undefined dynamic_lookup)
    endif()
    # ELF leaves undefined symbols in a shared object unresolved by default, so
    # Linux needs no equivalent flag.

    set_target_properties(${name} PROPERTIES PREFIX "" OUTPUT_NAME "${name}")
    install(TARGETS ${name} LIBRARY DESTINATION "${MINIMAL_PLUGIN_INSTALL_DIR}")
endfunction()

add_subdirectory(server)

# plugins/client/ -- none bundled yet. Client plugins are loaded by a different
# loader, export different ABI symbols, and install to a different directory, so
# they get their own subtree and their own add-function when the first one lands.
