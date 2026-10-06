# Core server library, built from one OBJECT library per module (minimal_mod_<name>,
# declared in <name>/CMakeLists.txt) and linked into ONE shared library.
#
# The per-module targets are what enforce the layering: a module's include
# directory reaches only the modules that list it in DEPS, so an include from a
# module that may not depend on it fails to compile. (scripts/check_module_deps.py
# checks the same DEPS lists at the include level, including headers that sneak
# in through a shared include path.)
#
# The result is SHARED, not STATIC, and that matters for plugins. An archive member
# is linked into an executable only if something references it, so with a static
# core a plugin calling e.g. ToolRegistry::Register would find that symbol absent
# from the host image and fail at dlopen(RTLD_NOW). A shared library gives one copy
# in memory, one set of globals, and ordinary symbol resolution -- which is what
# lets plugins link core directly instead of relying on export tricks.
function(minimal_server_module name)
    cmake_parse_arguments(PARSE_ARGV 1 MODULE "" "" "DEPS;SOURCES")
    set(target minimal_mod_${name})
    add_library(${target} OBJECT ${MODULE_SOURCES})
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(${target}
        PUBLIC
            minimal::common
            minimal::proto
            nlohmann_json::nlohmann_json
            yaml-cpp::yaml-cpp
        PRIVATE
            spdlog::spdlog
            CURL::libcurl
            SQLite3::SQLite3)
    foreach(dep IN LISTS MODULE_DEPS)
        target_link_libraries(${target} PUBLIC minimal_mod_${dep})
    endforeach()
    target_compile_options(${target} PRIVATE ${MINIMAL_WARNING_FLAGS})
    set_property(GLOBAL APPEND PROPERTY MINIMAL_SERVER_MODULES ${target})
endfunction()

add_subdirectory(llm)
add_subdirectory(net)
add_subdirectory(store)
add_subdirectory(session)
add_subdirectory(policy)
add_subdirectory(auth)
add_subdirectory(config)
add_subdirectory(sidecar)
add_subdirectory(hooks)
add_subdirectory(tools)
add_subdirectory(preset)
add_subdirectory(commands)
add_subdirectory(agent)
add_subdirectory(plugins)
add_subdirectory(rpc)
add_subdirectory(startup)

get_property(MINIMAL_SERVER_MODULE_TARGETS GLOBAL PROPERTY MINIMAL_SERVER_MODULES)
set(MINIMAL_SERVER_CORE_OBJECTS "")
foreach(module IN LISTS MINIMAL_SERVER_MODULE_TARGETS)
    list(APPEND MINIMAL_SERVER_CORE_OBJECTS $<TARGET_OBJECTS:${module}>)
endforeach()
add_library(minimal_server_core SHARED ${MINIMAL_SERVER_CORE_OBJECTS})
add_library(minimal::server_core ALIAS minimal_server_core)

target_include_directories(minimal_server_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/policy/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/sidecar/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/llm/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/net/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/config/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/plugins/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/hooks/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/preset/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/agent/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/commands/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/auth/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/rpc/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/session/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/store/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/startup/include")

# SQLite3 is PUBLIC because store's public headers (sqlite_util.hpp) include
# <sqlite3.h>, and plugins with their own database (memory) build on them.
target_link_libraries(minimal_server_core
    PUBLIC
        minimal::common
        minimal::proto
        nlohmann_json::nlohmann_json
        yaml-cpp::yaml-cpp
        SQLite3::SQLite3
    PRIVATE
        spdlog::spdlog
        CURL::libcurl)

set_target_properties(minimal_server_core PROPERTIES
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}")

# Same treatment as minimal_client_core: the core's own @rpath dependency on
# libminimal_proto resolves against this dylib's rpaths, not the executable's.
# Its other dependencies -- curl, sqlite, spdlog -- are gathered into
# lib/minimal/runtime (MINIMAL_RUNTIME_DEP_INSTALL_DIR, top-level
# CMakeLists.txt), so the second hop reaches them from lib/ where this core
# installs.
if(APPLE)
    set_property(TARGET minimal_server_core PROPERTY INSTALL_RPATH
        "@loader_path;@loader_path/${MINIMAL_LIBDIR_TO_RUNTIME_DEP_RPATH}")
else()
    set_property(TARGET minimal_server_core PROPERTY INSTALL_RPATH
        "$ORIGIN;$ORIGIN/${MINIMAL_LIBDIR_TO_RUNTIME_DEP_RPATH}")
endif()

add_executable(minimal-server main.cpp plugin_cli.cpp)
target_link_libraries(minimal-server PRIVATE minimal::server_core)

# The server loads its bundled plugins from the plugin DIRECTORY at startup,
# not through any link dependency -- so without this edge nothing orders the
# plugin builds after the host, and a partial build could produce a
# just-built server next to stale plugins. The startup sanity check treats a
# failed bundled plugin as fatal, so that pairing aborts startup outright
# (the "abi mismatch ... rebuild the plugin" failure mode). The aggregate
# target lives in plugins/CMakeLists.txt.
add_dependencies(minimal-server bundled_plugins)

# Find libminimal_server_core next to us both in the build tree and once
# installed. Plugins get the same treatment in plugins/CMakeLists.txt. The
# second hop reaches the gathered dependencies in lib/minimal/runtime -- the
# executable loads them through the core (DT_RPATH on Linux covers a
# dependency's own dependencies; macOS consults the executable's full rpath
# list for every @rpath lookup).
if(APPLE)
    set_target_properties(minimal-server PROPERTIES INSTALL_RPATH
        "@loader_path/../${CMAKE_INSTALL_LIBDIR};@loader_path/../${CMAKE_INSTALL_LIBDIR}/${MINIMAL_LIBDIR_TO_RUNTIME_DEP_RPATH}")
else()
    set_target_properties(minimal-server PROPERTIES INSTALL_RPATH
        "$ORIGIN/../${CMAKE_INSTALL_LIBDIR};$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/${MINIMAL_LIBDIR_TO_RUNTIME_DEP_RPATH}")
endif()

install(TARGETS minimal-server minimal_server_core
    RUNTIME_DEPENDENCY_SET minimal_runtime_deps
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
minimal_strip_debug_info(minimal-server "${CMAKE_INSTALL_BINDIR}")
minimal_strip_debug_info(minimal_server_core "${CMAKE_INSTALL_LIBDIR}")

target_compile_options(minimal_server_core PRIVATE ${MINIMAL_WARNING_FLAGS})
target_compile_options(minimal-server PRIVATE ${MINIMAL_WARNING_FLAGS})
