# Plugin construction helpers -- the BUNDLED pair and the USER twin.
#
# One module so the two deployment semantics sit side by side and diff
# cleanly. Bundled plugins install into the plugin dir of the install prefix
# (provenance "bundled": default enabled, failure fatal). User plugins deploy
# at BUILD time into the config dir's user plugin dirs (provenance "user":
# default disabled, failure warns) -- provenance is decided by discovery dir,
# which is why minimal_add_user_plugin deliberately has NO install() rule:
# installing into MINIMAL_PLUGIN_INSTALL_DIR would silently flip a user
# plugin to bundled provenance.
#
# Included once from the top-level CMakeLists; the functions are then visible
# everywhere below, including add_subdirectory'd user plugin trees under
# user-plugins/{server,client}/ (see the user-plugins stanza there).

include_guard(GLOBAL)

set(MINIMAL_DEPLOY_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/DeployUserPlugin.cmake")

# RPATH from each bundled plugin's install location back to the shared
# library directory holding the core it links against, computed rather than a
# hardcoded "../../../../" hop count: that assumes CMAKE_INSTALL_LIBDIR is a
# single path component, which GNUInstallDirs violates on Debian/Ubuntu
# multiarch (lib/x86_64-linux-gnu) and any other multi-segment libdir --
# file(RELATIVE_PATH) stays correct regardless of how many components either
# side has.
function(_minimal_plugin_rpath_to_libdir install_dir_var out_var)
    if(IS_ABSOLUTE "${${install_dir_var}}")
        set(_from "${${install_dir_var}}")
    else()
        set(_from "${CMAKE_INSTALL_PREFIX}/${${install_dir_var}}")
    endif()
    file(RELATIVE_PATH _rel "${_from}" "${CMAKE_INSTALL_FULL_LIBDIR}")
    set(${out_var} "${_rel}" PARENT_SCOPE)
endfunction()

_minimal_plugin_rpath_to_libdir(MINIMAL_PLUGIN_INSTALL_DIR MINIMAL_PLUGIN_RPATH_TO_LIBDIR)
_minimal_plugin_rpath_to_libdir(MINIMAL_CLIENT_PLUGIN_INSTALL_DIR
                                 MINIMAL_CLIENT_PLUGIN_RPATH_TO_LIBDIR)

# --- Bundled -----------------------------------------------------------------

function(minimal_add_plugin name)
    add_library(${name} MODULE ${ARGN})
    target_link_libraries(${name} PRIVATE minimal::server_core)
    target_compile_options(${name} PRIVATE ${MINIMAL_WARNING_FLAGS})
    set_target_properties(${name} PROPERTIES PREFIX "" OUTPUT_NAME "${name}")

    if(APPLE)
        set_target_properties(${name} PROPERTIES
            INSTALL_RPATH "@loader_path/${MINIMAL_PLUGIN_RPATH_TO_LIBDIR}")
    else()
        set_target_properties(${name} PROPERTIES
            INSTALL_RPATH "$ORIGIN/${MINIMAL_PLUGIN_RPATH_TO_LIBDIR}")
    endif()

    install(TARGETS ${name} LIBRARY DESTINATION "${MINIMAL_PLUGIN_INSTALL_DIR}")
endfunction()

# The client twin: MODULE, links the SHARED minimal::client_core (one copy in
# memory, ordinary symbol resolution -- see the linkage history in
# plugins/CMakeLists.txt), installs into the client plugin tree -- a SIBLING of
# the server's above (plugins/client vs plugins/server), same depth, same
# RPATH hop count.
function(minimal_add_client_plugin name)
    add_library(${name} MODULE ${ARGN})
    target_link_libraries(${name} PRIVATE minimal::client_core)
    target_compile_options(${name} PRIVATE ${MINIMAL_WARNING_FLAGS})
    set_target_properties(${name} PROPERTIES PREFIX "" OUTPUT_NAME "${name}")

    if(APPLE)
        set_target_properties(${name} PROPERTIES
            INSTALL_RPATH "@loader_path/${MINIMAL_CLIENT_PLUGIN_RPATH_TO_LIBDIR}")
    else()
        set_target_properties(${name} PROPERTIES
            INSTALL_RPATH "$ORIGIN/${MINIMAL_CLIENT_PLUGIN_RPATH_TO_LIBDIR}")
    endif()

    install(TARGETS ${name} LIBRARY DESTINATION "${MINIMAL_CLIENT_PLUGIN_INSTALL_DIR}")
endfunction()

# --- User --------------------------------------------------------------------

# Where user plugins deploy: the CONFIG DIR (the function appends plugins/ or
# client-plugins/ per kind). Resolved once at configure time, in the same
# order common::ConfigDir() (src/common/config_paths.cpp) and SeedConfig.cmake
# use. Unlike the runtime, a RELATIVE XDG_CONFIG_HOME is rejected here too --
# the runtime rejects it, so deploying there would build plugins the server
# would never look for. The cache variable is also the test seam.
if(NOT DEFINED MINIMAL_USER_PLUGIN_DEPLOY_DIR)
    set(_mup_config "")
    if(DEFINED ENV{MINIMAL_CONFIG_DIR} AND NOT "$ENV{MINIMAL_CONFIG_DIR}" STREQUAL "")
        set(_mup_config "$ENV{MINIMAL_CONFIG_DIR}")
    elseif(DEFINED ENV{XDG_CONFIG_HOME} AND NOT "$ENV{XDG_CONFIG_HOME}" STREQUAL "")
        cmake_path(ABSOLUTE_PATH _mup_xdg "$ENV{XDG_CONFIG_HOME}" NORMALIZE)
        if(IS_ABSOLUTE "${_mup_xdg}")
            set(_mup_config "${_mup_xdg}/minimal")
        endif()
        unset(_mup_xdg)
    endif()
    if(_mup_config STREQUAL "")
        if(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
            set(_mup_config "$ENV{HOME}/.config/minimal")
        endif()
    endif()
    set(MINIMAL_USER_PLUGIN_DEPLOY_DIR "${_mup_config}"
        CACHE PATH "Config dir user plugins deploy into (plugins/, client-plugins/)")
    unset(_mup_config)
endif()

# The USER twin of the bundled pair. One function for both kinds: the kind is
# decided by WHICH user-plugins/ subtree the registration came from
# (MINIMAL_USER_PLUGIN_KIND, set by the top-level stanza before
# add_subdirectory) -- so a plugin's CMakeLists.txt is the same one-liner on
# either side, and a plugin repo holding both server and client plugins
# registers one directory per plugin, in the right subtree.
#
# Deployment is a POST_BUILD copy into <config>/plugins/ (server) or
# <config>/client-plugins/ (client) -- automatic, so a rebuilt plugin is never
# left stale against the harness the ABI tag was baked from. It OVERWRITES the
# previous build and NEVER prunes: the user plugin dir is also the documented
# drop location for by-hand installs, so the deploy step must not delete
# files it did not produce. Enablement stays a manual plugins.json edit;
# user plugins default disabled, and that is a trust boundary, not friction.
function(minimal_add_user_plugin name)
    cmake_parse_arguments(PARSE_ARGV 1 mup "" "DEPLOY_DIR" "")
    if(NOT DEFINED MINIMAL_USER_PLUGIN_KIND)
        message(FATAL_ERROR
            "minimal_add_user_plugin(${name}): MINIMAL_USER_PLUGIN_KIND is not set; "
            "register user plugins only through the user-plugins/{server,client} tree "
            "(or set the kind explicitly in tests)")
    endif()
    if(MINIMAL_USER_PLUGIN_KIND STREQUAL "server")
        set(_core minimal::server_core)
        set(_deploy_subdir "plugins")
    elseif(MINIMAL_USER_PLUGIN_KIND STREQUAL "client")
        if(NOT TARGET minimal::client_core)
            message(FATAL_ERROR
                "minimal_add_user_plugin(${name}): kind is 'client' but the client core "
                "does not exist (MINIMAL_BUILD_CLIENT=OFF)")
        endif()
        set(_core minimal::client_core)
        set(_deploy_subdir "client-plugins")
    else()
        message(FATAL_ERROR
            "minimal_add_user_plugin(${name}): unknown kind '${MINIMAL_USER_PLUGIN_KIND}' "
            "(expected 'server' or 'client')")
    endif()

    add_library(${name} MODULE ${mup_UNPARSED_ARGUMENTS})
    target_link_libraries(${name} PRIVATE ${_core})
    target_compile_options(${name} PRIVATE ${MINIMAL_WARNING_FLAGS})
    set_target_properties(${name} PROPERTIES PREFIX "" OUTPUT_NAME "${name}")

    # Deployed files live in the CONFIG DIR, so neither the bundled plugins'
    # relative @loader_path hop nor the staged build-tree layout applies.
    # Absolute install libdir covers the installed harness; BUILD_RPATH covers
    # running the build-tree server. In practice the host has the core already
    # loaded at dlopen time (it links it too), so these are fallbacks.
    set_target_properties(${name} PROPERTIES
        BUILD_RPATH "${CMAKE_BINARY_DIR}/src/server;${CMAKE_BINARY_DIR}/src/client"
        INSTALL_RPATH "${CMAKE_INSTALL_FULL_LIBDIR}")

    if(mup_DEPLOY_DIR)
        set(_deploy_dir "${mup_DEPLOY_DIR}")
    else()
        if(NOT MINIMAL_USER_PLUGIN_DEPLOY_DIR)
            message(FATAL_ERROR
                "minimal_add_user_plugin(${name}): cannot resolve the config dir for "
                "deployment (no MINIMAL_CONFIG_DIR, XDG_CONFIG_HOME, or HOME); "
                "set MINIMAL_USER_PLUGIN_DEPLOY_DIR explicitly")
        endif()
        set(_deploy_dir "${MINIMAL_USER_PLUGIN_DEPLOY_DIR}/${_deploy_subdir}")
    endif()

    add_custom_command(TARGET ${name} POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_FILE=$<TARGET_FILE:${name}>"
                "-DDEPLOY_DIR=${_deploy_dir}"
                -P "${MINIMAL_DEPLOY_SCRIPT}"
        VERBATIM
        COMMENT "Deploying ${name} -> ${_deploy_dir}")
    message(STATUS "minimal: user plugin '${name}' deploys to ${_deploy_dir}")
endfunction()
