# 3.28 for FetchContent_Declare(EXCLUDE_FROM_ALL), used to keep cpp-mcp's bundled
# examples out of our build.
cmake_minimum_required(VERSION 3.28)

# The vcpkg toolchain must be set before project(). Pass it on the command line
# (-DCMAKE_TOOLCHAIN_FILE=...) or use CMakePresets.json, which reads $VCPKG_ROOT.
project(minimal
    VERSION 0.1.0
    DESCRIPTION "Local-first coding-agent harness"
    LANGUAGES C CXX)  # C for the vendored tree-sitter grammar sources

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(MINIMAL_BUILD_CLIENT "Build the FTXUI TUI client" ON)
option(MINIMAL_BUILD_TESTS  "Build the test suite"       ON)

# Applied to our own targets only -- add_compile_options() would also hit
# FetchContent'd dependencies, whose warnings we can neither fix nor act on.
# Measured before enabling: the first-party tree was already clean apart from two
# sign-compare lines, so this costs nothing to turn on and something to leave off.
set(MINIMAL_WARNING_FLAGS -Wall -Wextra)

# --- Platform gate -----------------------------------------------------------
# Windows is out of scope until 2.x: the bash tool is posix_spawn/setrlimit/killpg
# and plugin loading is dlopen. WSL is the interim answer. See ARCHITECTURE.md.
if(WIN32)
    message(FATAL_ERROR
        "minimal does not support native Windows (see ARCHITECTURE.md -- Deferred). Use WSL.")
endif()

include(GNUInstallDirs)
include(FetchContent)

# Where bundled plugins are installed, and where the server looks for them
# relative to its own executable. Both halves must agree.
#
# .../plugins/server, a SIBLING of the client tree below, not its parent: the
# server's bundled scan (plugin_discovery.cpp) recurses into MINIMAL_PLUGIN_DIR
# to support the build tree's one-subdirectory-per-plugin layout, so an
# installed server whose plugin dir CONTAINED plugins/client/ would walk into
# it, find client plugins missing the server's required ABI symbols, and abort
# startup with them reported Fatal (bundled provenance). Only the build tree
# (where the two are already siblings under build/plugins/) escaped this,
# which is why no test caught it.
# A build tree configured before the server/client split above cached the
# old, pre-split default (no /server suffix). A plain reconfigure of that
# tree would otherwise keep serving the stale value forever -- CACHE STRING
# only takes the new default when nothing is already cached -- reproducing
# the exact bundled-scan-walks-into-client-plugins startup abort this split
# exists to fix. Force it back onto the new default in that one case; a
# value the operator set to anything else is left untouched.
if(DEFINED MINIMAL_PLUGIN_INSTALL_DIR AND
   MINIMAL_PLUGIN_INSTALL_DIR STREQUAL "${CMAKE_INSTALL_LIBDIR}/minimal/plugins")
    set(MINIMAL_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/minimal/plugins/server"
        CACHE STRING "Install location for bundled server plugins" FORCE)
endif()
set(MINIMAL_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/minimal/plugins/server"
    CACHE STRING "Install location for bundled server plugins")

# The client plugin tree, separate from the server's for the same reason the
# loaders are separate: different ABI symbols, different discovery file. The
# client resolves it relative to its own executable (bundled_assets).
set(MINIMAL_CLIENT_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_LIBDIR}/minimal/plugins/client"
    CACHE STRING "Install location for bundled client plugins")

# Bundled + user plugin construction helpers (minimal_add_plugin,
# minimal_add_client_plugin, minimal_add_user_plugin). Included here so the
# functions are visible to every add_subdirectory below, including the
# user-plugins/ tree registered at the bottom of this file.
include(cmake/MinimalPlugins.cmake)

# bundled/ is copied into the USER'S CONFIG DIR at install time, not installed
# under the prefix and read from there. Everything in it -- prompts, presets,
# settings -- is a file the user owns and edits; a copy sitting in the install
# tree would either shadow theirs or be dead weight. So there is no
# MINIMAL_PROMPT_INSTALL_DIR and no runtime lookup: one copy, in the config dir,
# and that is the one that runs. Never overwrites. See cmake/SeedConfig.cmake.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SeedConfig.cmake"
               "${CMAKE_CURRENT_BINARY_DIR}/SeedConfig.cmake" @ONLY)
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/SeedConfig.cmake")

# --- Plugin ABI tag ----------------------------------------------------------
# Plugins are source-compatible, not binary-compatible: they are rebuilt with the
# harness. This tag is the "you forgot to rebuild" guard, not a compatibility
# promise. Host and plugin bake the same value; a mismatch refuses to load.
#
# The toolchain fields alone do not catch a stale-but-same-compiler .so: two
# builds of this tree at different commits, same compiler, produce identical
# tags and the stale plugin loads and reads the wrong struct/vtable offsets.
# The git revision closes that gap for the common case (a real ./configure &&
# make build after a pull) -- it does not catch uncommitted local edits
# rebuilt without a commit, which is why plugin_loader.cpp additionally
# compares minimal_plugin_version() against the host's own version.
find_package(Git QUIET)
set(MINIMAL_GIT_REVISION "nogit")
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE MINIMAL_GIT_REVISION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE MINIMAL_GIT_REVISION_RESULT)
    if(NOT MINIMAL_GIT_REVISION_RESULT EQUAL 0 OR MINIMAL_GIT_REVISION STREQUAL "")
        # NOT a shared "unknown": two builds from different sources would
        # both tag "unknown", and a stale plugin from one tree would load
        # into the other. A configure timestamp makes every build tree
        # distinct -- the fail-closed direction (plugins rebuild, always).
        string(TIMESTAMP MINIMAL_GIT_REVISION "nogit-%Y%m%d%H%M%S")
    endif()
    # A checkout/commit changes .git/HEAD (and, on a branch, the ref file it
    # points at) without touching any CMakeLists.txt -- without this, ninja
    # would happily rebuild against a stale cached MINIMAL_GIT_REVISION.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${CMAKE_CURRENT_SOURCE_DIR}/.git/HEAD")
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --git-path HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE MINIMAL_GIT_HEAD_PATH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} symbolic-ref -q HEAD
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            OUTPUT_VARIABLE MINIMAL_GIT_SYMBOLIC_REF
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT MINIMAL_GIT_SYMBOLIC_REF STREQUAL "")
            execute_process(
                COMMAND ${GIT_EXECUTABLE} rev-parse --git-path "${MINIMAL_GIT_SYMBOLIC_REF}"
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE MINIMAL_GIT_REF_PATH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
            if(EXISTS "${MINIMAL_GIT_REF_PATH}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                             "${MINIMAL_GIT_REF_PATH}")
            endif()
        endif()
    endif()
endif()
set(MINIMAL_ABI_TAG
    "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-cxx${CMAKE_CXX_STANDARD}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-${CMAKE_BUILD_TYPE}-${MINIMAL_GIT_REVISION}")
message(STATUS "minimal plugin ABI tag: ${MINIMAL_ABI_TAG}")

# --- Dependencies ------------------------------------------------------------
find_package(protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

# tree-sitter core is a BASE dependency: the tools-bash plugin's command
# scanner uses it, and bundled plugins are always built (the client's future
# highlighting is a second consumer, not the first). Two config names because
# vcpkg exports `unofficial-tree-sitter` while brew exports `tree-sitter`;
# normalized behind one interface target so consumers never see the difference.
find_package(unofficial-tree-sitter CONFIG QUIET)
if(unofficial-tree-sitter_FOUND)
    add_library(minimal_tree_sitter INTERFACE)
    target_link_libraries(minimal_tree_sitter INTERFACE unofficial::tree-sitter::tree-sitter)
else()
    find_package(tree-sitter CONFIG REQUIRED)
    add_library(minimal_tree_sitter INTERFACE)
    target_link_libraries(minimal_tree_sitter INTERFACE tree-sitter::tree-sitter)
endif()

# RE2 backs the tools-search plugin's regex matching. The point of the
# dependency is the LINEAR-TIME guarantee: grep patterns are model-authored,
# and a backtracking engine would let `(a+)+b` wedge the session worker thread
# -- the exact hang bash's timeout_ms exists to bound, now unreachable.
find_package(re2 CONFIG REQUIRED)
add_library(minimal_re2 INTERFACE)
target_link_libraries(minimal_re2 INTERFACE re2::re2)

if(MINIMAL_BUILD_CLIENT)
    find_package(ftxui CONFIG REQUIRED)
    find_package(md4c CONFIG REQUIRED)   # exports md4c::md4c (verified: vcpkg + brew agree)
endif()

if(MINIMAL_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
    include(CTest)
    include(Catch)
endif()

# The only dependency not in the vcpkg registry (verified, not assumed -- even
# tree-sitter is packaged). Vendored directly.
#
# Pinned to a SHA, not a branch: cpp-mcp publishes no tags or releases, so `main`
# is the only moving alternative and would silently change what every contributor
# builds. EXCLUDE_FROM_ALL because its CMakeLists unconditionally adds an
# `examples` subdirectory we have no interest in compiling.
FetchContent_Declare(cpp_mcp
    GIT_REPOSITORY https://github.com/hkr04/cpp-mcp.git
    GIT_TAG        13a8bef5f1885b10b8d6038c4b319b7024744fa5  # 2026-08-16
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(cpp_mcp)
# Exports a plain, un-namespaced static target: `mcp`.
#
# Patched in place, idempotently (existing build trees are already patched):
# the stdio child-pid accessor lets the harness kill a wedged stdio child on
# teardown. cmake/ApplyCppMcpPatches.cmake fails loudly on a half-applied or
# diverged tree.
include(cmake/ApplyCppMcpPatches.cmake)

# The second vendored dependency, and the first tree-sitter GRAMMAR: the
# registry packages the core runtime but not the per-language parsers (see
# ARCHITECTURE.md -- Dependency management). Grammars ship a generated
# parser.c, so vendoring means compiling two C files, not running the
# tree-sitter CLI/Node toolchain.
#
# Pinned to a tag: unlike cpp-mcp, this repo publishes releases. SOURCE_SUBDIR
# points at a directory that does not exist, which stops MakeAvailable from
# add_subdirectory-ing the grammar's own CMakeLists -- that file is written for
# standalone distribution (BUILD_SHARED_LIBS ON, a tree-sitter-CLI regeneration
# rule) and we want neither. We compile the two C files ourselves below.
FetchContent_Declare(tree_sitter_bash
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-bash.git
    GIT_TAG        v0.25.1  # 2026-08; ABI 14/15, loads into tree-sitter core >= 0.24
    SOURCE_SUBDIR  cmake/no-upstream-build)
FetchContent_MakeAvailable(tree_sitter_bash)

# No MINIMAL_WARNING_FLAGS here, deliberately: this is third-party generated C
# (a ~10 MB parser table), compiled with default flags for the same reason
# cpp-mcp is excluded from our warning set. PIC because it links into plugin
# MODULEs.
add_library(minimal_tree_sitter_bash STATIC
    "${tree_sitter_bash_SOURCE_DIR}/src/parser.c"
    "${tree_sitter_bash_SOURCE_DIR}/src/scanner.c")
target_include_directories(minimal_tree_sitter_bash
    PUBLIC "${tree_sitter_bash_SOURCE_DIR}/src")
set_target_properties(minimal_tree_sitter_bash PROPERTIES
    C_STANDARD 11
    POSITION_INDEPENDENT_CODE ON)

add_subdirectory(proto)
add_subdirectory(src/common)
add_subdirectory(src/server)
if(MINIMAL_BUILD_CLIENT)
    add_subdirectory(src/client)
endif()
# Bundled plugin implementations. A top-level peer of src/, not part of it --
# they are artifacts the harness loads, not harness code. See plugins/CMakeLists.txt.
add_subdirectory(plugins)

if(MINIMAL_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# --- User plugins -------------------------------------------------------------
#
# user-plugins/{server,client}/<name> -- one plugin directory per entry: a
# real directory, a symlink into your own plugin repo, or a git clone. The
# directory is gitignored: this is machine-local wiring, not harness content.
#
# The server/client SPLIT is the subtree the entry sits in, never anything the
# plugin itself declares -- a plugin's CMakeLists.txt is the same one-liner on
# both sides (minimal_add_user_plugin), and a repo holding both kinds
# registers one symlink per plugin. Missing dir or empty glob: fine; new
# entries are picked up by the next build (CONFIGURE_DEPENDS re-globs).
#
# After add_subdirectory(tests) so a user plugin's own tests can link
# minimal::test_support when it exists.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/user-plugins")
    foreach(_kind IN ITEMS server client)
        file(GLOB _user_plugin_entries LIST_DIRECTORIES true CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/user-plugins/${_kind}/*")
        if(_kind STREQUAL "client" AND NOT MINIMAL_BUILD_CLIENT AND _user_plugin_entries)
            message(STATUS
                "minimal: MINIMAL_BUILD_CLIENT=OFF; skipping user client plugins "
                "under user-plugins/client/")
        elseif(_user_plugin_entries)
            foreach(_entry IN LISTS _user_plugin_entries)
                if(NOT EXISTS "${_entry}/CMakeLists.txt")
                    continue()  # a stray file, or a repo root that is not a plugin
                endif()
                cmake_path(GET _entry FILENAME _entry_name)
                set(MINIMAL_USER_PLUGIN_KIND "${_kind}")
                add_subdirectory("${_entry}" "user-plugins/${_kind}/${_entry_name}")
                unset(MINIMAL_USER_PLUGIN_KIND)
            endforeach()
        endif()
        unset(_user_plugin_entries)
    endforeach()
endif()
